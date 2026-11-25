// WEB-13: idle connections are closed after ServerConfig::idleTimeout.
//
// In-process test of the public Server API only:
//   Server(ServerConfig, network::EpollReactor &, http::Router);  Result<void> start();
//   ServerConfig::idleTimeout (seconds), ServerConfig::sweepInterval (periodic sweep timer).
// The test drives the reactor itself with reactor.run(). It registers its own 20 ms tick timerfd
// so run() never blocks for long, without relying on the server's sweep timer being armed
// (with idleTimeout 0 the server may legitimately never wake up).
// Clients are non-blocking sockets on 127.0.0.1. Each test takes 1-3 s (idleTimeout is whole seconds).
#include <gtest/gtest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

#include "common/FileDescriptor.h"
#include "http/Response.h"
#include "http/Router.h"
#include "network/EpollReactor.h"
#include "server/Server.h"
#include "server/ServerConfig.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{
    constexpr std::string_view GET_ROOT = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    constexpr std::string_view BODY = "Hello, World!";

    // Asks the kernel for a free port. Small race with other processes, fine for a test.
    int freePort()
    {
        FileDescriptor s(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            return -1;
        socklen_t len = sizeof(addr);
        ::getsockname(s.get(), reinterpret_cast<sockaddr *>(&addr), &len);
        return ntohs(addr.sin_port);
    }

    enum class PeerState
    {
        Open,
        Closed
    };

    class ServerIdleTimeoutTest : public ::testing::Test
    {
    protected:
        void startServer(std::chrono::seconds idleTimeout)
        {
            m_port = freePort();
            ASSERT_GT(m_port, 0);

            http::Router router;
            router.add(http::HttpMethod::GET, "/", [](const http::Request &)
                       { return http::Response(http::StatusCode::Ok, std::string(BODY), "Content-Type: text/plain", false); });

            ServerConfig config{
                    .ip = "127.0.0.1",
                    .port = m_port,
                    .maxConnections = 1000,
                    .backlogSize = 1000,
                    .rootDirectory = std::nullopt,
                    .idleTimeout = idleTimeout,
                    .sweepInterval = 50ms,
            };
            m_reactor = std::make_unique<network::EpollReactor>();
            armTestTick();
            m_server = std::make_unique<Server>(std::move(config), *m_reactor, std::move(router));
            auto started = m_server->start();
            ASSERT_TRUE(started.ok()) << started.error().message();
        }

        // A connected non-blocking client. The accept happens on the next pump().
        FileDescriptor connectClient()
        {
            FileDescriptor c = connectOnly();
            pumpFor(20ms);
            return c;
        }

        // Starts the connect without running the reactor (for opening many clients quickly).
        [[nodiscard]] FileDescriptor connectOnly() const
        {
            FileDescriptor c(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(m_port));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int r = ::connect(c.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            EXPECT_TRUE(r == 0 || errno == EINPROGRESS) << std::strerror(errno);
            return c;
        }

        // Runs the reactor until `done()` is true or `limit` passes. Returns whether done() became true.
        bool pumpUntil(const std::function<bool()> &done, std::chrono::milliseconds limit)
        {
            const auto deadline = Clock::now() + limit;
            while (Clock::now() < deadline)
            {
                if (done())
                    return true;
                auto r = m_reactor->run(); // wakes at least every 20 ms (test tick)
                EXPECT_TRUE(r.ok());
            }
            return done();
        }

        void pumpFor(std::chrono::milliseconds d)
        {
            pumpUntil([] { return false; }, d);
        }

        // Reads whatever is available. Closed on EOF or reset.
        static PeerState poll(const FileDescriptor &c, std::string *into = nullptr)
        {
            char buf[4096];
            for (;;)
            {
                ssize_t n = ::recv(c.get(), buf, sizeof(buf), 0);
                if (n > 0)
                {
                    if (into)
                        into->append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0)
                    return PeerState::Closed;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return PeerState::Open;
                if (errno == EINTR)
                    continue;
                return PeerState::Closed; // ECONNRESET and friends
            }
        }

        // Sends GET / and pumps until the full response arrived. False if the server closed instead.
        bool request(const FileDescriptor &c)
        {
            if (::send(c.get(), GET_ROOT.data(), GET_ROOT.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(GET_ROOT.size()))
                return false;
            std::string got;
            bool closed = false;
            pumpUntil(
                    [&]
                    {
                        closed = poll(c, &got) == PeerState::Closed;
                        return closed || got.ends_with(BODY);
                    },
                    2s);
            return !closed && got.starts_with("HTTP/1.1 200") && got.ends_with(BODY);
        }

        // Seconds until the server closes `c`, or a negative value if it is still open after `limit`.
        double secondsUntilClosed(const FileDescriptor &c, std::chrono::milliseconds limit)
        {
            const auto t0 = Clock::now();
            if (!pumpUntil([&] { return poll(c) == PeerState::Closed; }, limit))
                return -1.0;
            return std::chrono::duration<double>(Clock::now() - t0).count();
        }

        int m_port{-1};
        std::unique_ptr<network::EpollReactor> m_reactor;
        std::unique_ptr<Server> m_server;
        FileDescriptor m_tick;

    private:
        // Test-owned periodic timer in the same reactor: keeps pumpUntil() from blocking.
        void armTestTick()
        {
            m_tick = FileDescriptor(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
            ASSERT_TRUE(m_tick.valid());
            itimerspec its{.it_interval = {.tv_sec = 0, .tv_nsec = 20'000'000},
                           .it_value = {.tv_sec = 0, .tv_nsec = 20'000'000}};
            ASSERT_EQ(::timerfd_settime(m_tick.get(), 0, &its, nullptr), 0);
            auto added = m_reactor->add(m_tick.get(), EPOLLIN,
                                        [](int fd, uint32_t)
                                        {
                                            std::uint64_t n{};
                                            static_cast<void>(::read(fd, &n, sizeof(n)));
                                        });
            ASSERT_TRUE(added.ok());
        }
    };
} // namespace

TEST_F(ServerIdleTimeoutTest, SilentConnectionIsClosedAfterTimeout)
{
    startServer(1s);
    auto c = connectClient();
    const double s = secondsUntilClosed(c, 3s);
    ASSERT_GE(s, 0.0) << "silent connection still open after 3 s with a 1 s timeout";
    EXPECT_GE(s, 0.8) << "closed before the timeout";
    EXPECT_LE(s, 1.5) << "closed too late for a 1 s timeout and a 50 ms sweep";
}

TEST_F(ServerIdleTimeoutTest, ActivityPushesTheDeadlineBack)
{
    startServer(1s);
    auto c = connectClient();
    // 4 requests 0.6 s apart: 2.4 s in total, more than twice the timeout.
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(request(c)) << "active connection was closed at request " << i;
        pumpFor(600ms);
    }
    // Then silence: closed about 1 s after the last activity (0.6 s of which already passed).
    const double s = secondsUntilClosed(c, 3s);
    ASSERT_GE(s, 0.0) << "connection never closed once it went idle";
    EXPECT_LE(s, 1.0);
}

// A connection closed by the peer must take its timer entry with it. Otherwise the stale entry,
// keyed by an fd number the next client reuses, expires and closes that innocent client.
TEST_F(ServerIdleTimeoutTest, PeerCloseLeavesNoStaleTimerForReusedFd)
{
    startServer(1s);
    {
        auto a = connectClient();
        ASSERT_TRUE(request(a));
    } // A closes here
    pumpFor(100ms);

    auto b = connectClient(); // usually gets A's old fd number on the server
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(request(b)) << "B was closed while active, request " << i;
        pumpFor(600ms);
    }
}

TEST_F(ServerIdleTimeoutTest, ConnectionCloseResponseLeavesNoStaleTimer)
{
    startServer(1s);
    {
        auto a = connectClient();
        constexpr std::string_view req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        ASSERT_EQ(::send(a.get(), req.data(), req.size(), MSG_NOSIGNAL), static_cast<ssize_t>(req.size()));
        ASSERT_TRUE(pumpUntil([&] { return poll(a) == PeerState::Closed; }, 2s)) << "Connection: close not honoured";
    }
    auto b = connectClient();
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(request(b)) << "B was closed while active, request " << i;
        pumpFor(600ms);
    }
}

TEST_F(ServerIdleTimeoutTest, ManySilentConnectionsAreAllClosedAndServerStillServes)
{
    startServer(1s);
    constexpr int count = 100;
    std::vector<FileDescriptor> clients;
    clients.reserve(count);
    for (int i = 0; i < count; ++i)
        clients.push_back(connectOnly());

    auto allClosed = [&] { return std::ranges::all_of(clients, [](const auto &c) { return poll(c) == PeerState::Closed; }); };
    EXPECT_TRUE(pumpUntil(allClosed, 3s)) << "some silent connections were never closed";

    auto fresh = connectClient();
    EXPECT_TRUE(request(fresh));
}

// ServerConfigParser documents 0 as "no timeout".
TEST_F(ServerIdleTimeoutTest, ZeroTimeoutDisablesIdleClosing)
{
    startServer(0s);
    auto c = connectClient();
    EXPECT_LT(secondsUntilClosed(c, 1500ms), 0.0) << "--idle-timeout 0 closed an idle connection";
    EXPECT_TRUE(request(c));
}
