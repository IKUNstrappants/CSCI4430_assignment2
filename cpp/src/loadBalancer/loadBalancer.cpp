#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <utility>
#include <functional> 
#include <fstream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "../common/LoadBalancerProtocol.h"
#include "../common/server.cpp"
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

using namespace std;

static const size_t MAX_MESSAGE_SIZE = 256;

struct Link {
    int destination;
    int cost;
};

struct Node {
    string type;
    string ip;
};


class LoadBalancer_rr{

public:

    vector< pair<string, uint16_t> > server_lists;
    int CurrentId = 0;

    LoadBalancer_rr(string &filename){
        ifstream file(filename);
        string line;
        int num_servers;

        getline(file, line);
        sscanf(line.c_str(),"NUM_SERVERS: %d", &num_servers);
        for(int i = 0; i < num_servers; i++){
            string ip;
            uint16_t port;
            getline(file,line);

            istringstream stream(line);
            stream >> ip >> port;
            server_lists.push_back({ip, port});
        }
    }

    LoadBalancerResponse response(LoadBalancerRequest &request){
        struct in_addr addr;
        addr.s_addr = request.client_addr;
        const char *ip_addr = inet_ntoa(addr);
        // string ip_addr = in_addr_t_to_str(ntohl(request.client_addr));
        spdlog::info("Received request for client {} with request ID {}", ip_addr, ntohs(request.request_id));
        LoadBalancerResponse response;
        
        response.request_id = request.request_id;
        response.videoserver_addr = inet_addr(server_lists[CurrentId].first.c_str());
        response.videoserver_port = htons(server_lists[CurrentId].second);
        spdlog::info("Responded to request ID {} with server {}:{}", ntohs(request.request_id), server_lists[CurrentId].first, server_lists[CurrentId].second);

        CurrentId = (CurrentId + 1) % server_lists.size();

        return response;
    }

};


class LoadBalance_geo{
public:
    vector<Node> nodes;
    vector< vector<Link> >graph;

    void changeToGraph(string &filename){
        ifstream file(filename);
        int numNodes, numLinks;
        string line;

        // get nodes
        getline(file, line);
        sscanf(line.c_str(),"NUM_NODES: %d",&numNodes);

        nodes.resize(numNodes);
        graph.resize(numNodes);

        string nodetype, ip_addr;
        for(int i = 0; i < numNodes; i++){
            getline(file, line);
            istringstream stream(line); 
            stream >> nodetype >> ip_addr;
            nodes[i] = {nodetype, ip_addr};
            
        }

        //get links
        getline(file, line);
        sscanf(line.c_str(),"NUM_LINKS: %d",&numLinks);

        for (int i = 0; i < numLinks; i++){
            getline(file, line);
            int origin, dest, cost;
            istringstream stream(line);
            stream >> origin >> dest >> cost;
            Link link = {dest, cost};
            graph[origin].push_back(link);

        }

        
    }

    LoadBalance_geo(string &filename){
        changeToGraph(filename);
    }

    // return the clientId if valid
    int findClosestServer(string &client){
        vector<int> distance(nodes.size(), numeric_limits<int>::max());

        int clientId = findClientId(client);
        //printf("client id is %d\n\n",clientId);
        if (clientId == -1){
            return -1;
        }

        distance[clientId] = 0;
        priority_queue< pair<int, int>, vector< pair<int, int> >, greater< pair<int, int> > > pq;
        pq.push({0, clientId});

        while(!pq.empty()){
            int node = pq.top().second;
            int dist = pq.top().first;
            pq.pop();
            
            // if visited
            if(dist > distance[node]){
                continue;
            }
            
            for( auto &link : graph[node] ){
                int dest = link.destination;
                int cost = link.cost;
                // printf("for node %d, neighbor %d cost %d",node,dest,cost);
                if ((cost + dist) < distance[dest]){
                    distance[dest] = cost+dist;
                    pq.push({distance[dest], dest});
                }
            }
        }

        /*
        for(int i = 0; i < nodes.size(); i++){
            printf("%s, %d\n",nodes[i].type.c_str(), distance[i]);
        }*/
        
        int minCost = numeric_limits<int>::max();
        int closestServer = -1 ;
        for(int i = 0; i < nodes.size(); i++){
            if(nodes[i].type == "SERVER" && distance[i] < minCost){
                minCost = distance[i];
                closestServer = i;
            }
        }

        if(closestServer == -1){
            return -1;
        }
        return closestServer;
    }


