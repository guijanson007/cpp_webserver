#pragma once

#include <cerrno>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "common/Result.h"

namespace network
{
    class SocketIo
    {
    public:
        SocketIo() = default;
        ~SocketIo() = default;

        static Result<void> readSome(int fd, std::string &buffer)
        {
            char tmp[4096];

            for (;;)
            {
                const ssize_t r = ::read(fd, tmp, sizeof(tmp));

                if (r > 0)
                {
                    buffer.append(tmp, static_cast<size_t>(r));
                    continue;
                }

                if (r == 0)
                {
                    // Clean EOF from peer
                    // return ENOTCONN to indicate the connection has been closed
                    return Error(ENOTCONN);
                }

                // r < 0: Evaluate errno immediately
                if (errno == EINTR)
                {
                    continue; // Interrupted by signal, retry
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Fully drained the kernel receive queue under EPOLLET
                    return Error(EAGAIN);
                }

                // Fatal socket error
                return {errno};
            }
            return {};
        }

        static Result<void> writeSome(int fd, std::string &buffer)
        {
            size_t total_sent = 0;

            while (total_sent < buffer.size())
            {
                const ssize_t r = ::send(fd, buffer.data() + total_sent, buffer.size() - total_sent, MSG_NOSIGNAL);

                if (r > 0)
                {
                    total_sent += static_cast<size_t>(r);
                    continue;
                }

                if (r < 0)
                {
                    if (errno == EINTR)
                    {
                        continue; // Interrupted by signal, retry
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        // Remove whatever was successfully sent so far
                        if (total_sent > 0)
                        {
                            buffer.erase(0, total_sent);
                        }
                        return Error(EAGAIN);
                    }

                    // Fatal error (EPIPE, ECONNRESET, etc.)
                    if (total_sent > 0)
                    {
                        buffer.erase(0, total_sent);
                    }
                    return {errno};
                }

                // r == 0: Typically shouldn't occur on non-zero send, but treat as closed/error
                if (total_sent > 0)
                {
                    buffer.erase(0, total_sent);
                }
                return Error(EPIPE);
            }

            // All data from the buffer was sent successfully
            buffer.clear();
            return {};
        }
    };
} // namespace network
