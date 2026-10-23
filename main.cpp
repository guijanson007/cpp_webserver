//
// Created by guijanson on 10/23/26.
//

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
#include <sys/epoll.h>
#include <fcntl.h>

#define MAX_CONNECTIONS 10
#define MAX_EVENTS 10

std::string readUntil(int fd, char cEnd) {
    char c = ' ';
    std::string buffer;
    while (c != cEnd) {
        ssize_t chars_read = read(fd, &c, sizeof(char));
        if (chars_read <= 0) {
            throw std::runtime_error("Failed to read from socket");
        }
        if (c != cEnd) {
            buffer += c;
        }
    }
    return buffer;
}


class FileDescriptor {
public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd) :
        m_fd(fd) {
    }

    ~FileDescriptor() {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&o) noexcept :
        m_fd(std::exchange(o.m_fd, -1)) {
    }

    FileDescriptor &operator=(FileDescriptor &&o) noexcept {
        if (this != &o) {
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

class Server {
public:
    Server(std::string ip, int port) :
        m_ip(std::move(ip)),
        m_port(port) {
    }

    void listen() {
        struct sockaddr_in s_addr{};

        int temp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (temp_socket < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        m_socket = FileDescriptor(temp_socket);

        memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = htons(m_port);

        // s_addr.sin_addr.s_addr = INADDR_ANY;
        if (inet_pton(AF_INET, m_ip.c_str(), &s_addr.sin_addr) < 0) {
            throw std::runtime_error("Invalid IP address");
        }
        if (bind(m_socket.get(), reinterpret_cast<sockaddr *>(&s_addr),
                 sizeof(s_addr)) < 0) {
            throw std::runtime_error("Failed to bind socket");
        }

        ::listen(m_socket.get(), MAX_CONNECTIONS);
    }

    void accept() {
        if (m_clients.size() == MAX_CONNECTIONS) {
            throw std::runtime_error("Too many clients");
        }

        const int fd_temp = ::accept(m_socket.get(), nullptr, nullptr);

        if (fd_temp < 0) {
            throw std::runtime_error("Failed to accept connection");
        }

        std::println("Connection accepted: {}", std::to_string(fd_temp));

        m_clients.emplace_back(fd_temp);
    }

    [[nodiscard]] std::string ReadMessage() const {
        return readUntil(m_clients.front().get(), '\n');
    }

    [[nodiscard]] std::string getIp() const { return m_ip; }

    [[nodiscard]] int getPort() const { return m_port; }

    [[nodiscard]] const FileDescriptor &getClient(int pos) const {
        return m_clients[pos];
    }

    [[nodiscard]] const FileDescriptor &get() const {
        return m_socket;
    }

private:
    FileDescriptor m_socket;
    std::string m_ip;
    std::vector<FileDescriptor> m_clients;
    int m_port;
};

void do_use_fd(int fd) {
    std::println("Message received: {}", readUntil(fd, '\n'));
}

int main() {
    Server server("0.0.0.0", 8080);
    FileDescriptor epoll_fd(epoll_create1(0));
    struct epoll_event event;
    struct epoll_event events[1];

    try {
        server.listen();
    } catch (const std::exception &e) {
        // Handle exception
        std::println("{}", e.what());
        return EXIT_FAILURE;
    }
    std::println("Server is listening on port {}...", server.getPort());


    event.events = EPOLLIN;
    event.data.fd = server.get().get();

    // add server to epoll
    epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, server.get().get(), &event);

    for (;;) {
        int nfds = epoll_wait(epoll_fd.get(), events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == server.get().get()) {
                server.accept();

                int client_fd = server.getClient(0).get();

                //setnonblocking(server.getClient(0).get());
                event.events = EPOLLIN | EPOLLET;
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, client_fd,
                              &event) == -1) {
                    perror("epoll_ctl: conn_sock");
                    return EXIT_FAILURE;
                }
            } else {
                do_use_fd(events[n].data.fd);
            }
        }
    }
    return EXIT_SUCCESS;
}
