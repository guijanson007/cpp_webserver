#pragma once

#include <arpa/inet.h>
#include <common/FileDescriptor.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

class Client
{
public:
    Client(std::string ip, int port) : m_ip(std::move(ip)), m_port(port), m_socket() {}
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    void start()
    {
        struct sockaddr_in s_addr{};
        int fd_socket = -1;

        fd_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_socket < 0)
        {
            throw std::runtime_error("ERROR: Could not create socket\n");
        }

        memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = ::htons(m_port);
        if (::inet_pton(AF_INET, m_ip.c_str(), &s_addr.sin_addr) != 1)
        {
            ::close(fd_socket);
            throw std::runtime_error("ERROR: Could not convert IP address\n");
        }

        if (::connect(fd_socket, (struct sockaddr *) &s_addr, sizeof(s_addr)) < 0)
        {
            ::close(fd_socket);
            throw std::runtime_error("ERROR: Could not connect to server\n");
        }
        m_socket = FileDescriptor(fd_socket);
    }

    void sendMessage(const std::string &message)
    {
        if (message.size() > 1028)
        {
            throw std::runtime_error("ERROR: Message size exceeds 1028 bytes\n");
        }
        char buffer[1028] = {0}; // Zero-initializes all 1028 bytes

        // Copy only up to 1028 bytes to prevent buffer overflow
        std::memcpy(buffer, message.data(), std::min(message.size(), size_t{1028}));

        ssize_t bytes_sent = ::send(m_socket.get(), buffer, 1028, 0);
        if (bytes_sent < 0)
        {
            throw std::runtime_error("ERROR: Could not send message\n");
        }
    }

private:
    std::string m_ip;
    int m_port;
    FileDescriptor m_socket;
};
