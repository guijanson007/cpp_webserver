#include <cstring>
#include <vector>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_CONNECTIONS 10

std::string readUntil(int fd, char cEnd)
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


class FileDescriptor
{
public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd)
        : m_fd(fd)
    {
    }

    ~FileDescriptor()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&o) noexcept
        : m_fd(std::exchange(o.m_fd, -1))
    {
    }

    FileDescriptor &operator=(FileDescriptor &&o) noexcept
    {
        if (this != &o)
        {
            if (m_fd >= 0)
                ::close(m_fd);
            m_fd = std::exchange(o.m_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const { return m_fd; }

private:
    int m_fd = -1;
};

class Server
{
public:
    Server(std::string ip, int port)
        : m_ip(std::move(ip)),
          m_port(port)
    {
    }

    void listen()
    {
        struct sockaddr_in s_addr{};

        int temp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (temp_socket < 0)
        {
            throw std::runtime_error("Failed to create socket");
        }

        m_socket = FileDescriptor(temp_socket);

        memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = htons(m_port);

        // s_addr.sin_addr.s_addr = INADDR_ANY;
        if (inet_pton(AF_INET, m_ip.c_str(), &s_addr.sin_addr) < 0)
        {
            throw std::runtime_error("Invalid IP address");
        }
        if (bind(m_socket.get(), reinterpret_cast<sockaddr *>(&s_addr),
                 sizeof(s_addr)) < 0)
        {
            throw std::runtime_error("Failed to bind socket");
        }

        ::listen(m_socket.get(), MAX_CONNECTIONS);
    }

    void accept()
    {
        if (m_clients.size() == MAX_CONNECTIONS)
        {
            throw std::runtime_error("Too many clients");
        }

        const int fd_temp = ::accept(m_socket.get(), nullptr, nullptr);

        if (fd_temp < 0)
        {
            throw std::runtime_error("Failed to accept connection");
        }
        m_clients.push_back(FileDescriptor(fd_temp));
    }

    [[nodiscard]] std::string ReadMessage() const
    {
        return readUntil(m_clients.front().get(), '\n');
    }

    [[nodiscard]] std::string getIp() const { return m_ip; }

    [[nodiscard]] int getPort() const { return m_port; }

private:
    FileDescriptor m_socket;
    std::string m_ip;
    std::vector<FileDescriptor> m_clients;
    int m_port;
};

int main()
{
    Server server("0.0.0.0", 8080);

    try
    {
        server.listen();
    }
    catch (const std::exception &e)
    {
        // Handle exception
        std::println("{}", e.what());
        return EXIT_FAILURE;
    }
    std::println("Server is listening on port {}...", server.getPort());
    server.accept();
    while (true)
    {
        // Accept connections and handle them
        std::println("Message received: {}", server.ReadMessage());
    }
    return EXIT_SUCCESS;
}