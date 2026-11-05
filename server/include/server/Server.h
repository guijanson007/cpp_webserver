//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H


#include <algorithm>
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <common/FileDescriptor.h>
#include "network/Connection.h"
#include "network/EpollReactor.h"

class Server
{
public:
    Server(std::string ip, int port, network::EpollReactor &reactor)
        : m_ip(std::move(ip)), m_port(port), m_reactor(reactor)
    {
        m_clients.reserve(MAX_CONNECTIONS);
    }
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    void start()
    {
        struct sockaddr_in s_addr{};

        int temp_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (temp_socket < 0)
        {
            throw std::runtime_error("Failed to create socket");
        }

        m_socket = FileDescriptor(temp_socket);

        std::memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = ::htons(m_port);

        // s_addr.sin_addr.s_addr = INADDR_ANY;
        if (1 != ::inet_pton(AF_INET, m_ip.c_str(), &s_addr.sin_addr))
        {
            throw std::runtime_error("Invalid IP address");
        }
        int opt = 1;
        // set socket options to allow address reuse
        if (0 > ::setsockopt(m_socket.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        {
            throw std::runtime_error("Failed to set socket options");
        }

        if (0 > ::bind(m_socket.get(), reinterpret_cast<sockaddr *>(&s_addr), sizeof(s_addr)))
        {
            throw std::runtime_error("Failed to bind socket");
        }

        // backlog size is set to 1000 times the max connections
        if (0 > ::listen(m_socket.get(), MAX_CONNECTIONS * 1000))
        {
            throw std::runtime_error("Failed to listen on socket");
        }

        // register the server socket with the epoll reactor
        if (!m_reactor.add(m_socket.get(), EPOLLIN, [this](int fd) { this->accept(); }))
        {
            throw std::runtime_error("Failed to add server socket to epoll reactor");
        }
    }

    void accept()
    {
        int fd_temp{};

        while (true)
        {
            fd_temp = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd_temp < 0)
            {
                if (errno == EAGAIN)
                {
                    return; // no more connections to accept
                }

                if (errno != EINTR)
                {
                    std::cout << std::format("Failed to accept connection: {}", std::strerror(errno)) << std::endl;
                    return;
                }
                else
                {
                    // retry if interrupted by signal
                    continue;
                }
            }
            else // successfully accepted a new connection
            {
                if (m_clients.size() == MAX_CONNECTIONS)
                {
                    std::cout << "Max connections reached. Closing the new connection." << std::endl;
                    ::close(fd_temp);
                    continue; // keep draining the accept queue
                }

                network::Connection new_client(fd_temp);
                if (!m_reactor.add(fd_temp, EPOLLIN | EPOLLET, [this](int fd) { this->clientCallback(fd); }))
                {
                    return; // let the client socket be closed by the destructor of new_client
                }
                std::cout << std::format("Connection accepted: {}", std::to_string(fd_temp)) << std::endl;
                m_clients.insert({fd_temp, std::move(new_client)});
            }
        }
    }


    void closeClientConnection(int fd) { m_clients.erase(fd); }

    [[nodiscard]] std::string getIp() const { return m_ip; }

    [[nodiscard]] int getPort() const { return m_port; }

    [[nodiscard]] int get() const { return m_socket.get(); }

private:
    void clientCallback(int client_fd)
    {
        auto client_it = m_clients.find(client_fd);

        if (client_it == m_clients.end())
        {
            std::cout << std::format("Client with fd {} not found", client_fd) << std::endl;
            return;
        }
        auto &client = client_it->second;

        auto &buf = client.getReadBuffer();
        char tmp[1028];
        bool closed = false;

        for (;;)
        {
            const ssize_t r = read(client_fd, tmp, sizeof(tmp));
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
            // remove the client from the epoll reactor
            if (!m_reactor.remove(client_fd))
            {
                // unhandled error, but we still want to close the connection
            }
            closeClientConnection(client_fd); // destroy the fd
        }
    }

private:
    static constexpr int MAX_CONNECTIONS{10};
    FileDescriptor m_socket;
    std::string m_ip;
    std::unordered_map<int, network::Connection> m_clients;
    int m_port;
    network::EpollReactor &m_reactor;
};


#endif // SERVER_SERVER_H
