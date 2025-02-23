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
using namespace std;


#define MAXCLIENTS 30

std::string modify_http_request(const string &request, const string &new_host, int port, int content_len=0)
{
  istringstream request_stream(request);
  ostringstream modified_request;
  string line;
  
  //modified_request << to_string(content_len) << "\r\n";
  while (getline(request_stream, line))
  {
    if (line.find("Host:") == 0)
    {
      modified_request << "Host: " << new_host << ':' << port << "\r\n";
    }
    else
    {
      modified_request << line << "\r\n";
    }
  }

  return modified_request.str();
}

void read_wrap(int socket, char* buffer, size_t length, int &readlen) {
    readlen = read(socket, buffer, length);
    if (readlen < 0) cerr << "read on socket " << socket << "failed" << endl;
    buffer[readlen] = '\0';
    //cout << buffer;
}

void read_http(int origin_sock, string &buffer) {
    char temp[129] = "";
    string header;
    int readlen, content_length = 0;
    buffer.clear();
    while (readlen > 0) {
        header.clear();
        while (temp[0] != '\r') {
            read_wrap(origin_sock, temp, 1, readlen);
            header += temp[0];
        }
        read_wrap(origin_sock, temp, 1, readlen);
        header += temp[0];
        buffer += header;
        if (header=="\r\n") break;
        int pos = header.find(':');
        if (pos != string::npos) {
            string header_name = header.substr(0, pos);
            string content = header.substr(pos+1, header.length()-pos-1);
            cout << header_name << ":" << content << endl;
            if (strcasecmp(header_name.c_str(), "content-length")==0) {
                content_length = stoi(content);
            }
        }
    }
    cout << "header: " << endl << buffer << endl;
    int header_len =  buffer.length();
    cout << "content: " << endl;
    while (content_length >= 128) {
        read_wrap(origin_sock, temp, 128, readlen);
        //cout << "read size = " << readlen << endl;
        buffer += temp;
        content_length -= 128;
    }
    if (content_length > 0) {
        read_wrap(origin_sock, temp, content_length, readlen);
        buffer += temp;
    }
    cout << buffer.substr(header_len, buffer.length() - header_len) << endl;
}

bool redirect_request(string buffer, struct sockaddr_in &dest_addr, int dest_socket)
{
  //const char *ip_str = inet_ntoa(dest_addr.sin_addr);
  //string msg = modify_http_request(buffer, ip_str, ntohs(dest_addr.sin_port));
  const char *new_buffer = buffer.c_str();//msg.c_str();
  //cout << "redirected message: [" << msg << "]" << endl;
  ssize_t bytes_sent = send(dest_socket, new_buffer, strlen(new_buffer), 0);
  if (bytes_sent < 0) cout << "redirect send failed" << endl;
  return bytes_sent >= 0;
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
  int success = setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &yes, sizeof(yes));
  if (success < 0)
  {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  address->sin_family = AF_INET;
  address->sin_port = htons(server_port);
  if (inet_pton(AF_INET, hostname.c_str(), &address->sin_addr) <= 0) {
      perror("Invalid address/ Address not supported");
      return -1;
  }

  // Connect to the server
  if (connect(server_socket, (struct sockaddr *)address, sizeof(*address)) < 0) {
      perror("Connection failed");
      return -1;
  }
  // bind the socket to host port
  /*success = bind(server_socket, (struct sockaddr *)address, sizeof(*address));
  if (success < 0)
  {
    //cout << "bind *&(&(*()))" << endl;
    perror("bind failed");
    exit(EXIT_FAILURE);
  }*/
  printf("-----server Listening on port %d-----\n", ntohs(address->sin_port));

  // try to specify maximum of 3 pending connections for the server socket
  /*if (listen(server_socket, 3) < 0)
  {
    perror("listen");
    exit(EXIT_FAILURE);
  }*/
  return server_socket;
}

