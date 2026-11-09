#include <cerrno>
#include <gtest/gtest.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "common/FileDescriptor.h"
#include "network/SocketIo.h"

using network::ConnectionState;
using network::SocketIo;

namespace
{
    // A connected, non-blocking AF_UNIX stream pair: `local` is the end under
    // test, `peer` plays the remote side.
    class SocketIoTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            int fds[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
            local = FileDescriptor(fds[0]);
            peer = FileDescriptor(fds[1]);
        }

        void peerSend(const std::string &data) const
        {
            ASSERT_EQ(::send(peer.get(), data.data(), data.size(), MSG_NOSIGNAL), static_cast<ssize_t>(data.size()));
        }

        // Reads everything currently available on the peer end.
        [[nodiscard]] std::string peerDrain() const
        {
            std::string out;
            char tmp[65536];
            for (;;)
            {
                const ssize_t r = ::recv(peer.get(), tmp, sizeof(tmp), 0);
                if (r <= 0)
                {
                    break;
                }
                out.append(tmp, static_cast<size_t>(r));
            }
            return out;
        }

        FileDescriptor local;
        FileDescriptor peer;
    };
} // namespace

// ---------------------------------------------------------------- readAllAvailable

TEST_F(SocketIoTest, ReadWithNoDataIsOpenAndZeroBytes)
{
    std::string buf;
    auto r = SocketIo::readAllAvailable(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bytes_read, 0u);
    EXPECT_EQ(r.value().state, ConnectionState::Open);
    EXPECT_TRUE(buf.empty());
}

TEST_F(SocketIoTest, ReadDrainsDataAndStaysOpen)
{
    peerSend("hello");
    std::string buf;
    auto r = SocketIo::readAllAvailable(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bytes_read, 5u);
    EXPECT_EQ(r.value().state, ConnectionState::Open);
    EXPECT_EQ(buf, "hello");
}

TEST_F(SocketIoTest, ReadAppendsToExistingBuffer)
{
    peerSend("world");
    std::string buf = "hello ";
    auto r = SocketIo::readAllAvailable(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bytes_read, 5u); // counts only what this call read
    EXPECT_EQ(buf, "hello world");
}

TEST_F(SocketIoTest, ReadLargerThanInternalChunkLoopsUntilDrained)
{
    const std::string payload(100'000, 'x'); // many 4096-byte reads
    // Send in pieces so the socket buffer is never overfilled.
    std::string buf;
    size_t total = 0;
    for (size_t off = 0; off < payload.size(); off += 20'000)
    {
        peerSend(payload.substr(off, 20'000));
        auto r = SocketIo::readAllAvailable(local.get(), buf);
        ASSERT_TRUE(r.ok());
        total += r.value().bytes_read;
    }
    EXPECT_EQ(total, payload.size());
    EXPECT_EQ(buf, payload);
}

TEST_F(SocketIoTest, ReadDataThenEofReportsBothInOneCall)
{
    peerSend("last words");
    peer = FileDescriptor(); // close the peer -> FIN after the data
    std::string buf;
    auto r = SocketIo::readAllAvailable(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bytes_read, 10u);
    EXPECT_EQ(r.value().state, ConnectionState::ClosedByPeer);
    EXPECT_EQ(buf, "last words"); // data before the FIN is not lost
}

TEST_F(SocketIoTest, ReadEofWithoutDataIsClosedByPeer)
{
    peer = FileDescriptor();
    std::string buf;
    auto r = SocketIo::readAllAvailable(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bytes_read, 0u);
    EXPECT_EQ(r.value().state, ConnectionState::ClosedByPeer);
}

TEST_F(SocketIoTest, ReadEofIsNotAnError)
{
    // Regression: EOF used to be encoded as ENOTCONN. A clean FIN must be a
    // success value, so it can never be confused with a real ENOTCONN error.
    peer = FileDescriptor();
    std::string buf;
    EXPECT_TRUE(SocketIo::readAllAvailable(local.get(), buf).ok());
}

TEST_F(SocketIoTest, ReadOnInvalidFdIsEbadf)
{
    std::string buf;
    auto r = SocketIo::readAllAvailable(-1, buf);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EBADF);
}

// ---------------------------------------------------------------- writeSome

TEST_F(SocketIoTest, WriteSendsEverythingAndClearsBuffer)
{
    std::string buf = "response";
    auto r = SocketIo::writeSome(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 8u);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(peerDrain(), "response");
}

TEST_F(SocketIoTest, WriteEmptyBufferIsZeroBytesOk)
{
    std::string buf;
    auto r = SocketIo::writeSome(local.get(), buf);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 0u);
}

TEST_F(SocketIoTest, WriteToFullSocketIsPartialNotAnError)
{
    // Far bigger than any AF_UNIX socket buffer, and the peer is not reading.
    std::string original(16 * 1024 * 1024, '\0');
    for (size_t i = 0; i < original.size(); ++i)
    {
        original[i] = static_cast<char>('a' + (i % 23));
    }
    std::string buf = original;

    auto r = SocketIo::writeSome(local.get(), buf);
    ASSERT_TRUE(r.ok()) << "EAGAIN must not be reported as an error";
    const size_t sent = r.value();
    EXPECT_GT(sent, 0u);
    EXPECT_LT(sent, original.size());

    // The unsent tail stays in the buffer, in order.
    ASSERT_EQ(buf.size(), original.size() - sent);
    EXPECT_EQ(buf, original.substr(sent));

    // Keep draining the peer and retrying: every byte arrives, in order.
    std::string received = peerDrain();
    while (!buf.empty())
    {
        auto again = SocketIo::writeSome(local.get(), buf);
        ASSERT_TRUE(again.ok());
        received += peerDrain();
    }
    received += peerDrain();
    EXPECT_EQ(received.size(), original.size());
    EXPECT_TRUE(received == original);
}

TEST_F(SocketIoTest, WriteToClosedPeerIsEpipeAndDoesNotRaiseSigpipe)
{
    peer = FileDescriptor();
    std::string buf = "into the void";
    auto r = SocketIo::writeSome(local.get(), buf); // would kill the test on SIGPIPE
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EPIPE);
}

TEST_F(SocketIoTest, WriteOnInvalidFdIsEbadf)
{
    std::string buf = "x";
    auto r = SocketIo::writeSome(-1, buf);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EBADF);
    EXPECT_EQ(buf, "x"); // nothing was sent, nothing erased
}
