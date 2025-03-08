#include <arpa/inet.h> //close
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h> //strlen
#include <string>
#include <sys/socket.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <sys/types.h>
#include <unistd.h> //close
#include <spdlog/spdlog.h>
#include <iostream>
#include <unordered_map>
#include <map>
#include <pugixml.hpp>
#include <regex>
#include <cxxopts.hpp>
#include "LoadBalancerProtocol.h"


#define MAXCLIENTS 30
#define BUFFER_SIZE 1024

int get_server_socket(struct sockaddr_in *address, int listen_port) {
    int yes = 1;
    int server_socket;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket <= 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int success =
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (success < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address->sin_family = AF_INET;
    address->sin_addr.s_addr = INADDR_ANY;
    address->sin_port = htons(listen_port);

    success = bind(server_socket, (struct sockaddr *)address, sizeof(*address));
    if (success < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    return server_socket;
}


int create_server_sock(const char* hostname,int port){
    int sock;
    struct sockaddr_in recv_addr;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(port);
    inet_pton(AF_INET, hostname, &recv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0) {
        perror("Connection to server failed");
        close(sock);
        return -1;
    }

    return sock;

}



int get_content_length(const std::string &data, int *header_len) {
    const std::string search_str = "content-length: ";
    size_t pos = data.find(search_str);

    if (pos == std::string::npos) {
        *header_len = 0;
        return 0;
    }

    pos += search_str.length();
    while (pos < data.size() && std::isspace(data[pos])) {
        pos++;
    }

    size_t end_pos = pos;
    while (end_pos < data.size() && std::isdigit(data[end_pos])) {
        end_pos++;
    }

    int content_length = std::stoi(data.substr(pos, end_pos - pos));

    size_t headers_end = data.find("\r\n\r\n");
    *header_len = headers_end + 4;
    return content_length;
}

int check_header(std::string &data){
    std::string lower_data = data;
    std::transform(lower_data.begin(), lower_data.end(), lower_data.begin(), ::tolower);

    size_t get_pos = lower_data.find("get");
    size_t http_pos = lower_data.find("http/1.1");
    size_t post_pos = lower_data.find("post");
    
    size_t init_pos = lower_data.find(".init");
    size_t vid_pos = lower_data.find(".mpd");
    size_t m4s_pos = lower_data.find(".m4s");
    size_t frag_pos = lower_data.find("on-fragment-received");
    if (get_pos != std::string::npos && http_pos != std::string::npos) {
        if (vid_pos < http_pos && vid_pos > get_pos) {
            return 1;
        }
        else if(get_pos < m4s_pos && m4s_pos < http_pos){
            if(init_pos == std::string::npos){
                return 2;
            }
        }
    }else if(post_pos != std::string::npos && http_pos != std::string::npos){
        if(post_pos < frag_pos && frag_pos < http_pos){
            return 3;
        }
    }
    return 0;
}

void printValidPaths(const std::unordered_map<std::string, std::vector<int>> &valid_paths) {
    for (const auto &entry : valid_paths) {
        std::cout << "Key: " << entry.first << ", Values: ";
        for (const int &value : entry.second) {
            std::cout << value << " "; // 输出每个值
        }
        std::cout << std::endl; // 输出换行
    }
}

std::vector<int> getVideoBandwidths(const std::string &xml_content) {
    std::vector<int> bandwidths;
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml_content.c_str());

    if (!result) {
        std::cerr << "XML parsed with errors, error: " << result.description() << std::endl;
        return bandwidths;
    }

    pugi::xml_node period = doc.child("MPD").child("Period");
    if (period) {
        pugi::xml_node adaptationSet = period.child("AdaptationSet");
        while (adaptationSet) {
            if (std::string(adaptationSet.attribute("mimeType").as_string()).find("video") != std::string::npos) {
                for (pugi::xml_node representation = adaptationSet.child("Representation"); representation; representation = representation.next_sibling("Representation")) {
                    int bandwidth = representation.attribute("bandwidth").as_int();
                    bandwidths.push_back(bandwidth);
                }
            }
            adaptationSet = adaptationSet.next_sibling("AdaptationSet");
        }
    }
    return bandwidths;
}

void replaceMpdWithNoList(std::string& data, std::unordered_map<std::string, std::vector<int>> &valid_paths,int server_sock) {
    std::string modifiedData = data;
    std::string oldData = data;
    std::transform(modifiedData.begin(), modifiedData.end(), modifiedData.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string target = "vid.mpd";
    std::string replacement = "vid-no-list.mpd";

    size_t pos = modifiedData.find(target);
    if (pos != std::string::npos) {
        data.replace(pos, target.length(), replacement);
    }
    std::string method = "get";
    std::string http_version = "http/1.1";
    size_t method_pos = modifiedData.find(method);
    if (method_pos != std::string::npos) {
        size_t path_start = method_pos + method.length() + 1;
        size_t http_pos = modifiedData.find(http_version, path_start);

        if (http_pos != std::string::npos) {
            std::string path = oldData.substr(path_start, http_pos - path_start);
            auto it = valid_paths.find(path);
            if (it == valid_paths.end()) {
                
                send(server_sock,oldData.c_str(),oldData.size(),0);
                std::string T_data;
                char temp_buffer[BUFFER_SIZE];
                int total_bytes_read = 0;

                while (true) {
                    ssize_t bytes_read = read(server_sock, temp_buffer, sizeof(temp_buffer) - 1);
                    temp_buffer[bytes_read] = '\0';
                    T_data.append(temp_buffer, bytes_read);
                    total_bytes_read += bytes_read;

                    if (T_data.find("\r\n\r\n") != std::string::npos) {
                        break;
                    }
                }

                int content_length, header_length;
                content_length = get_content_length(T_data, &header_length);
                int use_length = total_bytes_read - header_length;

                while (use_length < content_length) {
                    ssize_t bytes_read = read(server_sock, temp_buffer, sizeof(temp_buffer) - 1);

                    T_data.append(temp_buffer,bytes_read);
                    total_bytes_read += bytes_read;
                    use_length += bytes_read;
                }
                size_t header_end = T_data.find("\r\n\r\n");
                size_t lastSlashPos = path.find_last_of('/');

                valid_paths[path.substr(0, lastSlashPos) + "/video"] = getVideoBandwidths(T_data.substr(header_end + 4));
                std::vector<int> videoBandwidths = getVideoBandwidths(T_data.substr(header_end + 4));

                
            }
        }
    }


}


std::optional<std::string> findKeyInData(const std::unordered_map<std::string, std::vector<int>> &valid_paths, const std::string &data) {
    for (const auto &entry : valid_paths) {
        if (data.find(entry.first) != std::string::npos) {
            return entry.first;
        }
    }
    return std::nullopt;
}

std::optional<std::string> extractUUID(const std::string &request) {
    std::string key = "x-489-uuid: "; 
    std::string lowerRequest = request;
    
    std::transform(lowerRequest.begin(), lowerRequest.end(), lowerRequest.begin(), ::tolower);

    size_t pos = lowerRequest.find(key);
    if (pos != std::string::npos) {
        size_t start = pos + key.length();
        size_t end = lowerRequest.find("\n", start);
        return request.substr(start, end - start);
    }
    return std::nullopt;
}

int calculateBit(const std::unordered_map<std::string, std::vector<int>> &valid_paths,
                 const std::string &key, int t) {
    auto it = valid_paths.find(key);
    if (it != valid_paths.end()) {
        const std::vector<int> &values = it->second;
        int threshold = static_cast<int>(1.5 * t);
        int maxBelowThreshold = std::numeric_limits<int>::min();
        
        for (int value : values) {
            if (value < threshold) {
                maxBelowThreshold = std::max(maxBelowThreshold, value);
            }
        }

        if (maxBelowThreshold > std::numeric_limits<int>::min()) {
            return maxBelowThreshold;
        } else {
            return *std::min_element(values.begin(), values.end());
        }
    }
    return 0;
}

void replaceBitrateInRequest(std::string &request, int bitrate) {
    std::string oldVidPrefix = "video/vid-";
    std::string oldVidSuffix = "-seg-";
    size_t startPos = request.find(oldVidPrefix);
    
    if (startPos != std::string::npos) {
        size_t endPos = request.find(oldVidSuffix, startPos);
        if (endPos != std::string::npos) {
            std::string newBitrate = std::to_string(bitrate);
            request.replace(startPos + oldVidPrefix.length(), endPos - (startPos + oldVidPrefix.length()), newBitrate);
        }
    }
}

std::optional<int> extractFragmentSize(const std::string &request) {
    std::string key = "X-Fragment-Size: ";
    size_t pos = request.find(key);
    if (pos != std::string::npos) {
        size_t start = pos + key.length();
        size_t end = request.find("\n", start);
        return std::stoi(request.substr(start, end - start));
    }
    return std::nullopt;
}

std::optional<long> extractTimestampStart(const std::string &request) {
    std::string key = "X-Timestamp-Start: ";
    size_t pos = request.find(key);
    if (pos != std::string::npos) {
        size_t start = pos + key.length();
        size_t end = request.find("\n", start);
        return std::stol(request.substr(start, end - start));
    }
    return std::nullopt;
}

std::optional<long> extractTimestampEnd(const std::string &request) {
    std::string key = "X-Timestamp-End: ";
    size_t pos = request.find(key);
    if (pos != std::string::npos) {
        size_t start = pos + key.length();
        size_t end = request.find("\n", start);
        return std::stol(request.substr(start, end - start));
    }
    return std::nullopt;
}

double calculateThroughput(int fragmentSize, long startTime, long endTime) {
    double durationInSeconds = (endTime - startTime) / 1000.0;
    return (fragmentSize / 1024.0) / durationInSeconds; // KB/s
}

void updateThroughput(std::map<std::string, double> &throughput,
                      const std::string &uuid, int fragmentSize, long startTime, long endTime, double alpha,int *tput,int *avg_tput) {
    auto it = throughput.find(uuid);
    if (it != throughput.end()) {
        double oldThroughput = it->second;
        double newThroughput = calculateThroughput(fragmentSize, startTime, endTime);
        *tput = newThroughput;
        double updatedThroughput = alpha * newThroughput + (1 - alpha) * oldThroughput;
        *avg_tput = updatedThroughput;
        throughput[uuid] = updatedThroughput;
    }
}

void respondWithOK(int client_sock) {
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 2\r\n"
                           "\r\n"
                           "OK"; // 响应体

    send(client_sock, response.c_str(), response.size(), 0);
}

std::string extractChunkName(const std::string& request) {
    std::string lowerRequest = request;
    std::transform(lowerRequest.begin(), lowerRequest.end(), lowerRequest.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    std::string method = "get";
    std::string http_version = "http/1.1";
    
    size_t method_pos = lowerRequest.find(method);
    if (method_pos != std::string::npos) {
        size_t path_start = method_pos + method.length() + 1; // +1 for the space
        size_t http_pos = lowerRequest.find(http_version, path_start);
        
        if (http_pos != std::string::npos) {
            return request.substr(path_start, http_pos - path_start);
        }
    }
    
    return "";
}

int forward_data(int client_sock, std::string &data, const char *hostname, int port, int server_sock,std::unordered_map<std::string, std::vector<int>> &valid_paths,std::map<std::string, double> &throughput,double alpha,const char*videoserver_ip,int videoserver_port) {
    data.clear();
    char temp_buffer[BUFFER_SIZE];
    int total_bytes_read = 0;

    while (true) {
        ssize_t bytes_read = read(client_sock, temp_buffer, sizeof(temp_buffer) - 1);
        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        temp_buffer[bytes_read] = '\0';
        data.append(temp_buffer, bytes_read);
        total_bytes_read += bytes_read;

        if (data.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }

    int answer = check_header(data);
    if (answer == 1){
        replaceMpdWithNoList(data,valid_paths,server_sock);
        auto client_uuid = extractUUID(data);
        // std::cout << data.c_str() << std::endl;
        std::string chunkname = extractChunkName(data);
        spdlog::info("Manifest requested by {} forwarded to {}:{} for {}", *client_uuid, videoserver_ip, videoserver_port, chunkname); 
        // printValidPaths(valid_paths);
    }else if(answer == 2){
        int bitrate;
        std::optional<std::string> foundKey = findKeyInData(valid_paths, data);
        if (foundKey) {
            auto uuid = extractUUID(data);
            if (uuid) {
                double t;

                if (throughput.find(*uuid) == throughput.end()) {
                    throughput[*uuid] = 0.0;
                }
                t = throughput[*uuid]; 
                int result = calculateBit(valid_paths, *foundKey, t);
                bitrate = result;
                replaceBitrateInRequest(data,result);
                // std::cout << result << std::endl;
                // std::cout << data << std::endl;
            } else {
                std::cout << "UUID not found in the request." << std::endl;
            }
        }
        auto client_uuid = extractUUID(data);
        std::string chunkname = extractChunkName(data);
        spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps", *client_uuid, videoserver_ip, videoserver_port, chunkname, bitrate); 
    }else if(answer == 3){
        int segment_size, segment_duration, tput, avg_tput;
        auto foundKey = extractUUID(data);
        if (foundKey) {
            auto fragmentSize = extractFragmentSize(data);
            segment_size = *fragmentSize;
            auto startTime = extractTimestampStart(data);
            auto endTime = extractTimestampEnd(data);
            segment_duration = *endTime - *startTime ;

            if (fragmentSize && startTime && endTime) {
                updateThroughput(throughput, *foundKey, *fragmentSize, *startTime, *endTime, alpha,&tput,&avg_tput);
                // std::cout << "Updated Throughput for UUID " << *foundKey << ": " << throughput[*foundKey] << " KB/s" << std::endl;
            } else {
                std::cout << "Error extracting fragment size or timestamps." << std::endl;
            }
        } else {
            std::cout << "UUID not found in the request." << std::endl;
        }

        respondWithOK(client_sock);
        auto client_uuid = extractUUID(data);
        spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. Throughput: {} Kbps. Avg Throughput: {} Kbps", *client_uuid, segment_size, segment_duration, tput, avg_tput); 
        return -666;

    }
    


    // if(answer ==1)
    // std::cout << data.c_str() << std::endl;
    send(server_sock, data.c_str(), data.size(), 0);

    // std::cout << data.c_str() << std::endl;

    int content_length, header_length;
    content_length = get_content_length(data, &header_length);
    int use_length = total_bytes_read - header_length;

    while (use_length < content_length) {
        ssize_t bytes_read = read(client_sock, temp_buffer, sizeof(temp_buffer) - 1);
        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        send(server_sock, temp_buffer, bytes_read, 0);
        total_bytes_read += bytes_read;
        use_length += bytes_read;
    }

    return total_bytes_read;
}



void backward_data(int client_sock, int server_sock, std::string &data) {
    data.clear();
    char temp_buffer[BUFFER_SIZE];
    int total_bytes_read = 0;

    // std::cout << "e" << std::endl;

    while (true) {
        ssize_t bytes_read = read(server_sock, temp_buffer, sizeof(temp_buffer) - 1);
        if (bytes_read < 0) {
            return;
        }
        if (bytes_read == 0) {
            break;
        }

        temp_buffer[bytes_read] = '\0';
        data.append(temp_buffer, bytes_read);
        total_bytes_read += bytes_read;

        if (data.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        // std::cout << data << std::endl;

    }

    // std::cout << "f" << std::endl;

    send(client_sock, data.c_str(), data.size(), 0);

    // std::cout << "g" << std::endl;

    int content_length, header_length;
    content_length = get_content_length(data, &header_length);
    int use_length = total_bytes_read - header_length;
    // std::cout << content_length << std::endl;

    // std::cout << "h" << std::endl;


    while (use_length < content_length) {
        ssize_t bytes_read = read(server_sock, temp_buffer, sizeof(temp_buffer) - 1);
        if (bytes_read < 0) {
            return;
        }
        if (bytes_read == 0) {
            break;
        }
        // std::cout << use_length << std::endl;

        send(client_sock, temp_buffer, bytes_read, 0);
        total_bytes_read += bytes_read;
        use_length += bytes_read;
    }
}

void no_load_balancing(int listen_port, const char* hostname, int port, double alpha) {
    int server_socket, addrlen, activity, valread;
    int client_sockets[MAXCLIENTS] = {0};
    int client_sock;
    struct sockaddr_in address;
    server_socket = get_server_socket(&address, listen_port);
    char buffer[BUFFER_SIZE];
    addrlen = sizeof(address);
    fd_set readfds;
    std::unordered_map<std::string, std::vector<int>> valid_paths;
    std::map<std::string, double> throughput;


    while (true) {
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);

        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0) {
                FD_SET(client_sock, &readfds);
            }
        }

        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);

        if (FD_ISSET(server_socket, &readfds)) {
            int new_socket = accept(server_socket, (struct sockaddr *)&address, (socklen_t *)&addrlen);
            if (new_socket < 0) {
                perror("accept");
                exit(EXIT_FAILURE);
            }
            spdlog::info("New client socket connected with {}:{} on sockfd {}", inet_ntoa(address.sin_addr), ntohs(address.sin_port), new_socket); 
            // std::cout << "hello" << std::endl;
            
            for (int i = 0; i < MAXCLIENTS; i++) {
                if (client_sockets[i] == 0) {
                    client_sockets[i] = new_socket;
                    break;

                }
            }
        }

        int server_sock = create_server_sock(hostname, port);

        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0 && FD_ISSET(client_sock, &readfds)) {
                std::string data;
                // std::cout << "world" << std::endl;
                int l = forward_data(client_sock, data, hostname, port, server_sock,valid_paths,throughput,alpha,hostname,port);
                // std::cout << l << std::endl;
                // std::cout << data << std::endl;
                if (l > 0) {
                    // std::cout << "c" << std::endl;
                    backward_data(client_sock, server_sock, data);
                    // std::cout << "b" << std::endl;
                } else if(l == -666){

                }else {
                    client_sockets[i] = 0;
                    spdlog::info("Client socket sockfd {} disconnected", client_sock); 
                }
            }
        }
    }
}

