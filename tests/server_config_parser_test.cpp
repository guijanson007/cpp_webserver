// WEB-11: --root validation.
// API under test (written by the user in server/include/server/ServerConfigParser.h):
//   static Result<std::filesystem::path> ServerConfigParser::parseRootDirectory(std::string_view);
// Only a friendly startup error. The security boundary is the root fd opened in Server::start().
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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
