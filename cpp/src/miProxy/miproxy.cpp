#include <arpa/inet.h> //close
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h> //strlen
#include <sys/socket.h>
#include <sys/time.h> //FD_SET, FD_ISSET, FD_ZERO, FD_SETSIZE macros
#include <sys/types.h>
#include <unistd.h> //close
#include <iostream>
#include <string>
#include <vector>
#include <cxxopts.hpp>
#include <pugixml.hpp>
#include <fcntl.h>
#include "spdlog/spdlog.h"
#include <netinet/tcp.h>  
#include "../common/server.cpp"
#include "../common/LoadBalancerProtocol.h"
#include <arpa/inet.h>
#include <netinet/in.h>
using namespace std;

#define MAXCLIENTS 30

void set_nonblocking(int socket)
{
  int flags = fcntl(socket, F_GETFL, 0);
  fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

void send_message(int socket, string message)
{
  send(socket, message.c_str(), message.length(), 0);
}

vector<int> get_available_bandwidths(const string& xml_content) {
  vector<int> bandwidths;
  pugi::xml_document doc;

  pugi::xml_parse_result result = doc.load_string(xml_content.c_str());
  if (!result) {
      std::cerr << "XML 解析失败: " << result.description() << std::endl;
      return bandwidths;
  }

  // 修改点：使用精确的XPath过滤视频轨道
  pugi::xpath_node_set representations = doc.select_nodes(
      "//AdaptationSet[@mimeType='video/mp4']/Representation" // 只选择视频轨道
  );

  // 提取 bandwidth 属性值（原有逻辑不变）
  for (const auto& node : representations) {
      pugi::xml_attribute bandwidth_attr = node.node().attribute("bandwidth");
      if (bandwidth_attr) {
          bandwidths.push_back(bandwidth_attr.as_int());
      }
  }
  sort(bandwidths.begin(), bandwidths.end());
  return bandwidths;
}

ssize_t read_wrap(int socket, char *buffer, size_t length, int &readlen)
{
  readlen = read(socket, buffer, length);
  if (readlen < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      //cout << "read 0, pass" << endl;
      return 0; // 非错误，只是暂时无数据
    } 
    else {
      cerr << "Read " << length << " bytes on socket " << socket << " failed: " << strerror(errno) << endl;
      return -1;
    }
  }
  //cout << "Read " << readlen << " bytes" << endl;
  buffer[readlen] = '\0';
  return readlen;
}

ssize_t read_http(int socket_fd, string &response, string &client_ID)
{
  client_ID.clear();
  set_nonblocking(socket_fd);
  string buffer, header, content;
  char temp[4096];
  int readlen;
  ssize_t bytes_read, content_length = 0, pos;

  int original_flags = fcntl(socket_fd, F_GETFL);
  fcntl(socket_fd, F_SETFL, original_flags & ~O_NONBLOCK);

  while ((bytes_read = read_wrap(socket_fd, temp, sizeof(temp), readlen)) > 0) {
    buffer.append(temp, readlen);
    pos = buffer.find("\r\n\r\n");
    if (pos != std::string::npos) {
      header = buffer.substr(0, pos + 4); // 跳过 "\r\n\r\n"
      //cout << "header: " << header << endl;
      response = header;
      content = buffer.substr(pos + 4);
      buffer.clear(); 
      // 解析头部
      istringstream header_stream(header);
      string line;
      while (getline(header_stream, line) && line != "\r") {
        size_t colon_pos = line.find(':');
        if (colon_pos != string::npos) {
          string key = line.substr(0, colon_pos);
          string value = line.substr(colon_pos + 2); // 跳过 ": "
          if (strcasecmp(key.c_str(), "Content-Length")==0)
          {
            try {
              content_length = stoul(value);
            }
            catch (const invalid_argument &) {
              cerr << "Invalid Content-Length" << std::endl;
            }
          }
          else if (strcasecmp(key.c_str(), "X-489-UUID")==0) {
            client_ID = value;
          }
        }
      }
      break;
    }
  }
  if (bytes_read < 0)
  {
    // 处理读取错误
    return -1;
  }
  // 读取剩余内容
  if (content_length > 0)
  {
    while (content.length() < content_length)
    {
      bytes_read = read_wrap(socket_fd, temp, min(sizeof(temp), static_cast<size_t>(content_length - content.length())), readlen);
      if (bytes_read <= 0)
        break;
      content.append(temp, readlen);
    }
    response.append(content);
  }

  fcntl(socket_fd, F_SETFL, original_flags);

  return bytes_read;
}

