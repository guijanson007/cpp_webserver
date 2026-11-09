#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>
#include <optional>

#include "http/Parser.h"

// WEB-6 acceptance tests for the incremental request-head parser.
//
// All calls go through run() so a signature change only needs editing there.
// run() flattens Result<variant<Done, NeedMore>, ParseError> into one variant.

namespace
{
    using namespace std::string_literals;
    using http::Done;
    using http::NeedMore;
    using http::ParseError;
    using http::SizeLimits;

    using Outcome = std::variant<Done, NeedMore, ParseError>;

    constexpr SizeLimits kLimits{.maxRequestLineSize = 256, .maxHeaderSize = 1024, .maxHeaderCount = 8};

    Outcome run(std::string_view buffer, const SizeLimits &limits = kLimits)
    {
        auto result = http::parse(buffer, limits);
        if (!result.ok())
            return result.error();
        auto &v = result.value();
        if (auto *d = std::get_if<Done>(&v))
            return *d;
        return NeedMore{};
    }

    const Done *asDone(const Outcome &o) { return std::get_if<Done>(&o); }
    bool isNeedMore(const Outcome &o) { return std::holds_alternative<NeedMore>(o); }
    const ParseError *asError(const Outcome &o) { return std::get_if<ParseError>(&o); }

    ::testing::AssertionResult isError(const Outcome &o, int status)
    {
        const ParseError *e = asError(o);
        if (!e)
            return ::testing::AssertionFailure()
                   << "expected error " << status << ", got " << (asDone(o) ? "Done" : "NeedMore");
        if (static_cast<int>(*e) != status)
            return ::testing::AssertionFailure() << "expected error " << status << ", got " << static_cast<int>(*e);
        return ::testing::AssertionSuccess();
    }

    constexpr std::string_view kSimple = "GET /index.html HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n";
} // namespace

// ---------- Happy path ----------

TEST(ParserTest, ParsesSimpleGet)
{
    const Outcome o = run(kSimple);
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->consumed_bytes, kSimple.size());
    EXPECT_EQ(d->request.method, http::HttpMethod::GET);
    EXPECT_EQ(d->request.target, "/index.html");
    EXPECT_EQ(d->request.version, "HTTP/1.1");
    // The header view is the raw block: every header line, each ending in CRLF, without the blank line.
    EXPECT_EQ(d->request.header, "Host: example.com\r\nAccept: */*\r\n");
}

TEST(ParserTest, ParsesPostAndHead)
{
    const std::string post = "POST /submit HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
    const std::string head = "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n";
    const Outcome p = run(post);
    const Outcome h = run(head);
    ASSERT_NE(asDone(p), nullptr);
    ASSERT_NE(asDone(h), nullptr);
    EXPECT_EQ(asDone(p)->request.method, http::HttpMethod::POST);
    EXPECT_EQ(asDone(h)->request.method, http::HttpMethod::HEAD);
}

TEST(ParserTest, AcceptsHttp10)
{
    const Outcome o = run("GET / HTTP/1.0\r\nHost: x\r\n\r\n");
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->request.version, "HTTP/1.0");
}

TEST(ParserTest, RequestWithNoHeadersHasEmptyHeaderView)
{
    // header_end == line_end: easy to underflow when slicing the header block.
    constexpr std::string_view in = "GET / HTTP/1.0\r\n\r\n";
    const Outcome o = run(in);
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->consumed_bytes, in.size());
    EXPECT_TRUE(d->request.header.empty()) << "got: [" << d->request.header << "]";
}

TEST(ParserTest, PipelinedBytesAreNotConsumed)
{
    const std::string both = std::string(kSimple) + "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";
    const Outcome o = run(both);
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->consumed_bytes, kSimple.size());
    EXPECT_EQ(d->request.target, "/index.html");
}

TEST(ParserTest, PartialSecondRequestAfterFirstIsLeftAlone)
{
    const std::string both = std::string(kSimple) + "GET /sec";
    const Outcome o = run(both);
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->consumed_bytes, kSimple.size());
}

// ---------- Incremental behaviour ----------

TEST(ParserTest, EveryProperPrefixNeedsMore)
{
    for (size_t n = 0; n < kSimple.size(); ++n)
    {
        const std::string prefix(kSimple.substr(0, n));
        const Outcome o = run(prefix);
        EXPECT_TRUE(isNeedMore(o)) << "prefix of " << n << " bytes should be NeedMore";
    }
}

TEST(ParserTest, SplitAtEveryOffsetGivesSameResultAsOneShot)
{
    const Outcome whole = run(kSimple);
    ASSERT_NE(asDone(whole), nullptr);

    for (size_t split = 1; split < kSimple.size(); ++split)
    {
        std::string buf(kSimple.substr(0, split));
        ASSERT_TRUE(isNeedMore(run(buf))) << "first chunk of " << split << " bytes";

        buf.append(kSimple.substr(split)); // second read completes the request
        const Outcome o = run(buf);
        const Done *d = asDone(o);
        ASSERT_NE(d, nullptr) << "split at " << split;
        EXPECT_EQ(d->consumed_bytes, asDone(whole)->consumed_bytes);
        EXPECT_EQ(d->request.target, asDone(whole)->request.target);
        EXPECT_EQ(d->request.header, asDone(whole)->request.header);
    }
}

