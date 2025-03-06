#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include <cxxopts.hpp>
#include <pugixml.hpp>
#include <fcntl.h>
#include "spdlog/spdlog.h"
#include <fstream>
#include <map>
#include <algorithm>
using namespace std;

#define MAXCLIENTS 30

void set_nonblocking(int socket)
{
  int flags = fcntl(socket, F_GETFL, 0);
  fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

ssize_t send_message(int socket, string message)
{
  return send(socket, message.c_str(), message.length(), 0);
}

vector<int> get_available_bandwidths(const string& xml_content) {
  vector<int> bandwidths;
  pugi::xml_document doc;

  pugi::xml_parse_result result = doc.load_string(xml_content.c_str());
  if (!result) {
      std::cerr << "XML 解析失败: " << result.description() << std::endl;
      return bandwidths;
  }

  pugi::xpath_node_set representations = doc.select_nodes("//Representation");

  for (const auto& node : representations) {
      pugi::xml_attribute bandwidth_attr = node.node().attribute("bandwidth");
      if (bandwidth_attr) {
          bandwidths.push_back(bandwidth_attr.as_int());
      }
  }

  return bandwidths;
}

ssize_t read_wrap(int socket, char *buffer, size_t length, int &readlen) {
    readlen = read(socket, buffer, length);
    if (readlen < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; 
        } else if (errno == ECONNRESET) {
            spdlog::error("Connection reset by peer on socket {}", socket);
            return -1;
        } else {
            spdlog::error("Read failed on socket {}: {}", socket, strerror(errno));
            return -1;
        }
    } else if (readlen == 0) {
        spdlog::info("Connection closed by peer on socket {}", socket);
        return 0; 
    }
    return readlen; 
}

ssize_t read_http(int socket_fd, string &response, string &client_ID)
{
  spdlog::info("read socket number {}", socket_fd);
  client_ID.clear();
  response.clear();
  
  // 设置阻塞模式
  int flags = fcntl(socket_fd, F_GETFL, 0);
  fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
  
  // 设置超时
  struct timeval tv;
  tv.tv_sec = 5; // 5秒超时
  tv.tv_usec = 0;
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  
  // 使用大缓冲区
  char buffer[65536];
  int total_bytes = 0;
  
  // 读取数据直到连接关闭或超时
  while (true) {
    int bytes = recv(socket_fd, buffer, sizeof(buffer), 0);
    if (bytes <= 0) {
      if (bytes == 0) {
        spdlog::info("Connection closed after reading {} bytes", total_bytes);
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        spdlog::info("Timeout reached after reading {} bytes", total_bytes);
      } else {
        spdlog::error("Error reading from socket: {}", strerror(errno));
      }
      break;
    }
    
    response.append(buffer, bytes);
    total_bytes += bytes;
    
    // 如果这是第一次读取，尝试解析HTTP头部
    if (total_bytes == bytes) {
      // 查找头部结束标记
      size_t header_end = response.find("\r\n\r\n");
      if (header_end != string::npos) {
        // 解析头部，查找Content-Length和客户端ID
        string header = response.substr(0, header_end + 4);
        istringstream header_stream(header);
        string line;
        size_t content_length = 0;
        bool has_content_length = false;
        
        while (getline(header_stream, line) && line != "\r") {
          size_t colon_pos = line.find(':');
          if (colon_pos != string::npos) {
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 2); // Skip ": "
            
            // 移除尾部的\r
            if (!value.empty() && value.back() == '\r') {
              value.pop_back();
            }
            
            if (strcasecmp(key.c_str(), "Content-Length") == 0) {
              try {
                content_length = stoul(value);
                has_content_length = true;
                spdlog::info("Content-Length: {}", content_length);
              }
              catch (const invalid_argument &) {
                spdlog::error("Invalid Content-Length");
              }
            }
            else if (strcasecmp(key.c_str(), "X-489-UUID") == 0) {
              client_ID = value;
              spdlog::info("Client ID: {}", client_ID);
            }
          }
        }
        
        // 如果有Content-Length，检查是否已经读取了所有内容
        if (has_content_length) {
          size_t expected_total = header_end + 4 + content_length;
          if (response.length() >= expected_total) {
            spdlog::info("Already read complete response ({} bytes)", response.length());
            break;
          } else {
            spdlog::info("Need to read more data (have {}, need {})", response.length(), expected_total);
          }
        }
      }
    }
    
    // 报告进度
    if (total_bytes % (1024*1024) < bytes) {
      spdlog::info("Read {} MB total", total_bytes / (1024*1024));
    }
  }
  
  // 恢复非阻塞模式
  fcntl(socket_fd, F_SETFL, flags);
  
  // 重置超时
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  
  return total_bytes;
}