int get_server_socket(struct sockaddr_in *address, int server_port, string &hostname)
{
  int yes = 1;
  int server_socket;
  // create a master socket
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket <= 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // set master socket to allow multiple connections ,
  // this is just a good habit, it will work without this
  int success = setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR , &yes, sizeof(yes));
  if (success < 0)
  {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }
  
  setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  address->sin_family = AF_INET;
  address->sin_port = htons(server_port);
  if (inet_pton(AF_INET, hostname.c_str(), &address->sin_addr) <= 0)
  {
    perror("Invalid address/ Address not supported");
    return -1;
  }

  // Connect to the server
  if (connect(server_socket, (struct sockaddr *)address, sizeof(*address)) < 0)
  {
    perror("Connection failed");
    return -1;
  }

  //printf("-----server Listening on port %d-----\n", ntohs(address->sin_port));
  return server_socket;
}

int main(int argc, char *argv[])
{
  bool load_balancing = false;
  int listen_port = 0;
  string hostname;
  int server_port = 0;
  float alpha = 0.5f;

  try {
    cxxopts::Options options("miProxy", "HTTP adaptive streaming proxy");
        
    options.add_options()
      ("b,balancer", "Enable load balancing mode", cxxopts::value<bool>(load_balancing))
      ("l,listen-port", "Proxy listen port", cxxopts::value<int>(listen_port))
      ("h,hostname", "Video server hostname/IP", cxxopts::value<std::string>(hostname))
      ("p,port", "Video server port", cxxopts::value<int>(server_port))
      ("a,alpha", "EWMA coefficient [0-1]", cxxopts::value<float>(alpha))
      ("help", "Print usage");

      auto result = options.parse(argc, argv);

      if (result.count("help")) {
          std::cout << options.help() << std::endl;
          return 0;
      }

        listen_port = result["listen-port"].as<int>();
        hostname = result["hostname"].as<string>();
        server_port = result["port"].as<int>();
        alpha = result["alpha"].as<float>();
    } catch (const cxxopts::exceptions::exception& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        return 1;
    }



  if (hostname == "")
  {
    cerr << "hostname missing" << endl;
    return EXIT_FAILURE;
  }
  if (listen_port < 1024 || listen_port > 65535)
  {
    cerr << "listen-port must be in the range [1024, 65535]." << endl;
    return EXIT_FAILURE;
  }
  if (server_port < 1024 || server_port > 65535)
  {
    cerr << "server-port must be in the range [1024, 65535]." << endl;
    return EXIT_FAILURE;
  }
  if (alpha < 0.0 || alpha > 1.0)
  {
    cerr << "Alpha must be in the range [0, 1]." << endl;
    return EXIT_FAILURE;
  }

  vector<int> cache;
  map<string, vector<int>> bandwidths;
  vector<string> request_cache(30);
  map<string, double> throughput_cache;
  int proxy_socket, addrlen, activity, valread;
  int client_sockets[MAXCLIENTS] = {0};
  int client_states[MAXCLIENTS] = {0};
  int client_servers[MAXCLIENTS] = {0};
  vector<struct sockaddr_in> client_addresses(MAXCLIENTS), server_addresses(MAXCLIENTS);
  int client_sock, server_sock;

  struct sockaddr_in server_address, proxy_address, client_address, lb_address;

  if ((proxy_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  int socket_opt = 1;
  if (setsockopt(proxy_socket, SOL_SOCKET, SO_REUSEADDR, &socket_opt, sizeof(socket_opt)) < 0)
  {
    perror("setsockopt failed");
    close(proxy_socket);
    exit(EXIT_FAILURE);
  }
  memset(&proxy_address, 0, sizeof (proxy_address));
  proxy_address.sin_family = AF_INET;
  proxy_address.sin_port = htons(listen_port);
  proxy_address.sin_addr.s_addr = INADDR_ANY;
  if (::bind(proxy_socket, (struct sockaddr *)&proxy_address, sizeof(proxy_address)) < 0)
  {
    perror("bind failed");
    close(proxy_socket);
    exit(EXIT_FAILURE);
  }
  if (listen(proxy_socket, 10) < 0)
  {
    perror("listen");
    close(proxy_socket);
    exit(EXIT_FAILURE);
  }
  spdlog::info("miProxy started");

  char buffer[8192]; // data buffer of 1KiB + 1 bytes


  fd_set readfds;
  while (1)
  {
    //cout << "===========loop===========" << endl;

    FD_ZERO(&readfds);
    FD_SET(proxy_socket, &readfds);

    socklen_t addrlen = sizeof(client_address);  

    memset(&client_address, 0, sizeof(client_address)); 
    addrlen = sizeof(client_address);

    for (int i = 0; i < MAXCLIENTS; i++)
    {
      client_sock = client_sockets[i];
      server_sock = client_servers[i];
      if (client_sock != 0)
      {
        FD_SET(client_sock, &readfds);
        FD_SET(server_sock, &readfds);
      }
    }
    // cout << "added client sockets to readfds" << endl;
    //  wait for an activity on one of the sockets , timeout is NULL ,
    //  so wait indefinitely
    activity = select(FD_SETSIZE, &readfds, nullptr, nullptr, nullptr);
    // cout << "select finish" << endl;
    if ((activity < 0) && (errno != EINTR))
    {
      //cout << "no activity present, proxy terminated" << endl;
      perror("select error");
    }

    //=======================================================================================
    if (FD_ISSET(proxy_socket, &readfds))
    {
      // cout << "proxy: incoming connection" << endl;
      int new_socket = accept(proxy_socket, (struct sockaddr *)&client_address, (socklen_t *)&addrlen);
      setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &socket_opt, sizeof(socket_opt));
      if (new_socket < 0)
      {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      spdlog::info("New client socket connected with {}:{} on sockfd {}", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port), new_socket);

      if(load_balancing){
        int lb_fd;
        lb_fd = run_client(hostname.c_str(),server_port);
        
        LoadBalancerRequest request;
        request.client_addr = client_address.sin_addr.s_addr;
        //spdlo::info("{},{}",inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
        request.request_id = htons(rand() % 65536); 
        ssize_t send_bytes = send(lb_fd,&request,sizeof(request),0);

        LoadBalancerResponse response;
        char buffer[sizeof(LoadBalancerResponse)];
        memset(buffer, 0, sizeof(buffer));
        ssize_t recvd = 0;

        while (recvd < sizeof(LoadBalancerResponse)) {
            ssize_t rval = recv(lb_fd, buffer + recvd, sizeof(LoadBalancerResponse) - recvd, 0);
            if (rval <= 0) {
                close(lb_fd);
                return -1;
            }
            recvd += rval;
        }
        
        memcpy(&response, buffer, sizeof(LoadBalancerResponse));


        int actual_server_port = ntohs(response.videoserver_port);
        struct in_addr addr;
        addr.s_addr = response.videoserver_addr;
        const char *actual_ip_addr = inet_ntoa(addr);
        string ip_str(actual_ip_addr);
        //spdlo::info("ip address is {} , port is {}",actual_ip_addr,actual_server_port);

      for (int i = 0; i < MAXCLIENTS && client_sockets[i] != new_socket; i++)
      {
        // if position is empty
        if (client_sockets[i] == 0)
        {
          client_sockets[i] = new_socket;
          client_servers[i] = get_server_socket(&server_addresses[i], actual_server_port, ip_str); //这里的serverport和host改成ouput
          client_addresses[i] = client_address;
          set_nonblocking(client_sockets[i]);
          set_nonblocking(client_servers[i]);
          break;
        }
      }
    }
    else{
      for (int i = 0; i < MAXCLIENTS && client_sockets[i] != new_socket; i++)
      {
        // if position is empty
        if (client_sockets[i] == 0)
        {
          client_sockets[i] = new_socket;
          client_servers[i] = get_server_socket(&server_addresses[i], server_port, hostname); 
          client_addresses[i] = client_address;
          set_nonblocking(client_sockets[i]);
          set_nonblocking(client_servers[i]);
          break;
        }
      }
    }
    }
    // cout << "proxy operations done" << endl;

    //===============================================
    // else it's some IO operation on a client socket
    //===============================================
    for (int i = 0; i < MAXCLIENTS; i++)
    {
      client_sock = client_sockets[i];
      server_sock = client_servers[i];
      // Note: sd == 0 is our default here by fd 0 is actually stdin
      if (client_sock != 0 && server_sock != 0 && FD_ISSET(client_sock, &readfds))
      {
        //cout << "check client[" << i << ']' << endl;
        // Check if it was for closing , and also read the
        string client_message, client_ID;
        ssize_t result = read_http(client_sock, client_message, client_ID);
        //cout << "[" << endl << client_message << "]" << endl;
        if (result <= 0)
        {
          spdlog::info("Client socket sockfd {} disconnected", client_sock); 
          // Somebody disconnected , get their details and print
          //printf("\n---client disconnected---\n");
          //printf("client disconnected , ip %s , port %d \n", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
          // Close the socket and mark as 0 in list for reuse
          close(client_sock);
          client_sockets[i] = 0;
          close(server_sock);
          client_servers[i] = 0;
        } else if (result == 0) {
          //cout << "pass" << endl;
        } else {
          if (client_message.substr(0, 3) == "GET")
          {
            //cout << "{{{{{{ GET message }}}}}}" << endl;
            size_t pos;
            if ((pos = client_message.find("HTTP")) != string::npos)
            {
              string file_addr = client_message.substr(4, pos - 5);
              //cout << "file_addr: " << file_addr << endl;

              if (file_addr.length() >= 7 && file_addr.substr(file_addr.length() - 7, 7) == "vid.mpd")
              {
                
                send_message(server_sock, client_message);
                client_states[i] = 1;
                string file_addr_mod = file_addr.substr(0, file_addr.length() - 4).append("-no-list.mpd");
                
                spdlog::info("Manifest requested by {} forwarded to {}:{} for {}", client_ID, inet_ntoa(server_addresses[i].sin_addr), ntohs(server_addresses[i].sin_port), file_addr_mod); 
                //cout << "modified addr: " << file_addr_mod << endl;
                //cout << "modified request: " << string("GET ") + file_addr_mod + client_message.substr(pos - 1) << endl;
                request_cache[i] = string("GET ") + file_addr_mod + client_message.substr(pos - 1);
                // send_message(server_sock, string("GET ") + file_addr_mod + client_message.substr(pos - 1));
              }

              else if (file_addr.length() >= 5 && file_addr.substr(file_addr.length() - 4, 4) == ".m4s") 
              {
                // [PATH-TO-VIDEO] /video  /vid   -[BITRATE] -seg-[NUMBER].m4s
                //                 |       |      |          |
                //                 pos_a   pos_b  pos_b+4    pos_d
                size_t pos_a = string::npos, pos_b = file_addr.rfind('/'), pos_c = string::npos, pos_d = string::npos;
                int flag = 0;
                if (pos_b != string::npos) {
                  //cout << pos_b << endl;
                  pos_a = file_addr.substr(0, pos_b).rfind('/');
                  //cout << pos_a << ", " << pos_b << endl;
                  if (pos_a != string::npos) {
                    //cout << pos_a << ", " << pos_b << endl;
                    if (file_addr.substr(pos_a, 6)=="/video") {
                      pos_d = file_addr.substr(pos_b).find("-seg-");
                      if (pos_d != string::npos) {
                        pos_d += pos_b;
                        if (file_addr[pos_b+4]=='-') {
                          flag = 1;
                }}}}}
                if (flag) {
                  //cout << "select bandwidth" << endl;
                  if (throughput_cache.find(client_ID) == throughput_cache.end()) {
                    throughput_cache[client_ID] = 0.0;
                  }
                  /*if (bandwidths.find(client_ID) == bandwidths.end()) {
                    cout << "bandwidth for client_ID not found" << endl;
                  }*/
                  int selected_bw = bandwidths[client_ID].front(); // 默认最小带宽
                  for (int bw : bandwidths[client_ID]) {
                      if (bw <= throughput_cache[client_ID] * 1.5) {
                          selected_bw = bw; // 遍历升序列表，最终选中最大的可用值
                      } else {
                          break;
                      }
                  }
                  //cout << "current bandwidth: " << bandwidths[client_ID][j] << ", throughput: " << throughput_cache[client_ID] << endl;
                  string file_addr_mod = file_addr.substr(0, pos_b+5) + to_string(selected_bw) + file_addr.substr(pos_d);
                  //cout <<"modified message: " << string("GET ") + file_addr_mod + client_message.substr(pos - 1) << endl;
                  spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps", client_ID, inet_ntoa(server_addresses[i].sin_addr), ntohs(server_addresses[i].sin_port), file_addr_mod, selected_bw); 
                  send_message(server_sock, string("GET ") + file_addr_mod + client_message.substr(pos - 1));
                }
                else {
                  send_message(server_sock, client_message);
                }
              }
              else
              {
                send_message(server_sock, client_message);
              }
            }
            else
            {
              send_message(server_sock, client_message);
            }
          }
          else if (client_message.substr(0, 4) == "POST")
          {
            //cout << "{{{{{{ POST message }}}}}}" << endl;
            //cout << client_message << endl;
            int frag_size = 0;
            long long time_start = 0, time_end = 0;
            istringstream header_stream(client_message);
            string line;
            while (getline(header_stream, line) && line != "\r") {
              size_t colon_pos = line.find(':');
              if (colon_pos != string::npos) {
                string key = line.substr(0, colon_pos);
                string value = line.substr(colon_pos + 2); // 跳过 ": "
                value = value.substr(0, value.length()-1);
                //spdlo::info("[{}]: [{}]", key, value);
                if (strcasecmp(key.c_str(), "X-Fragment-Size")==0){
                  frag_size = stoi(value);
                  //pdlog::info("frag_size={}", frag_size);
                }
                else if (strcasecmp(key.c_str(), "X-Timestamp-Start")==0) {
                  time_start = stoll(value);
                  //pdlog::info("time_start={}", time_start);
                }
                else if (strcasecmp(key.c_str(), "X-Timestamp-End")==0) {
                  time_end = stoll(value);
                  //pdlog::info("time_end={}", time_end);
                }
              }
            }
            
            if (time_end <= time_start) {
              spdlog::error("Invalid timestamps from client {}", client_ID);
            } else {
                double time_diff_s = (time_end - time_start) / 1000.0;
                double throughput_kbps = (frag_size / 1024.0) / time_diff_s; // 转换为Kbps
                throughput_cache[client_ID] = alpha * throughput_kbps + (1 - alpha) * throughput_cache[client_ID];
                //spdlog::debug("throughput_kbps=[{}], tput_cache=[{}]", throughput_kbps, throughput_cache[client_ID]);
                //cout << throughput_kbps << ", " << throughput_cache[client_ID] << endl;
                spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. Throughput: {} Kbps. Avg Throughput: {} Kbps", client_ID, frag_size, time_end - time_start, floor(throughput_kbps), floor(throughput_cache[client_ID])); 
              send_message(client_sock, string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK"));
            }
          }
          else
          {
            send_message(server_sock, client_message);
            // send(server_socket, buffer, strlen(buffer), 0);
          }
        }
      }
      if (client_sock != 0 && server_sock != 0 && FD_ISSET(server_sock, &readfds))
      {
        string temp;
        string server_message;
        int readlen = 0;

        if (client_states[i] == 0)
        {
          read_http(server_sock, server_message, temp);
          send_message(client_sock, server_message);
        }
        else if (client_states[i] == 1) 
        {
          client_states[i] = 0;
          // read_http(server_sock, server_message);
          // cout << server_message << endl;
          server_message.clear();
          read_http(server_sock, server_message, temp);
          size_t content_pos = server_message.find("\r\n\r\n"), content_pos_2 = request_cache[i].find("\r\n\r\n");
          istringstream header_stream(request_cache[i].substr(0, content_pos_2));
          string line, client_ID;
          //cout << header_stream.str()<< endl;
          while (getline(header_stream, line) && line != "\r") {
            size_t colon_pos = line.find(':');
            //cout << "[" << line.substr(0, line.length()-1) << "]" << endl;
            if (colon_pos != string::npos) {
              //cout << "[" << line.substr(0, line.length()-1) << "]" << endl;
              string key = line.substr(0, colon_pos);
              string value = line.substr(colon_pos + 2); 
              if (strcasecmp(key.c_str(), "X-489-UUID")==0) {
                client_ID = value;
              }
            }
          }
          //cout << "client_ID=" << client_ID << endl;
          if (!client_ID.empty() && content_pos != string::npos) {
            bandwidths[client_ID] = get_available_bandwidths(server_message.substr(content_pos + 4));
            /*cout << "bandwidths: ";
            for(int j = 0; j < bandwidths[client_ID].size(); j++) {
              cout << bandwidths[client_ID][j] << ' ';
            }
            cout << endl;*/
          }
          send_message(server_sock, request_cache[i]);
          request_cache[i].clear();
          //cout << server_message << endl;
        }
        //cout << "server return message: " << server_message << endl;
      }
    }
  }
  return 0;
}