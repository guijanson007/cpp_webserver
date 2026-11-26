#pragma once

#include <chrono>
#include <common/FileDescriptor.h>
#include <list>
#include <optional>
#include <string>
#include <string_view>

namespace network
{
    struct TimerEntry
    {
        TimerEntry(int fd, std::optional<std::chrono::steady_clock::time_point> expires)
            : clientFd(fd), expiresAt(expires)
        {
        }
        int clientFd;
        std::optional<std::chrono::steady_clock::time_point> expiresAt;
    };


    class Connection
    {
    public:
        explicit Connection(int fd, std::list<TimerEntry>::iterator timer_it)
            : m_timerIterator(timer_it), m_clientFd{fd}
        {
        }
        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;
        Connection(Connection &&) = default;
        Connection &operator=(Connection &&) = default;
        ~Connection() = default;

        [[nodiscard]] int getFd() const { return m_clientFd.get(); }

        [[nodiscard]] std::string &getReadBuffer() { return m_readBuffer; }

        void queueWrite(std::string_view data) { m_writeBuffer.append(data); }

        [[nodiscard]] std::string &getWriteBuffer() { return m_writeBuffer; }

        void clearWriteBuffer() { m_writeBuffer.clear(); }

        void setReadClosed(bool closed) { m_readClosed = closed; }

        [[nodiscard]] bool isReadClosed() const { return m_readClosed; }

        void setWaitingForWrite(bool waiting) { m_waitingForWrite = waiting; }

        [[nodiscard]] bool isWaitingForWrite() const { return m_waitingForWrite; }

        [[nodiscard]] std::list<network::TimerEntry>::iterator getTimerIterator() { return m_timerIterator; }

        void close()
        {
            if (m_clientFd.valid())
            {
                m_clientFd = FileDescriptor(-1);
            }
        }

    private:
        std::list<network::TimerEntry>::iterator m_timerIterator; // iterator to the corresponding timer in the list
        FileDescriptor m_clientFd;
        std::string m_readBuffer;
        std::string m_writeBuffer;
        bool m_waitingForWrite{false};
        bool m_readClosed{false};
    };
} // namespace network
