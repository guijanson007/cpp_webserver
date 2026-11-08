#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include "common/FileDescriptor.h"

namespace network
{
    class EpollReactor
    {
    public:
        EpollReactor()
        {
            m_epollFd = FileDescriptor(::epoll_create1(EPOLL_CLOEXEC));

            if (m_epollFd.get() == -1)
            {
                throw std::runtime_error("Failed to create epoll instance");
            }
        }

        EpollReactor(const EpollReactor &) = delete;
        EpollReactor &operator=(const EpollReactor &) = delete;
        ~EpollReactor() = default;

        bool add(int fd, uint32_t events, std::function<void(int, uint32_t)> handler)
        {
            struct epoll_event event{};
            event.data.fd = fd;
            event.events = events;

            if (-1 == ::epoll_ctl(m_epollFd.get(), EPOLL_CTL_ADD, fd, &event))
            {
                std::cerr << "Failed to add file descriptor to epoll instance: " << strerror(errno) << std::endl;
                return false;
            }
            m_handlers[fd] = std::move(handler);
            return true;
        }

        bool modify(int fd, uint32_t events)
        {
            struct epoll_event event{};
            event.data.fd = fd;
            event.events = events;

            if (-1 == ::epoll_ctl(m_epollFd.get(), EPOLL_CTL_MOD, fd, &event))
            {
                std::cerr << "Failed to modify file descriptor in epoll instance: " << strerror(errno) << std::endl;
                return false;
            }
            return true;
        }

        bool remove(int fd)
        {
            if (-1 == ::epoll_ctl(m_epollFd.get(), EPOLL_CTL_DEL, fd, nullptr))
            {
                std::cerr << "Failed to remove file descriptor from epoll instance: " << strerror(errno) << std::endl;
                m_handlers.erase(fd);
                return false;
            }
            m_handlers.erase(fd);
            return true;
        }

        bool run()
        {
            int nfds{-1};
            do
            {
                nfds = ::epoll_wait(m_epollFd.get(), m_events.data(), MAX_EVENTS, EPOLL_TIMEOUT);
                if (nfds != -1)
                {
                    break;
                }
                if (errno != EINTR)
                {
                    std::cerr << "Failed to wait for events: " << strerror(errno) << std::endl;
                    return false;
                }
            } while (errno == EINTR);

            for (int n{}; n < nfds; ++n)
            {
                int client_fd = m_events[n].data.fd;
                auto it = m_handlers.find(client_fd);
                if (it == m_handlers.end())
                    continue; // No handler found for this fd, skip it

                auto handler = it->second; // copy the handler to avoid issues if the handler modifies m_handlers

                handler(client_fd, m_events[n].events); // Call the handler for this fd
            }
            return true;
        }

    private:
        static constexpr int MAX_EVENTS = 10'000;
        static constexpr int EPOLL_TIMEOUT = -1; // Wait indefinitely
        FileDescriptor m_epollFd;
        std::unordered_map<int, std::function<void(int, uint32_t)>> m_handlers; // handler for each client
        std::array<struct epoll_event, MAX_EVENTS> m_events{};
    };
} // namespace network
