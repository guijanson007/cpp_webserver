// WEB-11: --root validation.
// API under test (written by the user in server/include/server/ServerConfigParser.h):
//   static Result<std::filesystem::path> ServerConfigParser::parseRootDirectory(std::string_view);
// Only a friendly startup error. The security boundary is the root fd opened in Server::start().
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>

#include "server/ServerConfig.h"
#include "server/ServerConfigParser.h"

namespace fs = std::filesystem;

namespace
{
    class ParseRootDirectoryTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string tmpl = (fs::temp_directory_path() / "web11-root-XXXXXX").string();
            ASSERT_NE(::mkdtemp(tmpl.data()), nullptr) << "mkdtemp failed, errno " << errno;
            m_dir = tmpl;
        }

        void TearDown() override
        {
            std::error_code ec;
            // Restore permissions so remove_all can enter directories the test locked.
            fs::permissions(m_dir, fs::perms::owner_all, fs::perm_options::add, ec);
            fs::remove_all(m_dir, ec);
        }

        fs::path m_dir;
    };
} // namespace

TEST_F(ParseRootDirectoryTest, ExistingDirectoryIsAccepted)
{
    auto r = ServerConfigParser::parseRootDirectory(m_dir.string());
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), m_dir);
}

TEST_F(ParseRootDirectoryTest, RelativeDirectoryIsAccepted)
{
    auto r = ServerConfigParser::parseRootDirectory(".");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), fs::path("."));
}

TEST_F(ParseRootDirectoryTest, TrailingSlashIsAccepted)
{
    auto r = ServerConfigParser::parseRootDirectory(m_dir.string() + "/");
    ASSERT_TRUE(r.ok()) << r.error().message();
}

TEST_F(ParseRootDirectoryTest, SymlinkToDirectoryIsAccepted)
{
    // status() follows symlinks; a symlinked docroot is a normal deployment layout.
    fs::create_directory(m_dir / "real");
    fs::create_directory_symlink(m_dir / "real", m_dir / "link");
    auto r = ServerConfigParser::parseRootDirectory((m_dir / "link").string());
    ASSERT_TRUE(r.ok()) << r.error().message();
}

TEST_F(ParseRootDirectoryTest, EmptyStringIsInvalidArgument)
{
    auto r = ServerConfigParser::parseRootDirectory("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EINVAL);
}

TEST_F(ParseRootDirectoryTest, MissingPathIsNoSuchFile)
{
    auto r = ServerConfigParser::parseRootDirectory((m_dir / "does-not-exist").string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(ParseRootDirectoryTest, MissingParentIsNoSuchFile)
{
    auto r = ServerConfigParser::parseRootDirectory((m_dir / "a" / "b" / "c").string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(ParseRootDirectoryTest, RegularFileIsNotADirectory)
{
    const fs::path file = m_dir / "index.html";
    std::ofstream(file) << "<h1>hi</h1>";
    auto r = ServerConfigParser::parseRootDirectory(file.string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOTDIR);
}

TEST_F(ParseRootDirectoryTest, DanglingSymlinkIsNoSuchFile)
{
    fs::create_symlink(m_dir / "gone", m_dir / "dangling");
    auto r = ServerConfigParser::parseRootDirectory((m_dir / "dangling").string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(ParseRootDirectoryTest, UnsearchableParentIsPermissionDenied)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root bypasses directory permissions";
    }
    const fs::path locked = m_dir / "locked";
    fs::create_directories(locked / "www");
    fs::permissions(locked, fs::perms::none);

    auto r = ServerConfigParser::parseRootDirectory((locked / "www").string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EACCES);
    EXPECT_FALSE(r.error().message().empty());
}

// WEB-13: --idle-timeout validation.
// API under test (written by the user in server/include/server/ServerConfigParser.h):
//   static Result<std::chrono::seconds, std::string> ServerConfigParser::parseIdleTimeout(std::string_view);
// Range 0-3600 seconds; 0 means "no timeout" (the parser's own comment).

using namespace std::chrono_literals;

TEST(ParseIdleTimeoutTest, TypicalValuesAreAccepted)
{
    for (auto [text, expected]: {std::pair{"1", 1s}, std::pair{"60", 60s}, std::pair{"3600", 3600s}})
    {
        auto r = ServerConfigParser::parseIdleTimeout(text);
        ASSERT_TRUE(r.ok()) << text << ": " << r.error();
        EXPECT_EQ(r.value(), expected) << text;
    }
}

TEST(ParseIdleTimeoutTest, ZeroIsAcceptedAndMeansDisabled)
{
    auto r = ServerConfigParser::parseIdleTimeout("0");
    ASSERT_TRUE(r.ok()) << r.error();
    EXPECT_EQ(r.value(), 0s);
}

TEST(ParseIdleTimeoutTest, AboveOneHourIsRejectedWithRange)
{
    auto r = ServerConfigParser::parseIdleTimeout("3601");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error().find("3600"), std::string::npos) << r.error();
}

TEST(ParseIdleTimeoutTest, MalformedValuesAreRejected)
{
    for (const char *text: {"", "-1", "abc", "5s", " 5", "5 ", "+5", "1.5", "99999999999999999999"})
    {
        auto r = ServerConfigParser::parseIdleTimeout(text);
        EXPECT_FALSE(r.ok()) << "accepted '" << text << "'";
    }
}

TEST(ParseIdleTimeoutTest, DefaultConfigIsSixtySeconds)
{
    EXPECT_EQ(ServerConfig{}.idleTimeout, 60s);
}
