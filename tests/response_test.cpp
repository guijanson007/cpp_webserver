#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "http/Parser.h"
#include "http/Response.h"

// WEB-7 tests for the response serializer and the ParseError -> StatusCode mapping.
// Server wiring (keep-alive, pipelining, HEAD) is covered by scripts/check_http.sh.

namespace
{
    using http::Response;
    using http::StatusCode;

    // Splits a serialized response into head (without the blank line) and body.
    struct Split
    {
        std::string head;
        std::string body;
    };

    Split split(const std::string &raw)
    {
        const size_t sep = raw.find("\r\n\r\n");
        if (sep == std::string::npos)
            return {raw, {}};
        return {raw.substr(0, sep), raw.substr(sep + 4)};
    }

    bool hasLine(const std::string &head, std::string_view line)
    {
        const std::string needle = std::string("\r\n") + std::string(line) + "\r\n";
        return (head + "\r\n").find(needle) != std::string::npos || head.starts_with(std::string(line) + "\r\n");
    }
} // namespace

// ---------- Status line and framing ----------

TEST(ResponseTest, StatusLineForOk)
{
    const std::string raw = Response(StatusCode::Ok, "hi").serialize();
    EXPECT_TRUE(raw.starts_with("HTTP/1.1 200 OK\r\n")) << raw;
}

TEST(ResponseTest, HeadEndsWithBlankLineAndBodyFollows)
{
    const auto [head, body] = split(Response(StatusCode::Ok, "Hello, World!").serialize());
    EXPECT_EQ(body, "Hello, World!");
    EXPECT_FALSE(head.empty());
    EXPECT_EQ(head.find("\r\n\r\n"), std::string::npos);
}

TEST(ResponseTest, ContentLengthMatchesBody)
{
    const auto [head, body] = split(Response(StatusCode::Ok, "Hello, World!").serialize());
    EXPECT_TRUE(hasLine(head, "Content-Length: 13")) << head;
}

TEST(ResponseTest, EmptyBodyStillSendsContentLengthZero)
{
    // Without it a keep-alive client cannot tell where the response ends.
    const std::string raw = Response(StatusCode::BadRequest).serialize();
    const auto [head, body] = split(raw);
    EXPECT_TRUE(hasLine(head, "Content-Length: 0")) << raw;
    EXPECT_TRUE(body.empty());
}

TEST(ResponseTest, ContentLengthCountsBytesNotCharacters)
{
    const std::string utf8 = "h\xC3\xA9llo"; // 6 bytes
    const auto [head, body] = split(Response(StatusCode::Ok, utf8).serialize());
    EXPECT_TRUE(hasLine(head, "Content-Length: 6")) << head;
    EXPECT_EQ(body, utf8);
}

TEST(ResponseTest, BodyWithEmbeddedNulIsPreserved)
{
    const std::string body_in("a\0b", 3);
    const auto [head, body] = split(Response(StatusCode::Ok, body_in).serialize());
    EXPECT_EQ(body, body_in);
    EXPECT_TRUE(hasLine(head, "Content-Length: 3")) << head;
}

TEST(ResponseTest, LargeBodyIsSerializedIntact)
{
    const std::string big(1 << 20, 'x');
    const std::string raw = Response(StatusCode::Ok, big).serialize();
    const auto [head, body] = split(raw);
    EXPECT_EQ(body.size(), big.size());
    EXPECT_TRUE(hasLine(head, "Content-Length: 1048576")) << head;
}

// ---------- Connection header ----------

TEST(ResponseTest, ConnectionCloseOnlyWhenRequested)
{
    const auto keep = split(Response(StatusCode::Ok, "x", "", false).serialize());
    const auto close = split(Response(StatusCode::Ok, "x", "", true).serialize());
    EXPECT_EQ(keep.head.find("Connection:"), std::string::npos) << keep.head;
    EXPECT_TRUE(hasLine(close.head, "Connection: close")) << close.head;
}

