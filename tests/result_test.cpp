#include <cerrno>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <type_traits>
#include <unistd.h>

#include "common/FileDescriptor.h"
#include "common/Result.h"

TEST(Result, OkHoldsValue)
{
    Result<int> r = 42;
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.value(), 42);
}

TEST(Result, ErrorHoldsError)
{
    Result<int> r = Error::from_errno(ENOENT);
    ASSERT_FALSE(r.ok());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.error().code(), ENOENT);
    EXPECT_EQ(r.error().message(), "No such file or directory");
}

TEST(Result, ValueOr)
{
    EXPECT_EQ((Result<int>{7}).value_or(-1), 7);
    EXPECT_EQ((Result<int>{Error::from_errno(EIO)}).value_or(-1), -1);
}

TEST(Result, ValueIsMutableThroughLvalue)
{
    Result<std::string> r = std::string("abc");
    r.value() += "d";
    EXPECT_EQ(r.value(), "abcd");
}

TEST(Result, VoidSuccess)
{
    Result<void> r{};
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(static_cast<bool>(r));
}

TEST(Result, VoidError)
{
    Result<void> r = Error::from_errno(EPIPE);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EPIPE);
}

TEST(Result, MoveOnlyValue)
{
    Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
    ASSERT_TRUE(r.ok());
    std::unique_ptr<int> p = std::move(r).value();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 5);
}

TEST(Result, FileDescriptorIsClosedOnce)
{
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    ::close(fds[1]);
    {
        Result<FileDescriptor> r = FileDescriptor(fds[0]);
        ASSERT_TRUE(r.ok());
        FileDescriptor fd = std::move(r).value();
        EXPECT_EQ(fd.get(), fds[0]);
    } // fd closes here, exactly once
    errno = 0;
    EXPECT_EQ(::close(fds[0]), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(Result, ErrorFromErrnoCapturedAtFailureSite)
{
    errno = 0;
    EXPECT_EQ(::close(-1), -1);
    Result<void> r = Error::from_errno(errno);
    errno = 0; // later clobbering must not affect the stored error
    EXPECT_EQ(r.error().code(), EBADF);
}

TEST(Result, OkAndErrorAreCopyable)
{
    Result<int> a = 1;
    Result<int> b = a;
    EXPECT_EQ(b.value(), 1);
    Result<int> c = Error::from_errno(EIO);
    Result<int> d = c;
    EXPECT_EQ(d.error().code(), EIO);
}

// Concept constraints: nonsensical instantiations must be rejected.
template<typename T, typename E>
concept ResultInstantiable = requires { typename Result<T, E>; };

static_assert(ResultInstantiable<int, Error>);
static_assert(ResultInstantiable<void, Error>);
static_assert(!ResultInstantiable<int, int>, "T == E would make the variant ambiguous");
static_assert(!ResultInstantiable<void, void>);
