//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H


#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <common/FileDescriptor.h>


class Server
{
public:
    Server(std::string ip, int port) : m_ip(std::move(ip)), m_port(port) { m_clients.reserve(MAX_CONNECTIONS); }
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
        if (0 > ::bind(m_socket.get(), reinterpret_cast<sockaddr *>(&s_addr), sizeof(s_addr)))
        {
            throw std::runtime_error("Failed to bind socket");
        }

        if (0 > ::listen(m_socket.get(), MAX_CONNECTIONS))
        {
            throw std::runtime_error("Failed to listen on socket");
        }
    }

    std::optional<int> accept()
    {
        int fd_temp{};

        while (true)
        {
            fd_temp = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd_temp < 0)
            {
                if (errno == EINTR)
                {
                    // retry
                    continue;
                }

                if (errno != EAGAIN)
                {
                    std::cout << std::format("Failed to accept connection: {}", std::strerror(errno)) << std::endl;
                }
                return std::nullopt;
            }
            break;
        }

        std::cout << std::format("Connection accepted: {}", std::to_string(fd_temp)) << std::endl;

        if (m_clients.size() == MAX_CONNECTIONS)
        {
            std::cout << "Max connections reached. Closing the new connection." << std::endl;
            ::close(fd_temp);
            return {-1}; // Return -1 to indicate that the connection was closed due to max connections reached
        }


        m_clients.emplace_back(fd_temp);
        return {m_clients.back().get()};
    }


    void closeClientConnection(int fd)
    {
        auto it = std::remove_if(m_clients.begin(), m_clients.end(),
                                 [fd](FileDescriptor &client) { return client.get() == fd; });

        m_clients.erase(it, m_clients.end());
    }

    [[nodiscard]] std::string getIp() const { return m_ip; }

    [[nodiscard]] int getPort() const { return m_port; }

    [[nodiscard]] const FileDescriptor &get() const { return m_socket; }

    [[nodiscard]] int getUnderlying() const { return m_socket.get(); }

private:
    static constexpr int MAX_CONNECTIONS{10};
    FileDescriptor m_socket;
    std::string m_ip;
    std::vector<FileDescriptor> m_clients;
    int m_port;
};


#endif // SERVER_SERVER_H