    int findClientId(string &clientId){
        for(int i = 0; i < nodes.size(); i++){
            if (nodes[i].type == "CLIENT" && nodes[i].ip == clientId){
                return i;
            }
        }
        spdlog::info("no found1");
        return -1;
    }


    LoadBalancerResponse response(LoadBalancerRequest &request){
        struct in_addr addr;
        addr.s_addr = request.client_addr;
        const char *ip_addr = inet_ntoa(addr);
    
        spdlog::info("Received request for client {} with request ID {}", ip_addr, ntohs(request.request_id));
        LoadBalancerResponse response;
        
        string addr_str(ip_addr);
         
        int nodeId = findClosestServer(addr_str);

        if (nodeId == -1){
            spdlog::info("Failed to fulfill request ID {}", ntohs(request.request_id));
            response.request_id = request.request_id;
            response.videoserver_addr = 0;
            response.videoserver_port = UINT16_MAX;
            return response;
        }

        string dest_addr = nodes[nodeId].ip;
        // printf("dest %s\n",dest_addr.c_str());

        response.request_id = request.request_id;
        response.videoserver_addr = inet_addr(dest_addr.c_str());
        response.videoserver_port = htons(8000);
        spdlog::info("Responded to request ID {} with server {}:8000", ntohs(request.request_id), dest_addr);

        return response;
    }


};


int main(int argc, const char **argv){
    cxxopts::Options options("loadBalancer", "load balancer");

    options.add_options()
        ("p, port", "Port of load balancer", cxxopts::value<int>())
        ("g, geo", "Run in geo mode", cxxopts::value<bool>()->default_value("false"))
        ("r, rr", "Run in round-robin mode", cxxopts::value<bool>()->default_value("false"))
        ("s, servers", "Path to file containing server info", cxxopts::value<string>());

    
    auto result = options.parse(argc, argv);

    int port = result["port"].as<int>();
    spdlog::info("port is {}",port);
    if (port < 1024 || port > 65535) {
        exit(1);
    }

    bool geoMode = result["geo"].as<bool>();
    bool rrMode = result["rr"].as<bool>();
    if (!(geoMode ^ rrMode)){
        exit(1);
    }

    if (!(result.unmatched().empty())){
        exit(1);
    }
    string serverFile = result["servers"].as<string>();


    unique_ptr<LoadBalancer_rr> loadBalancerRR;
    unique_ptr<LoadBalance_geo> loadBalancerGeo;

    if (geoMode) {
        loadBalancerGeo = make_unique<LoadBalance_geo>(serverFile);
    } else {
        loadBalancerRR = make_unique<LoadBalancer_rr>(serverFile);
    }

    // receive from proxy
    int sockfd = run_server(port, 10);
    // spdlog::info("sockfd {}",sockfd);

    while(true){

    int connectionfd = accept(sockfd, 0, 0);
	if (connectionfd == -1) {
		perror("Error accepting connection");
		return -1;
	}
    else{
        spdlog::info("received!");
    }
	
    LoadBalancerRequest request;
    char buffer[sizeof(LoadBalancerRequest)];
    ssize_t recvd = 0;

    while (recvd < sizeof(LoadBalancerRequest)) {
        ssize_t rval = recv(connectionfd, buffer + recvd, sizeof(LoadBalancerRequest) - recvd, 0);
        if (rval <= 0) {
            close(connectionfd);
            return -1;
        }
        recvd += rval;
    }

    memcpy(&request, buffer, sizeof(LoadBalancerRequest));

    LoadBalancerResponse response_loader;
    if (geoMode){
        response_loader = loadBalancerGeo->response(request);
    }

    else{
        response_loader = loadBalancerRR->response(request);

    }

    if(response_loader.videoserver_port == UINT16_MAX){
        close(connectionfd);
        continue;
    }

    send(connectionfd, &response_loader,sizeof(response_loader),0);
	
	close(connectionfd);
    }

}