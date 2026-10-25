//
// Created by guijanson on 10/23/26.
//

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "Server.h"
#include "FileDescriptor.h"

void echoMessage(int fd)
{
  std::cout << std::format("Message received: {}", readUntil(fd, '\n')) << std::endl;
}

int main()
{
  Server server("0.0.0.0", 8080);
  FileDescriptor epoll_fd(epoll_create1(0));
  struct epoll_event event;
  struct epoll_event events[MAX_EVENTS];

  try {
    server.start();
  } catch (const std::exception &e) {
    // Handle exception
    std::cout << std::format("{}", e.what()) << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << std::format("Server is listening on port {}...", server.getPort()) << std::endl;

  event.events = EPOLLIN;
  event.data.fd = server.getUnderlying();

  // add server to epoll
  epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, server.getUnderlying(), &event);

  for (;;) {
    int nfds = epoll_wait(epoll_fd.get(), events, MAX_EVENTS, -1);
    if (nfds == -1) {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    for (int n = 0; n < nfds; ++n) {
      if (events[n].data.fd == server.getUnderlying()) {
        const FileDescriptor &client_fd = server.accept();

        if (client_fd.get() < 0) {
          perror("accept");
          return EXIT_FAILURE;
        }

        // setnonblocking(server.getClient(0).get());
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = client_fd.get();
        if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, client_fd.get(), &event) ==
            -1) {
          perror("epoll_ctl: conn_sock");
          return EXIT_FAILURE;
        }
      } else {
        echoMessage(events[n].data.fd);
      }
    }
  }
  return EXIT_SUCCESS;
}
