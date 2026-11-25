//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H


#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <common/FileDescriptor.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <functional>
#include <iostream>
#include <list>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include "http/Parser.h"
#include "http/Response.h"
#include "http/Router.h"
#include "network/Connection.h"
#include "network/EpollReactor.h"
#include "network/SocketIo.h"
#include "server/ServerConfig.h"

class Server
{
public:
    Server(ServerConfig config, network::EpollReactor &reactor, http::Router router)
        : m_config(std::move(config)), m_reactor(reactor), m_router(std::move(router))
    {
        m_clients.reserve(m_config.maxConnections);
    }

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    Result<void> start()
    {
        struct sockaddr_in s_addr{};

        int spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (spare < 0)
        {
            return Error::from_errno(errno);
        }
        m_spare_fd = FileDescriptor(spare);

        int temp_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (temp_socket < 0)
        {
            return Error::from_errno(errno);
        }
        m_socket = FileDescriptor(temp_socket);

        std::memset(&s_addr, 0, sizeof(s_addr));
        s_addr.sin_family = AF_INET;
        s_addr.sin_port = ::htons(m_config.port);

        int pton_res = ::inet_pton(AF_INET, m_config.ip.c_str(), &s_addr.sin_addr);
        if (pton_res <= 0)
        {
            return Error(pton_res == 0 ? EINVAL : errno);
        }

        int opt = 1;
        if (0 > ::setsockopt(m_socket.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        {
            return Error::from_errno(errno);
        }

        if (0 > ::bind(m_socket.get(), reinterpret_cast<sockaddr *>(&s_addr), sizeof(s_addr)))
        {
            return Error::from_errno(errno);
        }

        if (0 > ::listen(m_socket.get(), m_config.backlogSize))
        {
            return Error::from_errno(errno);
        }

        // Create and arm the single periodic timerfd
        int temp_timer = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (temp_timer < 0)
        {
            return Error::from_errno(errno);
        }
        m_timer_fd = FileDescriptor(temp_timer);

        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(m_config.sweepInterval);
        struct itimerspec its{};
        its.it_value.tv_sec = ns.count() / 1'000'000'000;
        its.it_value.tv_nsec = ns.count() % 1'000'000'000;
        its.it_interval = its.it_value;

        if (0 > ::timerfd_settime(m_timer_fd.get(), 0, &its, nullptr))
        {
            return Error::from_errno(errno);
        }

        Result<void> result_timer =
                m_reactor.add(m_timer_fd.get(), EPOLLIN | EPOLLET,
                              [this](int fd, uint32_t events_in) { this->timerCallback(fd, events_in); });
        if (!result_timer)
        {
            return result_timer.error();
        }

        Result<void> result_server = m_reactor.add(m_socket.get(), EPOLLIN, [this](int, uint32_t) { this->accept(); });
        if (!result_server)
        {
            return result_server.error();
        }

        return {};
    }

    void accept()
    {
        while (true)
        {
            int fd_temp = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd_temp < 0)
            {
                if (errno == EMFILE || errno == ENFILE)
                {
                    std::cerr << "Process or system FD limit exhausted. Discarding incoming client.\n";
                    discardExcessConnection();
                    return;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    return; // no more connections to accept
                }

                if (errno != EINTR)
                {
                    std::cerr << std::format("Failed to accept connection: {}\n", std::strerror(errno));
                    return;
                }
                continue;
            }

            if (static_cast<int>(m_clients.size()) >= m_config.maxConnections)
            {
                std::cerr << "Max connections reached. Closing the new connection.\n";
                ::close(fd_temp);
                continue; // keep draining the accept queue
            }

            // if the user did not set an idle timeout, the expiresAt will be std::nullopt and the client will never be
            // closed
            std::optional<std::chrono::steady_clock::time_point> timeout;
            if (m_config.idleTimeout.count() != 0)
            {
                timeout = std::chrono::steady_clock::now() + m_config.idleTimeout;
            }
            else
            {
                timeout = std::nullopt;
            }

            m_timers.emplace_back(fd_temp, timeout);

            auto timer_it = std::prev(m_timers.end());

            network::Connection new_client(fd_temp, timer_it);

            constexpr uint32_t events = EPOLLIN | EPOLLET;
            Result<void> client_result = m_reactor.add(fd_temp, events, [this](int fd, uint32_t events_in)
                                                       { this->clientCallback(fd, events_in); });

            if (!client_result)
            {
                m_timers.erase(timer_it);
                return; // new_client destructor closes fd_temp
            }

            m_clients.emplace(fd_temp, std::move(new_client));
        }
    }

    Result<void> flush(network::Connection &client)
    {
        using namespace network;

        std::string &write_buffer = client.getWriteBuffer();
        if (write_buffer.empty())
        {
            if (client.isReadClosed())
            {
                closeClientConnection(client.getFd());
            }
            return {};
        }

        Result<size_t> result = SocketIo::writeSome(client.getFd(), write_buffer);

        if (!result)
        {
            closeClientConnection(client.getFd());
            return result.error();
        }

        // If the buffer still has data, the socket is saturated (hit EAGAIN internally)
        if (!write_buffer.empty())
        {
            if (!client.isWaitingForWrite())
            {
                uint32_t events = EPOLLOUT | EPOLLET;
                if (!client.isReadClosed())
                {
                    events |= EPOLLIN;
                }

                if (!m_reactor.modify(client.getFd(), events))
                {
                    closeClientConnection(client.getFd());
                    return Error(errno != 0 ? errno : EIO);
                }

                client.setWaitingForWrite(true);
            }
            return {};
        }

        // Entire buffer was sent successfully
        if (client.isWaitingForWrite())
        {
            client.setWaitingForWrite(false);

            if (client.isReadClosed())
            {
                closeClientConnection(client.getFd());
                return {};
            }

            if (!m_reactor.modify(client.getFd(), EPOLLIN | EPOLLET))
            {
                closeClientConnection(client.getFd());
                return Error(errno != 0 ? errno : EIO);
            }
        }
        else if (client.isReadClosed())
        {
            closeClientConnection(client.getFd());
        }

        return {};
    }

    void closeClientConnection(int fd)
    {
        auto it = m_clients.find(fd);

        if (it == m_clients.end())
        {
            return;
        }

        static_cast<void>(m_reactor.remove(fd));
        m_timers.erase(it->second.getTimerIterator()); // O(1) removal via stored list iterator
        m_clients.erase(it);
    }

    [[nodiscard]] std::string_view getIp() const { return m_config.ip; }

    [[nodiscard]] int getPort() const { return m_config.port; }

    [[nodiscard]] int get() const { return m_socket.get(); }

private:
    void discardExcessConnection()
    {
        m_spare_fd = FileDescriptor();

        int discarded_fd = ::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (discarded_fd >= 0)
        {
            ::close(discarded_fd);
        }

        int new_spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (new_spare >= 0)
        {
            m_spare_fd = FileDescriptor(new_spare);
        }
        else
        {
            std::cerr << "Failed to reallocate reserve file descriptor\n";
        }
    }

    void timerCallback(int timer_fd, uint32_t events)
    {
        if (m_config.idleTimeout.count() == 0)
        {
            return; // No idle timeout set, no need to check for expired clients
        }

        static_cast<void>(events);

        // Mandatory drain for edge-triggered (EPOLLET) timerfd
        uint64_t expirations{};
        while (true)
        {
            ssize_t n = ::read(timer_fd, &expirations, sizeof(expirations));
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                std::cerr << std::format("timerfd read error: {}\n", std::strerror(errno));
                return;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        // m_timers is ordered oldest-activity-first via splice(); stop at first active client
        while (!m_timers.empty() && m_timers.front().expiresAt <= now)
        {
            closeClientConnection(m_timers.front().clientFd);
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

        if (events & EPOLLERR)
        {
            closeClientConnection(client_fd);
            return;
        }

        // O(1) timeout refresh: update deadline in userspace and relink to tail of LRU list
        auto timer_it = client.getTimerIterator();
        timer_it->expiresAt = std::chrono::steady_clock::now() + m_config.idleTimeout;
        m_timers.splice(m_timers.end(), m_timers, timer_it);

        // Drain pending write buffer on EPOLLOUT
        if ((events & EPOLLOUT) && client.isWaitingForWrite())
        {
            if (auto result = flush(client); !result)
            {
                return;
            }

            if (!m_clients.contains(client_fd))
            {
                return;
            }
        }

        // Read incoming data (only if read side isn't already closed)
        if (!client.isReadClosed() && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)))
        {
            auto &buf = client.getReadBuffer();
            auto result = SocketIo::readAllAvailable(client_fd, buf);

            if (!result)
            {
                closeClientConnection(client_fd);
                return;
            }

            const auto &outcome = result.value();
            while (true)
            {
                http::ParseOutcome parseOutcome = http::parse(buf);

                if (std::holds_alternative<http::NeedMore>(parseOutcome))
                {
                    break;
                }
                else if (std::holds_alternative<http::Done>(parseOutcome))
                {
                    const auto &done = std::get<http::Done>(parseOutcome);
                    const auto &req = done.request;

                    http::Response resp(m_router.route(req));
                    resp.close_connection = !req.shouldKeepAlive() || resp.close_connection;

                    std::string serialized_resp = resp.serialize();
                    if (req.method == http::HttpMethod::HEAD)
                    {
                        serialized_resp.resize(serialized_resp.size() - resp.body.size());
                    }
                    client.queueWrite(std::move(serialized_resp));
                    buf.erase(0, done.consumed_bytes);

                    if (resp.close_connection)
                    {
                        client.setReadClosed(true);
                        break;
                    }
                }
                else if (std::holds_alternative<http::ParseError>(parseOutcome))
                {
                    auto statusCode = http::StatusCode(std::get<http::ParseError>(parseOutcome));
                    client.queueWrite(http::Response(statusCode, "", "", true).serialize());
                    client.setReadClosed(true);
                    break;
                }
            }

            if (outcome.state == ConnectionState::ClosedByPeer)
            {
                client.setReadClosed(true);
            }

            static_cast<void>(flush(client));
        }
    }

private:
    ServerConfig m_config;
    FileDescriptor m_socket;
    FileDescriptor m_spare_fd;
    FileDescriptor m_timer_fd;
    std::list<network::TimerEntry> m_timers;
    std::unordered_map<int, network::Connection> m_clients;
    network::EpollReactor &m_reactor;
    http::Router m_router;
};

#endif // SERVER_SERVER_H
