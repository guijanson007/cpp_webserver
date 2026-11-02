//
// Created by guijanson on 10/23/26.
//

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <sys/epoll.h>
#include <unordered_map>

#include <common/FileDescriptor.h>
#include <server/Server.h>

static constexpr int MAX_EVENTS{10};

int main()
{
    Server server("0.0.0.0", 8080);
    FileDescriptor epoll_fd(epoll_create1(0));
    struct epoll_event event{};
    struct epoll_event events[MAX_EVENTS];

    try
    {
        server.start();
    }
    catch (const std::exception &e)
    {
        // Handle exception
        std::cout << std::format("{}", e.what()) << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << std::format("Server is listening on port {}...", server.getPort()) << std::endl;

    event.events = EPOLLIN;
    event.data.fd = server.getUnderlying();

    // add server to epoll
    if (-1 == epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, server.getUnderlying(), &event))
    {
        perror("epoll_ctl: server_fd");
        return EXIT_FAILURE;
    }

    // buffers map
    std::unordered_map<int, std::string> buffers;

    while (true)
    {
        int nfds = epoll_wait(epoll_fd.get(), events, MAX_EVENTS, -1);
        if (nfds == -1)
        {
            perror("epoll_wait");
            return EXIT_FAILURE;
        }

        for (int n = 0; n < nfds; ++n)
        {
            if (events[n].data.fd == server.getUnderlying())
            {

                while (std::optional<int> opt_fd = server.accept())
                {
                    if (opt_fd.value() == -1)
                    {
                        continue; // reach max connections, rejected new connection
                    }

                    int client_fd = opt_fd.value();
                    event.events = EPOLLIN | EPOLLET;
                    event.data.fd = client_fd;
                    if (-1 == epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, client_fd, &event))
                    {
                        std::cout << std::format("Failed to add client socket {} to epoll", client_fd) << std::endl;
                        close(client_fd);
                        continue;
                    }
                }
            }
            else
            {
                int fd = events[n].data.fd;
                auto &buf = buffers[fd];
                char tmp[1028];
                bool closed = false;
                for (;;)
                {
                    const ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0)
                    {
                        buf.append(tmp, r);
                        continue;
                    }
                    if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    {
                        closed = true; // still deliver what was buffered before the peer hung up
                    }
                    break; // closed, or EAGAIN: drained
                }
                for (size_t pos; (pos = buf.find('\0')) != std::string::npos;)
                {
                    if (pos > 0) // zero padding of the fixed-size frame yields empty messages
                    {
                        std::cout << std::format("Message received: {}", buf.substr(0, pos)) << std::endl;
                    }
                    buf.erase(0, pos + 1);
                }
                if (closed)
                {
                    if (-1 == epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, fd, nullptr))
                    {
                        std::cout << std::format("Failed to remove client socket {} from epoll", fd) << std::endl;
                    }
                    buffers.erase(fd);
                    server.closeClientConnection(fd); // destroy the fd
                }
            }
        }
    }
    return EXIT_SUCCESS;
}