int main(int argc, char *argv[])
{
  int opt, listen_port = 0;
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
      std::cout << options.help() << std::endl;
      return 0;
    }

    listen_port = result["listen-port"].as<int>();
    hostname = result["hostname"].as<std::string>();
    server_port = result["port"].as<int>();
    alpha = result["alpha"].as<float>();

    std::cout << "Listen Port: " << listen_port << std::endl;
    std::cout << "Hostname: " << hostname << std::endl;
    std::cout << "Port: " << server_port << std::endl;
    std::cout << "Alpha: " << alpha << std::endl;

    // Your proxy server implementation goes here
  }
  catch (const cxxopts::exceptions::no_such_option &e)
  {
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
  int proxy_socket, addrlen, activity, valread;
  int client_sockets[MAXCLIENTS] = {0};
  int client_states[MAXCLIENTS]  = {0};
  int client_servers[MAXCLIENTS] = {0};
  vector<struct sockaddr_in> client_addresses(MAXCLIENTS), server_addresses(MAXCLIENTS);

  int client_sock, server_sock;

  struct sockaddr_in server_address, proxy_address, client_address;

  //cout << ntohs(server_address.sin_port) << endl;

  if ((proxy_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
  proxy_address.sin_family = AF_INET;
  proxy_address.sin_port = htons(listen_port);
  proxy_address.sin_addr.s_addr = inet_addr(hostname.c_str());
  if (bind(proxy_socket, (struct sockaddr *)&proxy_address, sizeof(proxy_address)) < 0)
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

  char buffer[1025]; // data buffer of 1KiB + 1 bytes


  // accept the incoming connection
  addrlen = sizeof(server_address);
  puts("Waiting for connections ...");
  // set of socket descriptors
  fd_set readfds;

  while (1)
  {
    cout << "===========loop===========" << endl;
    
    // clear the socket set
    FD_ZERO(&readfds);

    // add master socket to set
    FD_SET(proxy_socket, &readfds);
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
    cout << "added client sockets to readfds" << endl;
    // wait for an activity on one of the sockets , timeout is NULL ,
    // so wait indefinitely
    activity = select(FD_SETSIZE, &readfds, nullptr, nullptr, nullptr);
    cout << "select finish" << endl;
    if ((activity < 0) && (errno != EINTR))
    {
      cout << "no activity present, proxy terminated" << endl;
      perror("select error");
    }
    
    //=======================================================================================
    if (FD_ISSET(proxy_socket, &readfds))
    {

      cout << "proxy: incoming connection" << endl;
      int new_socket = accept(proxy_socket, (struct sockaddr *)&client_address,
                              (socklen_t *)&addrlen);
      if (new_socket < 0)
      {
        perror("accept");
        exit(EXIT_FAILURE);
      }

      // inform user of socket number - used in send and receive commands
      printf("\n---New client connection---\n");
      printf("socket fd is %d , ip is : %s , port : %d \n", new_socket,
             inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));

      //  add new socket to the array of sockets
      for (int i = 0; i < MAXCLIENTS && client_sockets[i] != new_socket; i++)
      {
        // if position is empty
        if (client_sockets[i] == 0)
        {
          client_sockets[i] = new_socket;
          client_servers[i] = get_server_socket(&server_addresses[i], server_port, hostname);
          client_addresses[i] = client_address;
          break;
        }
      }
    }
    cout << "proxy operations done" << endl;
        
    //===============================================
    // else it's some IO operation on a client socket
    //===============================================
    for (int i = 0; i < MAXCLIENTS; i++)
    {
      client_sock = client_sockets[i];
      server_sock = client_servers[i];
      // Note: sd == 0 is our default here by fd 0 is actually stdin
      if (client_sock != 0 && FD_ISSET(client_sock, &readfds))
      {
        cout << "check client[" << i << ']' << endl;
        // Check if it was for closing , and also read the
        // incoming message
        getpeername(client_sock, (struct sockaddr *)&client_address,
                    (socklen_t *)&addrlen);
        string client_message;
        read_http(client_sock, client_message);
        cout << "[" << endl << client_message << "]" << endl;
        if (client_message.empty())
        {
          // Somebody disconnected , get their details and print
          printf("\n---client disconnected---\n");
          printf("client disconnected , ip %s , port %d \n",
                 inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
          // Close the socket and mark as 0 in list for reuse
          close(client_sock);
          client_sockets[i] = 0;
          close(server_sock);
          client_servers[i] = 0;
        }
        else
        {
          cache.push_back(i);
          // send the same message back to the client, hence why it's called
          // "echo_server"
          if (client_message.substr(0, 3) == "GET")
          {
            cout << "{{{{{{ GET message }}}}}}" << endl;
            if (false && (client_message.substr(client_message.length() - 3, 3) == "mpd"))
            {
              
            }
            else if (false && (client_message.substr(client_message.length() - 3, 3) == "m4s"))
            {
              send(server_sock, client_message.c_str(), client_message.length(), 0);
            }
            else
            {
              //cout << "[" << client_message << "]" << endl;
              redirect_request(client_message, server_addresses[i], server_sock);
              // cout << send(server_socket, buffer, strlen(buffer), 0) << endl;
              /*ssize_t bytes_sent = send(server_socket, buffer, strlen(buffer), 0);
              if (bytes_sent < 0)
              {
                cout << "send data to server failed" << endl;
                perror("send");
                // Handle the error, possibly close the socket and clean up
              }*/
            }
          }
          else if (client_message.substr(0, 4) == "POST")
          {
            cout << "{{{{{{ POST message }}}}}}" << endl;
            redirect_request(client_message, server_address, server_sock);
            // send(server_socket, buffer, strlen(buffer), 0);
          }
          else
          {
            redirect_request(client_message, server_address, server_sock);
            // send(server_socket, buffer, strlen(buffer), 0);
          }
          // printf("\n---New message---\n\n");
          // printf("%s", buffer);
          // printf("\nReceived from: ip %s , port %d \n", inet_ntoa(server_address.sin_addr), ntohs(server_address.sin_port));
        }
      }
      if (client_sock != 0 && FD_ISSET(server_sock, &readfds)) {
        string server_message = "";
        read_http(server_sock,server_message);
        cout << "server return message: [" << server_message << "]" << endl;
        send(client_sock, server_message.c_str(), server_message.length(), 0);
        //redirect_request(server_message, client_addresses[i], client_sock);
      }
    }
  }
  return 0;
}