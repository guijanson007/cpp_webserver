// WEB-9: strict integer parsing for command-line arguments.
// API under test (written by the user in common/include/common/ParseNumber.h):
//   template <std::integral T>
//   Result<T, std::string> parseInteger(std::string_view text, T min, T max);
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string_view>

#include "common/ParseNumber.h"

namespace
{
    constexpr int kMin = 1;
    constexpr int kMax = 65535;

    Result<int, std::string> parsePort(std::string_view s) { return parseInteger<int>(s, kMin, kMax); }
} // namespace

TEST(ParseNumberTest, Valid)
{
    auto r = parsePort("8080");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 8080);
}

TEST(ParseNumberTest, Boundaries)
{
    auto lo = parsePort("1");
    auto hi = parsePort("65535");
    ASSERT_TRUE(lo.ok());
    ASSERT_TRUE(hi.ok());
    EXPECT_EQ(lo.value(), 1);
    EXPECT_EQ(hi.value(), 65535);
}

TEST(ParseNumberTest, BelowMin)
{
    EXPECT_FALSE(parsePort("0").ok());
}

TEST(ParseNumberTest, AboveMax)
{
    EXPECT_FALSE(parsePort("65536").ok());
    EXPECT_FALSE(parsePort("70000").ok());
}

TEST(ParseNumberTest, TrailingGarbage)
{
    EXPECT_FALSE(parsePort("8080abc").ok());
    EXPECT_FALSE(parsePort("8080 ").ok());
    EXPECT_FALSE(parsePort("80.5").ok());
    EXPECT_FALSE(parsePort("0x50").ok());
}

TEST(ParseNumberTest, LeadingGarbage)
{
    EXPECT_FALSE(parsePort(" 8080").ok());
    EXPECT_FALSE(parsePort("+8080").ok()); // from_chars rejects '+'; the CLI has no use for it
    EXPECT_FALSE(parsePort("abc").ok());
}

TEST(ParseNumberTest, Empty)
{
    EXPECT_FALSE(parsePort("").ok());
}

TEST(ParseNumberTest, Negative)
{
    EXPECT_FALSE(parsePort("-1").ok());
    EXPECT_FALSE(parsePort("-").ok());
    // A negative number is fine when the range allows it.
    auto r = parseInteger<int>("-5", -10, 10);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), -5);
}

TEST(ParseNumberTest, Overflow)
{
    EXPECT_FALSE(parsePort("99999999999999999999").ok());
    EXPECT_FALSE(parseInteger<int>("2147483648", 0, std::numeric_limits<int>::max()).ok());
    EXPECT_FALSE(parseInteger<std::uint16_t>("65536", 0, 65535).ok());
    auto r = parseInteger<int>("2147483647", 0, std::numeric_limits<int>::max());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), std::numeric_limits<int>::max());
}
