#pragma once

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "common/Result.h"

namespace network
{
    enum class ConnectionState
    {
        Open,
        ClosedByPeer
    };

    struct ReadOutcome
    {
        size_t bytes_read{0};
        ConnectionState state{ConnectionState::Open};
    };

    class SocketIo
    {
    public:
        SocketIo() = default;
        ~SocketIo() = default;

        // Drains the kernel buffer until EAGAIN or EOF.
        // Returns the number of bytes read and whether the peer closed connection.
        // Only returns Error on real OS failures (EBADF, ECONNRESET, etc.).
        static Result<ReadOutcome> readAllAvailable(int fd, std::string &buffer)
        {
            char tmp[4096];
            size_t total_read = 0;

            for (;;)
            {
                const ssize_t r = ::read(fd, tmp, sizeof(tmp));

                if (r > 0)
                {
                    buffer.append(tmp, static_cast<size_t>(r));
                    total_read += static_cast<size_t>(r);
                    continue;
                }

                if (r == 0)
                {
                    // Clean EOF: peer sent FIN
                    return ReadOutcome{.bytes_read = total_read, .state = ConnectionState::ClosedByPeer};
                }

                if (errno == EINTR)
                {
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Fully drained - standard completion under EPOLLET
                    return ReadOutcome{.bytes_read = total_read, .state = ConnectionState::Open};
                }

                return Error::from_errno(errno);
            }
        }

        // Attempts to flush as much of the buffer as the socket will accept.
        // Erases sent bytes in place once when leaving the loop.
        static Result<size_t> writeSome(int fd, std::string &buffer)
        {
            size_t total_sent = 0;
            const size_t initial_size = buffer.size();

            while (total_sent < initial_size)
            {
                const ssize_t r = ::send(fd, buffer.data() + total_sent, initial_size - total_sent, MSG_NOSIGNAL);

                if (r > 0)
                {
                    total_sent += static_cast<size_t>(r);
                    continue;
                }

                if (r < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }

                    // On partial send or drain, erase sent slice once and return
                    if (total_sent > 0)
                    {
                        buffer.erase(0, total_sent);
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        return total_sent;
                    }

                    return Error::from_errno(errno);
                }

                // r == 0 on send() with non-zero length implies connection broken
                if (total_sent > 0)
                {
                    buffer.erase(0, total_sent);
                }
                return Error(EPIPE);
            }

            buffer.clear();
            return total_sent;
        }
    };
} // namespace network
