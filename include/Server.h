//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#define MAX_CONNECTIONS 10
#define MAX_EVENTS 10

#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <vector>

#include "FileDescriptor.h"

inline std::string readUntil(int fd, char cEnd)
{
    char c = ' ';
    std::string buffer;
    while (c != cEnd)
    {
        ssize_t chars_read = read(fd, &c, sizeof(char));
        if (chars_read <= 0)
        {
            throw std::runtime_error("Failed to read from socket");
        }
        if (c != cEnd)
        {
            buffer += c;
        }
    }
    return buffer;
}


class Server
{
public:
    Server(std::string ip, int port) : m_ip(std::move(ip)), m_port(port)
    {
    }

    void start()
    {
        struct sockaddr_in s_addr{};

        int temp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (temp_socket < 0)
        {
            throw std::runtime_error("Failed to create socket");
        }

        m_socket = FileDescriptor(temp_socket);

        std::memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = htons(m_port);

        // s_addr.sin_addr.s_addr = INADDR_ANY;
        if (inet_pton(AF_INET, m_ip.c_str(), &s_addr.sin_addr) < 0)
        {
            throw std::runtime_error("Invalid IP address");
        }
        if (bind(m_socket.get(), reinterpret_cast<sockaddr*>(&s_addr),
                 sizeof(s_addr)) < 0)
        {
            throw std::runtime_error("Failed to bind socket");
        }

        ::listen(m_socket.get(), MAX_CONNECTIONS);
    }

    const FileDescriptor& accept()
    {
        if (m_clients.size() == MAX_CONNECTIONS)
        {
            throw std::runtime_error("Too many clients");
        }

        const int fd_temp = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd_temp < 0)
        {
            throw std::runtime_error("Failed to accept connection");
        }

        std::cout << std::format("Connection accepted: {}", std::to_string(fd_temp)) << std::endl;

        m_clients.emplace_back(fd_temp);
        return m_clients.back();
    }

    [[nodiscard]] std::string ReadMessage() const
    {
        return readUntil(m_clients.front().get(), '\n');
    }

    void close(int fd)
    {
        auto it = std::remove_if(m_clients.begin(), m_clients.end(), [fd](FileDescriptor& client)
        {
            return client.get() == fd;
        });

        m_clients.erase(it, m_clients.end());
    }

    [[nodiscard]] std::string getIp() const { return m_ip; }

    [[nodiscard]] int getPort() const { return m_port; }

    [[nodiscard]] const FileDescriptor& get() const { return m_socket; }

    [[nodiscard]] int getUnderlying() const { return m_socket.get(); }

private:
    FileDescriptor m_socket;
    std::string m_ip;
    std::vector<FileDescriptor> m_clients;
    int m_port;
};


#endif //SERVER_SERVER_H
