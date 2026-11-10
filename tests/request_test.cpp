#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

#include "http/Parser.h"

// WEB-7 tests for Request::shouldKeepAlive().
// Requests come from http::parse so the header block has the exact shape the server sees.

namespace
{
    // Parses `raw` and returns shouldKeepAlive(); fails the test if the request does not parse.
    // The Request views point into `raw`, which outlives the call.
    bool keepAlive(const std::string &raw)
    {
        const http::ParseOutcome o = http::parse(raw);
        const auto *done = std::get_if<http::Done>(&o);
        EXPECT_NE(done, nullptr) << "request did not parse: " << raw;
        return done != nullptr && done->request.shouldKeepAlive();
    }

    std::string req11(std::string_view extraHeaders = "")
    {
        return "GET / HTTP/1.1\r\nHost: x\r\n" + std::string(extraHeaders) + "\r\n";
    }

    std::string req10(std::string_view extraHeaders = "")
    {
        return "GET / HTTP/1.0\r\n" + std::string(extraHeaders) + "\r\n";
    }
} // namespace

// ---------- HTTP/1.1: persistent by default ----------

TEST(KeepAliveTest, Http11DefaultsToKeepAlive) { EXPECT_TRUE(keepAlive(req11())); }

TEST(KeepAliveTest, Http11ConnectionCloseDisablesKeepAlive) { EXPECT_FALSE(keepAlive(req11("Connection: close\r\n"))); }

TEST(KeepAliveTest, Http11ConnectionKeepAliveStaysOpen) { EXPECT_TRUE(keepAlive(req11("Connection: keep-alive\r\n"))); }

TEST(KeepAliveTest, Http11UnknownConnectionValueStaysOpen) { EXPECT_TRUE(keepAlive(req11("Connection: upgrade\r\n"))); }

TEST(KeepAliveTest, Http11EmptyConnectionValueStaysOpen) { EXPECT_TRUE(keepAlive(req11("Connection:\r\n"))); }

// ---------- HTTP/1.0: close by default ----------

TEST(KeepAliveTest, Http10DefaultsToClose) { EXPECT_FALSE(keepAlive(req10())); }

TEST(KeepAliveTest, Http10ConnectionKeepAliveStaysOpen) { EXPECT_TRUE(keepAlive(req10("Connection: keep-alive\r\n"))); }

TEST(KeepAliveTest, Http10ConnectionCloseCloses) { EXPECT_FALSE(keepAlive(req10("Connection: close\r\n"))); }

TEST(KeepAliveTest, Http10UnknownConnectionValueCloses) { EXPECT_FALSE(keepAlive(req10("Connection: upgrade\r\n"))); }

// ---------- Header spelling ----------

TEST(KeepAliveTest, HeaderNameAndValueAreCaseInsensitive)
{
    EXPECT_FALSE(keepAlive(req11("connection: CLOSE\r\n")));
    EXPECT_FALSE(keepAlive(req11("CONNECTION: Close\r\n")));
    EXPECT_TRUE(keepAlive(req10("cOnNeCtIoN: Keep-Alive\r\n")));
}

TEST(KeepAliveTest, SurroundingWhitespaceInValueIsIgnored)
{
    EXPECT_FALSE(keepAlive(req11("Connection:    close \t\r\n")));
    EXPECT_TRUE(keepAlive(req10("Connection:\tkeep-alive  \r\n")));
}

TEST(KeepAliveTest, ConnectionHeaderAmongOthersIsFound)
{
    EXPECT_FALSE(keepAlive(req11("Accept: */*\r\nConnection: close\r\nX-Other: 1\r\n")));
}

TEST(KeepAliveTest, SimilarHeaderNamesAreNotConfusedWithConnection)
{
    EXPECT_TRUE(keepAlive(req11("X-Connection: close\r\n")));
    EXPECT_TRUE(keepAlive(req11("Proxy-Connection: close\r\n")));
}

TEST(KeepAliveTest, ValueMustMatchWholeTokenNotPrefix)
{
    EXPECT_TRUE(keepAlive(req11("Connection: closed\r\n")));
    EXPECT_FALSE(keepAlive(req10("Connection: keep-alive-ish\r\n")));
}

// ---------- Independent of method ----------

TEST(KeepAliveTest, MethodDoesNotAffectDecision)
{
    EXPECT_TRUE(keepAlive("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n"));
    EXPECT_FALSE(keepAlive("HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    EXPECT_TRUE(keepAlive("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"));
}

// ---------- Comma-separated token lists (RFC 9110 7.6.1) ----------

TEST(KeepAliveTest, CloseIsRecognisedInsideTokenList)
{
    EXPECT_FALSE(keepAlive(req11("Connection: TE, close\r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: close, TE\r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: TE, close, upgrade\r\n")));
}

TEST(KeepAliveTest, KeepAliveIsRecognisedInsideTokenListOnHttp10)
{
    EXPECT_TRUE(keepAlive(req10("Connection: keep-alive, TE\r\n")));
    EXPECT_TRUE(keepAlive(req10("Connection: TE, keep-alive\r\n")));
}

TEST(KeepAliveTest, Http10TokenListWithoutKeepAliveCloses)
{
    EXPECT_FALSE(keepAlive(req10("Connection: TE, upgrade\r\n")));
}

TEST(KeepAliveTest, CloseWinsOverKeepAliveOnBothVersions)
{
    EXPECT_FALSE(keepAlive(req11("Connection: keep-alive, close\r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: close, keep-alive\r\n")));
    EXPECT_FALSE(keepAlive(req10("Connection: keep-alive, close\r\n")));
}

TEST(KeepAliveTest, TokenListWhitespaceAndCaseAreIgnored)
{
    EXPECT_FALSE(keepAlive(req11("Connection:TE ,\tCLOSE \r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: te,Close\r\n")));
    EXPECT_TRUE(keepAlive(req10("Connection:  Keep-Alive ,TE\r\n")));
}

TEST(KeepAliveTest, EmptyListElementsAreTolerated)
{
    EXPECT_FALSE(keepAlive(req11("Connection: ,close\r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: close,\r\n")));
    EXPECT_FALSE(keepAlive(req11("Connection: TE,, close\r\n")));
    EXPECT_TRUE(keepAlive(req11("Connection: ,\r\n")));
    EXPECT_FALSE(keepAlive(req10("Connection: ,\r\n")));
}

TEST(KeepAliveTest, TokensMustMatchWholeNotSubstring)
{
    EXPECT_TRUE(keepAlive(req11("Connection: TE, closed\r\n")));
    EXPECT_TRUE(keepAlive(req11("Connection: unclose, TE\r\n")));
    EXPECT_FALSE(keepAlive(req10("Connection: keep-alive-ish, TE\r\n")));
}

TEST(KeepAliveTest, LongListWithCloseAtTheEnd)
{
    EXPECT_FALSE(keepAlive(req11("Connection: a, b, c, d, e, f, g, close\r\n")));
}

// ---------- Known gap ----------
// Request::getHeaderValue returns only the first Connection header, but RFC 9110 5.3 lets a
// sender split a list across several header lines. "Connection: keep-alive" followed by
// "Connection: close" is therefore read as keep-alive. Remove DISABLED_ once shouldKeepAlive
// looks at every Connection header.

TEST(KeepAliveTest, DISABLED_RepeatedConnectionHeadersAreCombined)
{
    EXPECT_FALSE(keepAlive(req11("Connection: TE\r\nConnection: close\r\n")));
    EXPECT_TRUE(keepAlive(req10("Connection: TE\r\nConnection: keep-alive\r\n")));
}
