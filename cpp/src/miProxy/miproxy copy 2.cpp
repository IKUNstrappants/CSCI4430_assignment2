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
using namespace std;

#define MAXCLIENTS 30

void set_nonblocking(int socket)
{
  int flags = fcntl(socket, F_GETFL, 0);
  fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

void send_message(int socket, string message) {
  send(socket, message.c_str(), message.length(), 0);
}

int read_wrap(int socket, char *buffer, size_t length, int &readlen)
{
  readlen = read(socket, buffer, length);
  cout << "read " << readlen << " bytes" << endl;
  if (readlen < 0) {
    cerr << "read " << length << " bytes on socket " << socket << " failed" << endl;
    return -1;
  }
  buffer[readlen] = '\0';
  return strlen(buffer);
  // cout << buffer;
}

void read_http(int origin_sock, string &buffer)
{
  if (false)
  {
    char temp[8192] = "";
    int readlen1;
    read_wrap(origin_sock, temp, 8192, readlen1);
    buffer = temp;
    return;
  }
  set_nonblocking(origin_sock);
  char temp[1024] = "";
  string header, content;
  int readlen = 1, content_length = 0;
  buffer.clear();
  //cout << "run read http loop" << endl;
  while (true)
  {
    int length_read = read_wrap(origin_sock, temp, 1024, readlen);
    //cout << "readlen = " << readlen << endl;
    header.append(temp);
    size_t pos = header.find("\r\n\r\n");
    //cout << "pos = " << pos << endl;
    if (pos == string::npos) continue;
    else {
      pos += 4;
      cout << "end of header reached, last readlen=" << readlen << endl;
      if (readlen < 1024 || pos == header.length()) {
        break;
      }
      cout << "header.length()=" << header.length() << ", pos=" << pos << endl;
      content = header.substr(pos);
      header = header.substr(0, pos);
      string line;
      istringstream stream(header);
      while (getline(stream, line) && (line != "\r"))
      {
        cout << "[ " << line.substr(0, line.length() - 1) << " ]" << endl;
        size_t colon_pos = line.find(':');
        //cout << line.substr(0, colon_pos) << endl;
        if (colon_pos != string::npos) {
          if (colon_pos==16 && strcasecmp(line.substr(0, colon_pos).c_str(), "Content-Length")==0)
          {
            content_length = stoul(line.substr(colon_pos + 2));
            cout << "content-length=" << content_length << endl;
            break;
          }
        }
      }
      break;
    }
  }
  content_length -= content.length() - 1;
  cout << "loop finish, read remaining content" << endl;
  while (content_length >= 1024)
  {
    read_wrap(origin_sock, temp, 1024, readlen);
    content.append(temp);
    content_length -= 1024;
  }
  if (content_length > 0)
  {
    read_wrap(origin_sock, temp, content_length, readlen);
    content.append(temp);
  }
  buffer = header + content;
}

bool redirect_request(string buffer, struct sockaddr_in &dest_addr, int dest_socket)
{
  const char *new_buffer = buffer.c_str();
  ssize_t bytes_sent = send(dest_socket, new_buffer, strlen(new_buffer), 0);
  if (bytes_sent < 0)
    cout << "redirect send failed" << endl;
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

  printf("-----server Listening on port %d-----\n", ntohs(address->sin_port));
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

    // Your proxy server implementation goes here
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
  vector<string> request_cache(20), socket_bitrates(20);
  int proxy_socket, addrlen, activity, valread;
  int client_sockets[MAXCLIENTS] = {0};
  int client_states[MAXCLIENTS] = {0};
  int client_servers[MAXCLIENTS] = {0};
  vector<struct sockaddr_in> client_addresses(MAXCLIENTS), server_addresses(MAXCLIENTS);

  int client_sock, server_sock;

  struct sockaddr_in server_address, proxy_address, client_address;

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

  char buffer[8192]; // data buffer of 1KiB + 1 bytes

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
    //cout << "added client sockets to readfds" << endl;
    // wait for an activity on one of the sockets , timeout is NULL ,
    // so wait indefinitely
    activity = select(FD_SETSIZE, &readfds, nullptr, nullptr, nullptr);
    //cout << "select finish" << endl;
    if ((activity < 0) && (errno != EINTR))
    {
      cout << "no activity present, proxy terminated" << endl;
      perror("select error");
    }

    //=======================================================================================
    if (FD_ISSET(proxy_socket, &readfds))
    {
      //cout << "proxy: incoming connection" << endl;
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
    //cout << "proxy operations done" << endl;

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
        cout << "check client[" << i << ']' << endl;
        // Check if it was for closing , and also read the
        string client_message;
        // read(client_sock, buffer, 1024);client_message = buffer;
        read_http(client_sock, client_message);
        cout << "[" << endl
             << client_message << "]" << endl;
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
            size_t pos;
            if ((pos = client_message.find("HTTP")) != string::npos) {
              string file_addr = client_message.substr(4, pos - 5);
              cout << "file_addr: " << file_addr << endl;
              if (file_addr.length() >= 7 && file_addr.substr(file_addr.length()-7, 7) == "vid.mpd")
              {
                send_message(server_sock, client_message);
                client_states[i] = 1;
                string file_addr_mod = file_addr.substr(0, file_addr.length()-4).append("-no-list.mpd");
                cout << "modified addr: " << file_addr_mod << endl;
                cout << "modified request: " << string("GET ") + file_addr_mod + client_message.substr(pos - 1) << endl;
                request_cache[i] = string("GET ") + file_addr_mod + client_message.substr(pos - 1);
                //send_message(server_sock, string("GET ") + file_addr_mod + client_message.substr(pos - 1));
              }
              else if (false && (client_message.substr(client_message.length() - 3, 3) == "m4s"))
              {
                send_message(server_sock, client_message);
              }
              else
              {
                send_message(server_sock, client_message);
              }
            }
            else {
              send_message(server_sock, client_message);
            }
          }
          else if (client_message.substr(0, 4) == "POST")
          {
            cout << "{{{{{{ POST message }}}}}}" << endl;
            send_message(server_sock, client_message);
            // send(server_socket, buffer, strlen(buffer), 0);
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
        cout << "check server[" << i << ']' << endl;
        string server_message;
        int readlen = 0;
        // read_wrap(server_sock, buffer, 4096, readlen);server_message = buffer;
        if (client_states[i]==0) {
          read_http(server_sock, server_message);
          send_message(client_sock, server_message);
        }
        else if (client_states[i]==1) {
          client_states[i] = 0;
          //read_http(server_sock, server_message);
          //cout << server_message << endl;
          server_message.clear();
          read_http(server_sock, server_message);
          socket_bitrates[i] = server_message;
          send_message(server_sock, request_cache[i]);
          request_cache[i].clear();
          cout << server_message << endl;
        }
        cout << "server return message: [" << endl
             << server_message << endl
             << "]" << endl;
        // redirect_request(server_message, client_addresses[i], client_sock);
      }
    }
  }
  return 0;
}