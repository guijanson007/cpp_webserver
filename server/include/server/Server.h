//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H


#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <common/FileDescriptor.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include "network/Connection.h"
#include "network/EpollReactor.h"
#include "network/SocketIo.h"
#include "server/ServerConfig.h"

class Server
{
public:
    Server(ServerConfig config, network::EpollReactor &reactor) : m_config(config), m_reactor(reactor)
    {
        m_clients.reserve(config.maxConnections);
    }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    void start()
    {
        struct sockaddr_in s_addr{};

        int spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (spare < 0)
        {
            throw std::runtime_error("Failed to allocate reserve file descriptor");
        }
        m_spare_fd = FileDescriptor(spare);

        int temp_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (temp_socket < 0)
        {
            throw std::runtime_error("Failed to create socket");
        }

        m_socket = FileDescriptor(temp_socket);

        std::memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = ::htons(m_config.port);

        // s_addr.sin_addr.s_addr = INADDR_ANY;
        if (1 != ::inet_pton(AF_INET, m_config.ip.c_str(), &s_addr.sin_addr))
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

        if (0 > ::listen(m_socket.get(), m_config.backlogSize))
        {
            throw std::runtime_error("Failed to listen on socket");
        }

        // register the server socket with the epoll reactor
        uint32_t events = EPOLLIN;
        if (!m_reactor.add(m_socket.get(), events, [this, events](int fd, uint32_t events_in) { this->accept(); }))
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
            network::Connection new_client(fd_temp);
            if (fd_temp < 0)
            {

                if (errno == EMFILE || errno == ENFILE)
                {
                    std::cerr << "Process or system FD limit exhausted. Discarding incoming client." << std::endl;
                    discardExcessConnection();
                    return;
                }

                if (errno == EAGAIN)
                {
                    return; // no more connections to accept
                }

                if (errno != EINTR)
                {
                    std::cerr << std::format("Failed to accept connection: {}", std::strerror(errno)) << std::endl;
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
                if (m_clients.size() == m_config.maxConnections)
                {
                    std::cerr << "Max connections reached. Closing the new connection." << std::endl;
                    new_client.close();
                    continue; // keep draining the accept queue
                }

                uint32_t events = EPOLLIN | EPOLLET; // edge-triggered read events
                if (!m_reactor.add(fd_temp, events,
                                   [this, events](int fd, uint32_t events_in) { this->clientCallback(fd, events_in); }))
                {
                    return; // let the client socket be closed by the destructor of new_client
                }
                std::cout << std::format("Connection accepted: {}", std::to_string(fd_temp)) << std::endl;
                m_clients.insert({fd_temp, std::move(new_client)});
            }
        }
    }

    bool flush(network::Connection &client)
    {
        using namespace network;

        std::string &write_buffer = client.getWriteBuffer();
        if (write_buffer.empty())
        {
            // If the peer already closed their write end (our read end), and
            // our write buffer is fully drained, teardown the connection.
            if (client.isReadClosed())
            {
                closeClientConnection(client.getFd());
                return true;
            }
            return true;
        }

        SocketIoResult result = SocketIo::writeSome(client.getFd(), write_buffer);

        if (result == SocketIoResult::Error || result == SocketIoResult::Eof)
        {
            closeClientConnection(client.getFd());
            return false;
        }

        if (result == SocketIoResult::Again)
        {
            // Register EPOLLOUT. If read side is closed, do NOT register EPOLLIN.
            uint32_t events = EPOLLOUT | EPOLLET;
            if (!client.isReadClosed())
            {
                events |= EPOLLIN;
            }

            if (!m_reactor.modify(client.getFd(), events))
            {
                closeClientConnection(client.getFd());
                return false;
            }

            client.setWaitingForWrite(true);
            return true;
        }

        if (result == SocketIoResult::Ok)
        {
            client.setWaitingForWrite(false);

            // If the read channel is closed, draining all writes means we are done.
            if (client.isReadClosed())
            {
                closeClientConnection(client.getFd());
                return true;
            }

            // Restore to read-only monitoring
            if (!m_reactor.modify(client.getFd(), EPOLLIN | EPOLLET))
            {
                closeClientConnection(client.getFd());
                return false;
            }
        }
        return true;
    }
    void closeClientConnection(int fd)
    {
        m_reactor.remove(fd);
        m_clients.erase(fd);
    }

    [[nodiscard]] std::string_view getIp() const { return m_config.ip; }

    [[nodiscard]] int getPort() const { return m_config.port; }

    [[nodiscard]] int get() const { return m_socket.get(); }

private:
    void discardExcessConnection()
    {
        // Release the reserved slot
        m_spare_fd = FileDescriptor();

        // Accept into the opened slot
        int discarded_fd = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (discarded_fd >= 0)
        {
            // Close immediately to emit a FIN/RST to the peer and vacate the listen queue
            ::close(discarded_fd);
        }

        // Re-reserve the slot for subsequent bursts
        int new_spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (new_spare >= 0)
        {
            m_spare_fd = FileDescriptor(new_spare);
        }
        else
        {
            std::cerr << "Failed to reallocate reserve file descriptor" << std::endl;
        }
    }

    static void processFrames(network::Connection &client)
    {
        auto &buf = client.getReadBuffer();
        for (size_t pos; (pos = buf.find('\0')) != std::string::npos;)
        {
            if (pos > 0)
            {
                std::cout << std::format("Message received: {}", buf.substr(0, pos)) << std::endl;
                client.queueWrite(buf.substr(0, pos) + '\0');
            }
            buf.erase(0, pos + 1);
        }
    }

    void clientCallback(int client_fd, uint32_t events)
    {
        using namespace network;

        auto client_it = m_clients.find(client_fd);
        if (client_it == m_clients.end())
        {
            return;
        }
        auto &client = client_it->second;

        // Fatal hardware/epoll error: tear down immediately
        if (events & EPOLLERR)
        {
            closeClientConnection(client_fd);
            return;
        }

        // Drain pending write buffer on EPOLLOUT
        if ((events & EPOLLOUT) && client.isWaitingForWrite())
        {
            if (!flush(client))
            {
                return; // flush handles closeClientConnection on error/completion
            }

            // Re-check existence in case flush() drained and closed a half-closed client
            if (m_clients.find(client_fd) == m_clients.end())
            {
                return;
            }
        }

        // Read incoming data (only if read side isn't already closed)
        if (!client.isReadClosed() && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)))
        {
            auto &buf = client.getReadBuffer();
            SocketIoResult result = SocketIo::readSome(client_fd, buf);

            if (result == SocketIoResult::Error)
            {
                closeClientConnection(client_fd);
                return;
            }

            // Parse any complete messages received before/at EOF
            processFrames(client);

            if (result == SocketIoResult::Eof)
            {
                client.setReadClosed(true);
            }

            // Flush queued echoes out to socket
            flush(client);
        }
    }

private:
    ServerConfig m_config;
    FileDescriptor m_socket;
    FileDescriptor m_spare_fd; // spare file descriptor to handle EMFILE/ENFILE errors
    std::unordered_map<int, network::Connection> m_clients;
    network::EpollReactor &m_reactor;
};


#endif // SERVER_SERVER_H
