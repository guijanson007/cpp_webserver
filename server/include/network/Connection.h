#pragma once

#include <common/FileDescriptor.h>
#include <string>

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


    private:
        FileDescriptor m_clientFd;
        std::string m_readBuffer;
    };
} // namespace network
