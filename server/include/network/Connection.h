#pragma once

#include <common/FileDescriptor.h>
#include <string>
#include <string_view>

namespace network
{
    class Connection
    {
    public:
        Connection(int fd) : m_clientFd(fd), m_readBuffer{} {}
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

        void setWaitingForWrite(bool waiting) { m_waitingForWrite = waiting; }

        [[nodiscard]] bool isWaitingForWrite() const { return m_waitingForWrite; }

    private:
        FileDescriptor m_clientFd;
        std::string m_readBuffer;
        std::string m_writeBuffer;
        bool m_waitingForWrite{false};
    };
} // namespace network