int send_load_balancer_request(const char* hostname, int port, const LoadBalancerRequest& request) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/Address not supported");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }

    if (send(sock, &request, sizeof(request), 0) < 0) {
        perror("Send failed");
    } else {
        // std::cout << "Request sent: client_addr = " << request.client_addr 
        //           << ", request_id = " << request.request_id << std::endl;
    }

    return sock;
}

void receive_load_balancer_response(int server_sock, LoadBalancerResponse& response) {
    ssize_t bytes_received = recv(server_sock, &response, sizeof(response), 0);
    
    if (bytes_received < 0) {
        perror("Receive failed");
    } else if (bytes_received == 0) {
        std::cerr << "Connection closed by the server" << std::endl;
    } else {
        close(server_sock);
        // std::cout << "Response received: videoserver_addr = " << response.videoserver_addr 
        //           << ", videoserver_port = " << response.videoserver_port 
        //           << ", request_id = " << response.request_id << std::endl;
    }
}

int create_server_sock(in_addr_t server_addr, uint16_t server_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = server_port;
    server_address.sin_addr.s_addr = server_addr;
    if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        // std::cout << "Failed to connect to server at " 
        //           << inet_ntoa(*(struct in_addr*)&server_addr) 
        //           << " on port " << htons(server_port) << server_port << std::endl;
        perror("Connection to server failed");
        close(sock);
        return -1;
    }

    // std::cout << "Connected to video server " 
    //           << inet_ntoa(*(struct in_addr*)&server_addr) 
    //           << " on port " << htons(server_port) << std::endl;

    return sock; // 返回有效的套接字
}