int get_server_socket(struct sockaddr_in *address, int server_port, string &hostname)
{
  int yes = 1;
  int server_socket;
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket <= 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
  {
    perror("setsockopt");
    close(server_socket);
    exit(EXIT_FAILURE);
  }

  address->sin_family = AF_INET;
  address->sin_port = htons(server_port);
  if (inet_pton(AF_INET, hostname.c_str(), &address->sin_addr) <= 0)
  {
    perror("Invalid address/ Address not supported");
    close(server_socket);
    return -1;
  }

  if (connect(server_socket, (struct sockaddr *)address, sizeof(*address)) < 0)
  {
    perror("Connection failed");
    close(server_socket);
    return -1;
  }

  return server_socket;
}

int main(int argc, char *argv[])
{
  int cmd_opt, listen_port = 0;
  string hostname = "";
  int server_port = 0;
  float alpha = -1;

  try
  {
    cxxopts::Options options("miProxy", "A proxy server for video streaming");

    options.add_options()("l,listen-port", "The port proxy should listen on for accepting connections", cxxopts::value<int>())("h,hostname", "IP address of the video server", cxxopts::value<std::string>())("p,port", "Port of the video server", cxxopts::value<int>())("a,alpha", "Coefficient in your EWMA throughput estimate", cxxopts::value<float>())("help", "Print usage");

    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
      cout << options.help() << endl;
      return 0;
    }

    listen_port = result["listen-port"].as<int>();
    hostname = result["hostname"].as<string>();
    server_port = result["port"].as<int>();
    alpha = result["alpha"].as<float>();

    cout << "Listen Port: " << listen_port << endl;
    cout << "Hostname: " << hostname << endl;
    cout << "Port: " << server_port << endl;
    cout << "Alpha: " << alpha << endl;
  }
  catch (const cxxopts::exceptions::no_such_option &e)
  {
    cerr << "Error parsing options: " << e.what() << endl;
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
  vector<string> request_cache(20);
  map<string, double> throughput_cache;
  int proxy_socket, client_sock, server_sock;
  int client_sockets[MAXCLIENTS] = {0};
  int client_servers[MAXCLIENTS] = {0};
  int client_states[MAXCLIENTS] = {0};
  int addrlen, activity, valread;
  vector<struct sockaddr_in> client_addresses(MAXCLIENTS), server_addresses(MAXCLIENTS);

  struct sockaddr_in server_address, proxy_address, client_address;

  if ((proxy_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0)
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
  
  proxy_address.sin_family = AF_INET;
  proxy_address.sin_port = htons(listen_port);
  proxy_address.sin_addr.s_addr = INADDR_ANY;
  spdlog::info("Binding proxy to 0.0.0.0:{}", listen_port);
  if (::bind(proxy_socket, (struct sockaddr *)&proxy_address, sizeof(proxy_address)) < 0)
  {
    perror("bind failed");
    close(proxy_socket);
    exit(EXIT_FAILURE);
  }
  if (listen(proxy_socket, 3) < 0)
  {
    perror("listen");
    close(proxy_socket);
    exit(EXIT_FAILURE);
  }
  spdlog::info("miProxy started");
  char buffer[8192];
  puts("Waiting for connections ...");
  fd_set readfds;
  while (1)
  {
    // clear the socket set
    FD_ZERO(&readfds);

    // add master socket to set
    FD_SET(proxy_socket, &readfds);
    
    // Find the maximum file descriptor for select
    int max_fd = proxy_socket;

    socklen_t addrlen = sizeof(client_address);  

    memset(&client_address, 0, sizeof(client_address)); 
    addrlen = sizeof(client_address);
    
    // 检查和清理无效的套接字
    for (int i = 0; i < MAXCLIENTS; i++) {
      if (client_sockets[i] != 0) {
        // 检查客户端套接字是否有效
        int error = 0;
        socklen_t len = sizeof(error);
        int retval = getsockopt(client_sockets[i], SOL_SOCKET, SO_ERROR, &error, &len);
        if (retval != 0 || error != 0) {
          spdlog::info("Cleaning up invalid client socket {}", client_sockets[i]);
          close(client_sockets[i]);
          client_sockets[i] = 0;
          
          if (client_servers[i] != 0) {
            close(client_servers[i]);
            client_servers[i] = 0;
          }
          continue;
        }
        
        // 检查服务器套接字是否有效
        if (client_servers[i] != 0) {
          retval = getsockopt(client_servers[i], SOL_SOCKET, SO_ERROR, &error, &len);
          if (retval != 0 || error != 0) {
            spdlog::info("Cleaning up invalid server socket {}", client_servers[i]);
            close(client_servers[i]);
            client_servers[i] = 0;
          }
        }
      }
    }

    // 添加有效的套接字到select集合
    for (int i = 0; i < MAXCLIENTS; i++)
    {
      client_sock = client_sockets[i];
      server_sock = client_servers[i];
      if (client_sock != 0)
      {
        FD_SET(client_sock, &readfds);
        max_fd = max(max_fd, client_sock);
        
        if (server_sock != 0) {
          FD_SET(server_sock, &readfds);
          max_fd = max(max_fd, server_sock);
        }
      }
    }
    
    // 设置select超时，避免无限等待
    struct timeval tv;
    tv.tv_sec = 1;  // 1秒超时
    tv.tv_usec = 0;
    
    // 等待活动
    activity = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
    
    // 如果select超时，继续下一次循环
    if (activity == 0) {
      continue;
    }
    
    // 如果select出错
    if (activity < 0) {
      if (errno == EINTR) {
        continue;  // 被信号中断，继续下一次循环
      }
      perror("select error");
      break;  // 其他错误，退出循环
    }

    // 处理新连接
    if (FD_ISSET(proxy_socket, &readfds))
    {
      int new_socket = accept(proxy_socket, (struct sockaddr *)&client_address,
                              &addrlen);
      if (new_socket < 0)
      {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      spdlog::info("New client socket connected with {}:{} on sockfd {}", inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port), new_socket);

      for (int i = 0; i < MAXCLIENTS && client_sockets[i] != new_socket; i++)
      {
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
    
    // 处理现有连接
    for (int i = 0; i < MAXCLIENTS; i++)
    {
      client_sock = client_sockets[i];
      server_sock = client_servers[i];
      
      // 处理客户端数据
      if (client_sock != 0 && FD_ISSET(client_sock, &readfds))
      {
        string client_message, client_ID;
        ssize_t result = read_http(client_sock, client_message, client_ID);
        
        if (result <= 0)
        {
          // 客户端关闭连接或出错
          spdlog::info("Client socket {} disconnected or error", client_sock);
          close(client_sock);
          client_sockets[i] = 0;
          
          if (server_sock != 0) {
            close(server_sock);
            client_servers[i] = 0;
          }
          continue;
        }
        
        cache.push_back(i);
        if (client_message.substr(0, 3) == "GET")
        {
          cout << "{{{{{{ GET message }}}}}}" << endl;
          size_t pos;
          if ((pos = client_message.find("HTTP")) != string::npos)
          {
            string file_addr = client_message.substr(4, pos - 5);
            
            // 处理所有JavaScript文件
            if (file_addr.find(".js") != string::npos) {
              spdlog::info("Handling JavaScript request: {}", file_addr);
              
              // 直接转发请求到服务器
              send_message(server_sock, client_message);
              spdlog::info("JavaScript request forwarded to server");
              
              // 简单地读取服务器响应并转发给客户端
              char buffer[65536]; // 使用大缓冲区
              int total_bytes = 0;
              string response;
              
              // 设置阻塞模式
              int flags = fcntl(server_sock, F_GETFL, 0);
              fcntl(server_sock, F_SETFL, flags & ~O_NONBLOCK);
              
              // 设置超时
              struct timeval tv;
              tv.tv_sec = 5; // 5秒超时
              tv.tv_usec = 0;
              setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
              
              // 读取数据直到连接关闭或超时
              while (true) {
                int bytes = recv(server_sock, buffer, sizeof(buffer), 0);
                if (bytes <= 0) {
                  if (bytes == 0) {
                    spdlog::info("Server closed connection after sending {} bytes", total_bytes);
                  } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    spdlog::info("Timeout reached after reading {} bytes", total_bytes);
                  } else {
                    spdlog::error("Error reading from server: {}", strerror(errno));
                  }
                  break;
                }
                
                response.append(buffer, bytes);
                total_bytes += bytes;
                spdlog::info("Read {} bytes from server, total: {}", bytes, total_bytes);
              }
              
              // 恢复非阻塞模式
              fcntl(server_sock, F_SETFL, flags);
              
              // 重置超时
              tv.tv_sec = 0;
              tv.tv_usec = 0;
              setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
              
              if (total_bytes > 0) {
                spdlog::info("Sending {} bytes to client", response.length());
                send_message(client_sock, response);
                spdlog::info("JavaScript response sent to client");
              } else {
                spdlog::error("No data received from server for JavaScript request");
              }
            }
            else if (file_addr.length() >= 7 && file_addr.substr(file_addr.length() - 7, 7) == "vid.mpd")
            {
              spdlog::info("Handling MPD manifest request: {}", file_addr);
              
              // 直接转发请求到服务器
              send_message(server_sock, client_message);
              client_states[i] = 1;
              string file_addr_mod = file_addr.substr(0, file_addr.length() - 4).append("-no-list.mpd");
              
              spdlog::info("Manifest requested by {} forwarded to {}:{} for {}", client_ID, inet_ntoa(server_addresses[i].sin_addr), ntohs(server_addresses[i].sin_port), file_addr_mod); 
              request_cache[i] = string("GET ") + file_addr_mod + client_message.substr(pos - 1);
              
              // 立即读取服务器响应
              string server_response, temp_id;
              spdlog::info("Reading MPD response from server");
              
              // 使用阻塞模式读取
              int flags = fcntl(server_sock, F_GETFL, 0);
              fcntl(server_sock, F_SETFL, flags & ~O_NONBLOCK);
              
              // 设置超时
              struct timeval tv;
              tv.tv_sec = 5; // 5秒超时
              tv.tv_usec = 0;
              setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
              
              // 读取数据直到连接关闭或超时
              char buffer[65536]; // 使用大缓冲区
              int total_bytes = 0;
              
              while (true) {
                int bytes = recv(server_sock, buffer, sizeof(buffer), 0);
                if (bytes <= 0) {
                  if (bytes == 0) {
                    spdlog::info("Server closed connection after sending {} bytes", total_bytes);
                  } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    spdlog::info("Timeout reached after reading {} bytes", total_bytes);
                  } else {
                    spdlog::error("Error reading from server: {}", strerror(errno));
                  }
                  break;
                }
                
                server_response.append(buffer, bytes);
                total_bytes += bytes;
                spdlog::info("Read {} bytes from server, total: {}", bytes, total_bytes);
              }
              
              // 恢复非阻塞模式
              fcntl(server_sock, F_SETFL, flags);
              
              // 重置超时
              tv.tv_sec = 0;
              tv.tv_usec = 0;
              setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
              
              if (total_bytes > 0) {
                // 解析MPD文件，提取带宽信息
                size_t content_pos = server_response.find("\r\n\r\n");
                if (content_pos != string::npos && !client_ID.empty()) {
                  string content = server_response.substr(content_pos + 4);
                  bandwidths[client_ID] = get_available_bandwidths(content);
                  sort(bandwidths[client_ID].begin(), bandwidths[client_ID].end());
                  spdlog::info("Extracted {} bandwidth options for client {}: ", bandwidths[client_ID].size(), client_ID);
                  for (size_t j = 0; j < bandwidths[client_ID].size(); j++) {
                    spdlog::info("  Bandwidth option {}: {} Kbps", j, bandwidths[client_ID][j]);
                  }
                }
                
                // 发送修改后的请求到服务器
                spdlog::info("Sending modified request to server: {}", request_cache[i]);
                send_message(server_sock, request_cache[i]);
                
                // 读取修改后的MPD响应
                server_response.clear();
                total_bytes = 0;
                
                while (true) {
                  int bytes = recv(server_sock, buffer, sizeof(buffer), 0);
                  if (bytes <= 0) {
                    if (bytes == 0) {
                      spdlog::info("Server closed connection after sending {} bytes", total_bytes);
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                      spdlog::info("Timeout reached after reading {} bytes", total_bytes);
                    } else {
                      spdlog::error("Error reading from server: {}", strerror(errno));
                    }
                    break;
                  }
                  
                  server_response.append(buffer, bytes);
                  total_bytes += bytes;
                  spdlog::info("Read {} bytes from server, total: {}", bytes, total_bytes);
                }
                
                // 将修改后的MPD响应发送给客户端
                spdlog::info("Forwarding modified MPD response to client ({} bytes)", server_response.length());
                send_message(client_sock, server_response);
              } else {
                spdlog::error("No data received from server for MPD request");
              }
              
              // 清除请求缓存
              request_cache[i].clear();
              // 重置状态
              client_states[i] = 0;
            }
            else if (file_addr.length() >= 5 && file_addr.substr(file_addr.length() - 4, 4) == ".m4s") 
            {
              spdlog::info("Handling video segment request: {}", file_addr);
              size_t pos_a = string::npos, pos_b = file_addr.rfind('/'), pos_c = string::npos, pos_d = string::npos;
              int flag = 0;
              if (pos_b != string::npos) {
                pos_a = file_addr.substr(0, pos_b).rfind('/');
                if (pos_a != string::npos) {
                  if (file_addr.substr(pos_a, 6)=="/video") {
                    pos_d = file_addr.substr(pos_b).find("-seg-");
                    if (pos_d != string::npos) {
                      pos_d += pos_b; 
                      if (file_addr[pos_b+4]=='-') {
                        flag = 1;
                      }
                    }
                  }
                }
              }
              
              if (flag && !bandwidths[client_ID].empty()) {
                if (throughput_cache.find(client_ID) == throughput_cache.end()) {
                  throughput_cache[client_ID] = 0.0;
                  // 如果没有吞吐量数据，使用最低比特率
                  int lowest_bitrate_index = 0;
                  string file_addr_mod = file_addr.substr(0, pos_b+5) + to_string(bandwidths[client_ID][lowest_bitrate_index]) + file_addr.substr(pos_d);
                  spdlog::info("First segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps", client_ID, inet_ntoa(server_addresses[i].sin_addr), ntohs(server_addresses[i].sin_port), file_addr_mod, bandwidths[client_ID][lowest_bitrate_index]); 
                  send_message(server_sock, string("GET ") + file_addr_mod + client_message.substr(pos - 1));
                } else {
                  // 根据吞吐量选择合适的比特率
                  int j = 0;
                  double target_throughput = throughput_cache[client_ID] * 1.5;
                  
                  // 找到不超过目标吞吐量的最高比特率
                  for (int k = 0; k < bandwidths[client_ID].size(); k++) {
                    if (bandwidths[client_ID][k] <= target_throughput) {
                      j = k;
                    } else {
                      break;
                    }
                  }
                  
                  string file_addr_mod = file_addr.substr(0, pos_b+5) + to_string(bandwidths[client_ID][j]) + file_addr.substr(pos_d);
                  spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps (throughput: {} Kbps)", 
                              client_ID, inet_ntoa(server_addresses[i].sin_addr), ntohs(server_addresses[i].sin_port), 
                              file_addr_mod, bandwidths[client_ID][j], throughput_cache[client_ID]); 
                  send_message(server_sock, string("GET ") + file_addr_mod + client_message.substr(pos - 1));
                }
              } else {
                spdlog::info("Forwarding original segment request: {}", file_addr);
                send_message(server_sock, client_message);
              }
            }
            else
            {
              spdlog::info("Forwarding regular GET request: {}", file_addr);
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
          cout << "{{{{{{ POST message }}}}}}" << endl;
          cout << client_message << endl;
          int frag_size = 0, time_start = 0, time_end = 0;
          istringstream header_stream(client_message);
          string line;
          while (getline(header_stream, line) && line != "\r") {
            size_t colon_pos = line.find(':');
            if (colon_pos != string::npos) {
              string key = line.substr(0, colon_pos);
              string value = line.substr(colon_pos + 2);
              if (strcasecmp(key.c_str(), "x-fragment-size")==0){
                frag_size = stoul(value);
              }
              else if (strcasecmp(key.c_str(), "x-timestamp-start")==0) {
                time_start = stoul(value);
              }
              else if (strcasecmp(key.c_str(), "x-timestamp-end")==0) {
                time_end = stoul(value);
              }
            }
          }
          double throughput = (double)frag_size / (time_end - time_start);
          if (throughput_cache.find(client_ID) == throughput_cache.end()) {
            throughput_cache[client_ID] = 0;
          }
          throughput_cache[client_ID] = alpha * throughput + (1 - alpha) * throughput_cache[client_ID]; 
          spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. Throughput: {} Kbps. Avg Throughput: {} Kbps", client_ID, frag_size, time_end - time_start, throughput, throughput_cache[client_ID]); 
          send_message(client_sock, string("HTTP/1.1 200 OK\r\n\r\n"));
        }
        else
        {
          send_message(server_sock, client_message);
        }
      }
      
      // 处理服务器数据
      if (server_sock != 0 && FD_ISSET(server_sock, &readfds))
      {
        spdlog::info("bingo proxy need read data from server sock {}", server_sock);
        string temp;
        string server_message;
        
        if (client_states[i] == 0)
        {
          spdlog::info("Reading regular response from server");
          ssize_t bytes_read = read_http(server_sock, server_message, temp);
          
          if (bytes_read > 0) {
            spdlog::info("Forwarding {} bytes from server to client", server_message.length());
            
            // 检查是否是JavaScript响应
            if (server_message.find("Content-Type: application/javascript") != string::npos || 
                server_message.find("Content-Type: text/javascript") != string::npos ||
                server_message.find(".js") != string::npos) {
              spdlog::info("Detected JavaScript response");
            }
            
            // 发送响应给客户端
            ssize_t sent = send_message(client_sock, server_message);
            if (sent != server_message.length()) {
              spdlog::error("Failed to send complete response to client: sent {} of {} bytes", 
                           sent, server_message.length());
            } else {
              spdlog::info("Successfully sent response to client");
            }
          } else if (bytes_read == 0) {
            spdlog::info("Server closed connection");
          } else {
            spdlog::error("Error reading from server");
          }
        }
      }
    }
  }
  return 0;
}