TEST(ParserTest, ByteByByteFeed)
{
    std::string buf;
    Outcome last{NeedMore{}};
    for (size_t i = 0; i < kSimple.size(); ++i)
    {
        buf.push_back(kSimple[i]);
        last = run(buf);
        if (i + 1 < kSimple.size())
        {
            ASSERT_TRUE(isNeedMore(last)) << "after " << (i + 1) << " bytes";
        }
    }
    const Done *d = asDone(last);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->consumed_bytes, kSimple.size());
    EXPECT_EQ(d->request.method, http::HttpMethod::GET);
}

TEST(ParserTest, LoneCarriageReturnAtEndOfBufferNeedsMore)
{
    EXPECT_TRUE(isNeedMore(run("GET / HTTP/1.1\r\nHost: x\r\n\r")));
    EXPECT_TRUE(isNeedMore(run("GET / HTTP/1.1\r")));
}

// ---------- Size limits ----------

TEST(ParserTest, RequestLineTooLongWithoutTerminatorIs414)
{
    const std::string in = "GET /" + std::string(kLimits.maxRequestLineSize + 10, 'a');
    EXPECT_TRUE(isError(run(in), 414));
}

TEST(ParserTest, RequestLineTooLongWithTerminatorIs414)
{
    const std::string in = "GET /" + std::string(kLimits.maxRequestLineSize + 10, 'a') + " HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(isError(run(in), 414));
}

TEST(ParserTest, HeaderBlockTooLargeWithoutTerminatorIs431)
{
    // Never sends the blank line: the parser must give up instead of buffering forever.
    std::string in = "GET / HTTP/1.1\r\nHost: x\r\n";
    while (in.size() <= kLimits.maxHeaderSize + 64)
        in += "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
    EXPECT_TRUE(isError(run(in, {.maxRequestLineSize = 256, .maxHeaderSize = 1024, .maxHeaderCount = 1000}), 431));
}

TEST(ParserTest, HeaderBlockTooLargeWithTerminatorIs431)
{
    std::string in = "GET / HTTP/1.1\r\nHost: x\r\n";
    while (in.size() <= kLimits.maxHeaderSize + 64)
        in += "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
    in += "\r\n";
    EXPECT_TRUE(isError(run(in, {.maxRequestLineSize = 256, .maxHeaderSize = 1024, .maxHeaderCount = 1000}), 431));
}

TEST(ParserTest, SameHeadGivesSameVerdictWhetherWholeOrPartial)
{
    // The size limit must not depend on whether the terminator has arrived yet.
    // Head that fits: request line + headers just under the limit.
    const SizeLimits limits{.maxRequestLineSize = 256, .maxHeaderSize = 200, .maxHeaderCount = 100};
    std::string head = "GET / HTTP/1.1\r\nHost: x\r\n";
    while (head.size() + 20 < limits.maxHeaderSize)
        head += "X: aaaaaaaaaaaaaa\r\n"; // 18 bytes each
    head += "\r\n";
    ASSERT_LE(head.size(), limits.maxHeaderSize);

    EXPECT_NE(asDone(run(head, limits)), nullptr) << "a head within the limit must parse";
    const std::string partial = head.substr(0, head.size() - 2); // terminator not yet received
    EXPECT_TRUE(isNeedMore(run(partial, limits))) << "and must not be rejected while incomplete";
}

TEST(ParserTest, TooManyHeadersIs431)
{
    std::string in = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (size_t i = 0; i <= kLimits.maxHeaderCount; ++i)
        in += "H" + std::to_string(i) + ": v\r\n";
    in += "\r\n";
    EXPECT_TRUE(isError(run(in), 431));
}

TEST(ParserTest, ExactlyMaxHeaderCountIsAccepted)
{
    std::string in = "GET / HTTP/1.1\r\nHost: x\r\n"; // Host counts as one header
    for (size_t i = 0; i + 1 < kLimits.maxHeaderCount; ++i)
        in += "H" + std::to_string(i) + ": v\r\n";
    in += "\r\n";
    EXPECT_NE(asDone(run(in)), nullptr);
}

// ---------- Malformed input: must be an error, never a crash ----------

struct BadCase
{
    const char *name;
    std::string input;
    int expectedStatus;
};

class ParserMalformedTest : public ::testing::TestWithParam<BadCase>
{
};

TEST_P(ParserMalformedTest, IsRejectedWithExpectedStatus)
{
    const BadCase &c = GetParam();
    EXPECT_TRUE(isError(run(c.input), c.expectedStatus));
}