struct pair_hash {
    template <class T1, class T2>
    std::size_t operator () (const std::pair<T1, T2>& pair) const {
        auto hash1 = std::hash<T1>{}(pair.first);
        auto hash2 = std::hash<T2>{}(pair.second);
        return hash1 ^ hash2;
    }
};

void load_balancing(int listen_port, const char* hostname, int port, double alpha) {
    int server_socket, addrlen, activity;
    int client_sockets[MAXCLIENTS] = {0};
    int client_sock;
    struct sockaddr_in address;
    server_socket = get_server_socket(&address, listen_port);
    char buffer[BUFFER_SIZE];
    addrlen = sizeof(address);
    fd_set readfds;

    std::unordered_map<int, int> client_map; 
    std::unordered_map<std::pair<in_addr_t, uint16_t>, int, pair_hash> server_map; 
    std::map<std::string, double> throughput;
    std::unordered_map<int,LoadBalancerResponse> response_map;
    std::unordered_map<std::string, std::vector<int>> valid_paths;

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);

        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0) {
                FD_SET(client_sock, &readfds);
            }
        }

        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);

        if (FD_ISSET(server_socket, &readfds)) {
            int new_socket = accept(server_socket, (struct sockaddr *)&address, (socklen_t *)&addrlen);
            if (new_socket < 0) {
                perror("accept");
                exit(EXIT_FAILURE);
            }
            spdlog::info("New client socket connected with {}:{} on sockfd {}", inet_ntoa(address.sin_addr), ntohs(address.sin_port), new_socket); 
           
            for (int i = 0; i < MAXCLIENTS; i++) {
                if (client_sockets[i] == 0) {
                    client_sockets[i] = new_socket;
                    break;
                }
            }
        }

        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0 && FD_ISSET(client_sock, &readfds)) {
                if (client_map.find(client_sock) == client_map.end()) {
                    uint16_t request_id = static_cast<uint16_t>(rand() % 65536);

                    LoadBalancerRequest request;
                    request.client_addr = address.sin_addr.s_addr; 
                    request.request_id = request_id;

                    int server_sockets = send_load_balancer_request(hostname, port, request);
                    LoadBalancerResponse response;

                    receive_load_balancer_response(server_sockets, response); 
                    response_map[client_sock] = response;

                    auto server_key = std::make_pair(response.videoserver_addr, response.videoserver_port);
                    if (server_map.find(server_key) != server_map.end()) {
                        int server_sock = server_map[server_key];
                        client_map[client_sock] = server_sock;
                    } else {
                        int server_sock = create_server_sock(response.videoserver_addr, response.videoserver_port);
                        server_map[server_key] = server_sock;
                        client_map[client_sock] = server_sock;
                    }
                }

                int server_sock = client_map[client_sock];
                LoadBalancerResponse response = response_map[client_sock];

                struct in_addr ip_addr;
                ip_addr.s_addr = response.videoserver_addr;
                char* ip = inet_ntoa(ip_addr);

                int port = static_cast<int>(response.videoserver_port);

                std::string data;
                int l = forward_data(client_sock, data, ip, port, server_sock,valid_paths,throughput,alpha,hostname,port);
                if (l > 0) {
                    backward_data(client_sock,server_sock, data);
                } else if (l == -666) {
                    // 处理特定错误
                } else {
                    client_sockets[i] = 0;
                    client_map.erase(client_sock);
                    spdlog::info("Client socket sockfd {} disconnected", client_sock);
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    cxxopts::Options options("miProxy", "A simple HTTP streaming proxy");

    options.add_options()
        ("l,listening-port", "Port for proxy to listen on", cxxopts::value<int>())
        ("h,hostname", "IP address of the video server or load balancer", cxxopts::value<std::string>())
        ("p,port", "Port of the video server or load balancer", cxxopts::value<int>())
        ("a,alpha", "EWMA throughput estimate coefficient (0 to 1)", cxxopts::value<float>())
        ("b,balance", "Enable load balancing mode")
        ("help", "Print help");

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    // try {
    //     listening_port = result["listening-port"].as<int>();
    //     hostname = result["hostname"].as<std::string>();
    //     port = result["port"].as<int>();
    //     alpha = result["alpha"].as<float>();
    // } catch (const std::bad_variant_access&) {
    //     std::cerr << "Error: Invalid type for one or more arguments." << std::endl;
    //     return 1;
    // }

    int listening_port = result["listening-port"].as<int>();
    std::string hostname = result["hostname"].as<std::string>();
    int port = result["port"].as<int>();
    double alpha = result["alpha"].as<float>();

    if (result.count("listening-port") == 0 || 
        result.count("hostname") == 0 || 
        result.count("port") == 0 || 
        result.count("alpha") == 0) {
        std::cerr << "Error: All four required arguments must be specified." << std::endl;
        return 1;
    }

    if (listening_port < 1024 || listening_port > 65535 || 
        port < 1024 || port > 65535) {
        std::cerr << "Error: Ports must be in the range [1024, 65535]." << std::endl;
        return 1;
    }

    if (alpha < 0.0f || alpha > 1.0f) {
        std::cerr << "Error: Alpha value must be in the range [0, 1]." << std::endl;
        return 1;
    }

    if (result.count("balance")) {
        spdlog::info("miProxy started");
        load_balancing(listening_port,hostname.c_str(),port,alpha);
    } else {
        spdlog::info("miProxy started");
        no_load_balancing(listening_port,hostname.c_str(),port,alpha);
    }

    return 0;
}
