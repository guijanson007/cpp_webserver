#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <system_error>

#include "common/Error.h"

TEST(Error, FromErrnoKeepsCodeAndMessage)
{
    const Error e = Error::from_errno(ENOENT);
    EXPECT_EQ(e.code(), ENOENT);
    EXPECT_EQ(e.message(), std::strerror(ENOENT));
}

TEST(Error, FromErrorCode)
{
    const Error e{std::make_error_code(std::errc::connection_reset)};
    EXPECT_EQ(e.code(), ECONNRESET);
    EXPECT_FALSE(e.message().empty());
}

TEST(Error, IntConstructorIsExplicit)
{
    static_assert(!std::is_convertible_v<int, Error>, "int must not implicitly convert to Error");
    static_assert(std::is_constructible_v<Error, int>);
}