INSTANTIATE_TEST_SUITE_P(
        Web6, ParserMalformedTest,
        ::testing::Values(
                BadCase{"NoVersion", "GET /\r\nHost: x\r\n\r\n", 400},
                BadCase{"NoTarget", "GET\r\nHost: x\r\n\r\n", 400},
                BadCase{"DoubleSpaceBeforeTarget", "GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
                BadCase{"TrailingJunkAfterVersion", "GET / HTTP/1.1 junk\r\nHost: x\r\n\r\n", 400},
                BadCase{"GarbageVersion", "GET / FOO\r\nHost: x\r\n\r\n", 400},
                BadCase{"UnsupportedVersion", "GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
                BadCase{"LowercaseMethod", "get / HTTP/1.1\r\nHost: x\r\n\r\n", 501},
                BadCase{"UnknownMethod", "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", 501},
                BadCase{"BareLfLineEndings", "GET / HTTP/1.1\nHost: x\n\n", 400},
                BadCase{"BareLfInsideHeaders", "GET / HTTP/1.1\r\nHost: x\nA: b\r\n\r\n", 400},
                BadCase{"BareCrInsideHeaderValue", "GET / HTTP/1.1\r\nHost: x\rA: b\r\n\r\n", 400},
                BadCase{"SpaceBeforeColon", "GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
                BadCase{"TabBeforeColon", "GET / HTTP/1.1\r\nHost\t: x\r\n\r\n", 400},
                BadCase{"ObsFoldContinuationWithColon", "GET / HTTP/1.1\r\nA: b\r\n c: d\r\n\r\n", 400},
                BadCase{"ObsFoldContinuationNoColon", "GET / HTTP/1.1\r\nA: b\r\n c\r\n\r\n", 400},
                BadCase{"LeadingWhitespaceFirstHeader", "GET / HTTP/1.1\r\n Host: x\r\n\r\n", 400},
                BadCase{"EmptyHeaderName", "GET / HTTP/1.1\r\n: value\r\n\r\n", 400},
                BadCase{"HeaderWithoutColon", "GET / HTTP/1.1\r\nHost x\r\n\r\n", 400},
                BadCase{"ControlCharInHeaderName", "GET / HTTP/1.1\r\nHo\x01st: x\r\n\r\n", 400},
                BadCase{"DuplicateContentLength",
                        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400},
                BadCase{"DuplicateContentLengthDifferentCase",
                        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\n", 400}),
        [](const ::testing::TestParamInfo<BadCase> &info) { return std::string(info.param.name); });

TEST(ParserTest, NulByteInRequestLineIsRejected)
{
    // Built with an explicit length: a plain string literal would be cut at the NUL.
    const std::string in = "GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n"s;
    EXPECT_TRUE(isError(run(in), 400));
}

TEST(ParserTest, NulByteInHeaderValueIsRejected)
{
    const std::string in = "GET / HTTP/1.1\r\nHost: x\0y\r\n\r\n"s;
    EXPECT_TRUE(isError(run(in), 400));
}

TEST(ParserTest, EmptyBufferNeedsMore)
{
    EXPECT_TRUE(isNeedMore(run("")));
}

TEST(ParserTest, HeaderNamesAreCaseInsensitiveForDuplicateDetection)
{
    // Different headers are fine; only duplicate Content-Length is fatal.
    const Outcome o = run("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nX-Other: 1\r\nX-Other: 2\r\n\r\n");
    EXPECT_NE(asDone(o), nullptr);
}

TEST(ParserTest, Http11WithoutHostIs400)
{
    EXPECT_TRUE(isError(run("GET / HTTP/1.1\r\nAccept: */*\r\n\r\n"), 400));
    EXPECT_TRUE(isError(run("GET / HTTP/1.1\r\n\r\n"), 400));
}

TEST(ParserTest, Http10WithoutHostIsAccepted)
{
    EXPECT_NE(asDone(run("GET / HTTP/1.0\r\nAccept: */*\r\n\r\n")), nullptr);
}

TEST(ParserTest, HostHeaderNameIsCaseInsensitive)
{
    EXPECT_NE(asDone(run("GET / HTTP/1.1\r\nhOsT: x\r\n\r\n")), nullptr);
}

// ---------- Request::getHeaderValue ----------

TEST(RequestTest, HeaderLookupIsCaseInsensitiveAndTrimsValue)
{
    const Outcome o = run("POST / HTTP/1.1\r\nHost: example.com\r\ncontent-LENGTH:   42 \t\r\n\r\n");
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->request.getHeaderValue("Content-Length"), std::optional<std::string_view>("42"));
    EXPECT_EQ(d->request.getHeaderValue("HOST"), std::optional<std::string_view>("example.com"));
    EXPECT_EQ(d->request.getHeaderValue("X-Missing"), std::nullopt);
}

TEST(RequestTest, HeaderLookupWithEmptyHeaderBlock)
{
    const Outcome o = run("GET / HTTP/1.0\r\n\r\n");
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->request.getHeaderValue("Host"), std::nullopt);
}

TEST(RequestTest, HeaderLookupFindsLastHeader)
{
    // Lookup must see the last header line of the block.
    const Outcome o = run("GET / HTTP/1.1\r\nHost: a\r\nX-Last: z\r\n\r\n");
    const Done *d = asDone(o);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->request.getHeaderValue("X-Last"), std::optional<std::string_view>("z"));
}