TEST(ResponseTest, ErrorResponseWithCloseHasNoBody)
{
    const std::string raw = Response(StatusCode::BadRequest, "", "", true).serialize();
    const auto [head, body] = split(raw);
    EXPECT_TRUE(raw.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_TRUE(hasLine(head, "Connection: close"));
    EXPECT_TRUE(hasLine(head, "Content-Length: 0"));
    EXPECT_TRUE(body.empty());
}

// ---------- Extra headers ----------

TEST(ResponseTest, ExtraHeaderWithoutTrailingCrlfIsTerminated)
{
    const auto [head, body] = split(Response(StatusCode::Ok, "x", "Content-Type: text/plain").serialize());
    EXPECT_TRUE(hasLine(head, "Content-Type: text/plain")) << head;
    EXPECT_EQ(body, "x") << "header must not bleed into the body";
}

TEST(ResponseTest, ExtraHeaderWithTrailingCrlfIsNotDoubled)
{
    const std::string raw = Response(StatusCode::Ok, "x", "Content-Type: text/plain\r\n").serialize();
    const auto [head, body] = split(raw);
    EXPECT_TRUE(hasLine(head, "Content-Type: text/plain")) << raw;
    EXPECT_EQ(body, "x") << "a doubled CRLF would end the head early";
}

TEST(ResponseTest, EveryHeadLineIsCrlfTerminated)
{
    const std::string raw = Response(StatusCode::Ok, "x", "Content-Type: text/plain", true).serialize();
    const auto [head, body] = split(raw);
    for (size_t i = 0; i < head.size(); ++i)
    {
        if (head[i] == '\n')
        {
            ASSERT_TRUE(i > 0 && head[i - 1] == '\r') << "bare LF at offset " << i;
        }
    }
}

// ---------- Reason phrases ----------

struct ReasonCase
{
    StatusCode code;
    std::string_view phrase;
};

class ReasonPhraseTest : public ::testing::TestWithParam<ReasonCase>
{
};

TEST_P(ReasonPhraseTest, MapsToExpectedPhraseAndAppearsInStatusLine)
{
    const auto [code, phrase] = GetParam();
    EXPECT_EQ(http::statusCodeToReasonPhrase(code), phrase);
    const std::string expected = "HTTP/1.1 " + std::to_string(static_cast<int>(code)) + " " + std::string(phrase) + "\r\n";
    EXPECT_TRUE(Response(code).serialize().starts_with(expected));
}

INSTANTIATE_TEST_SUITE_P(Web7, ReasonPhraseTest,
                         ::testing::Values(ReasonCase{StatusCode::Ok, "OK"},
                                           ReasonCase{StatusCode::BadRequest, "Bad Request"},
                                           ReasonCase{StatusCode::NotFound, "Not Found"},
                                           ReasonCase{StatusCode::MethodNotAllowed, "Method Not Allowed"},
                                           ReasonCase{StatusCode::ContentTooLarge, "Content Too Large"},
                                           ReasonCase{StatusCode::UriTooLong, "URI Too Long"},
                                           ReasonCase{StatusCode::HeaderTooLarge, "Request Header Fields Too Large"},
                                           ReasonCase{StatusCode::InternalServerError, "Internal Server Error"},
                                           ReasonCase{StatusCode::NotImplemented, "Not Implemented"},
                                           ReasonCase{StatusCode::BadVersion, "HTTP Version Not Supported"}),
                         [](const ::testing::TestParamInfo<ReasonCase> &info)
                         { return std::to_string(static_cast<int>(info.param.code)); });

TEST(ResponseTest, UnknownStatusCodeHasFallbackPhrase)
{
    EXPECT_FALSE(http::statusCodeToReasonPhrase(static_cast<StatusCode>(799)).empty());
}

// ---------- ParseError <-> StatusCode ----------

TEST(ResponseTest, ParseErrorValuesAreValidStatusCodes)
{
    // Server.h converts with http::StatusCode(ParseError); every error the parser can return
    // must therefore have a real reason phrase, not the "Unknown" fallback.
    for (http::ParseError e: {http::ParseError::BadRequest, http::ParseError::UriTooLong,
                              http::ParseError::HeaderTooLarge, http::ParseError::NotImplemented,
                              http::ParseError::BadVersion})
    {
        const auto code = static_cast<StatusCode>(static_cast<uint16_t>(e));
        EXPECT_NE(http::statusCodeToReasonPhrase(code), "Unknown") << static_cast<int>(e);
    }
}

TEST(ResponseTest, ParserErrorBecomesWellFormedResponse)
{
    const http::ParseOutcome o = http::parse("FOO / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto *err = std::get_if<http::ParseError>(&o);
    ASSERT_NE(err, nullptr);
    const std::string raw = Response(static_cast<StatusCode>(static_cast<uint16_t>(*err)), "", "", true).serialize();
    EXPECT_TRUE(raw.starts_with("HTTP/1.1 501 Not Implemented\r\n")) << raw;
}
