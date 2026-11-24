// WEB-11: file access confined to a directory fd, behind the wrapper layer.
// API under test (written by the user in common/include/common/FileReader.h):
//   Result<SharedDirFd> openDirectory(const std::filesystem::path &);            // O_PATH | O_DIRECTORY
//   Result<std::string> readRegularFileBeneath(int dirfd, std::string_view rel); // openat2(RESOLVE_BENEATH)
// Escapes fail with EXDEV (kernel), non-regular files with ENOENT (so the handler maps them to 404).
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "common/FileReader.h"

namespace fs = std::filesystem;

namespace
{
    class FileReaderTest : public ::testing::Test
    {
    protected:
        // m_dir/outside.txt is outside the root; m_dir/root is the directory the fd points at.
        void SetUp() override
        {
            std::string tmpl = (fs::temp_directory_path() / "web11-file-XXXXXX").string();
            ASSERT_NE(::mkdtemp(tmpl.data()), nullptr) << "mkdtemp failed, errno " << errno;
            m_dir = tmpl;
            m_root = m_dir / "root";
            fs::create_directories(m_root / "sub");
            write(m_dir / "outside.txt", "OUTSIDE");

            auto dir = openDirectory(m_root);
            ASSERT_TRUE(dir.ok()) << dir.error().message();
            m_rootFd = std::move(dir.value());
            ASSERT_TRUE(m_rootFd && m_rootFd->valid());
        }

        void TearDown() override
        {
            m_rootFd.reset();
            std::error_code ec;
            fs::permissions(m_dir, fs::perms::owner_all, fs::perm_options::add, ec);
            fs::remove_all(m_dir, ec);
        }

        static void write(const fs::path &p, const std::string &content)
        {
            std::ofstream(p, std::ios::binary) << content;
        }

        [[nodiscard]] Result<std::string> read(std::string_view rel) const
        {
            return readRegularFileBeneath(m_rootFd->get(), rel);
        }

        fs::path m_dir;
        fs::path m_root;
        SharedDirFd m_rootFd;
    };
} // namespace

// ---------- openDirectory ----------

TEST_F(FileReaderTest, OpenDirectoryReturnsValidFd)
{
    auto r = openDirectory(m_root);
    ASSERT_TRUE(r.ok()) << r.error().message();
    ASSERT_NE(r.value(), nullptr);
    EXPECT_TRUE(r.value()->valid());
}

TEST_F(FileReaderTest, OpenDirectoryMissingIsENOENT)
{
    auto r = openDirectory(m_dir / "missing");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(FileReaderTest, OpenDirectoryOnFileIsENOTDIR)
{
    auto r = openDirectory(m_dir / "outside.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOTDIR);
}

TEST_F(FileReaderTest, RootFdSurvivesRenameOfTheDirectory)
{
    // The fd pins the directory inode: renaming the path afterwards does not change what is served.
    write(m_root / "a.txt", "pinned");
    fs::rename(m_root, m_dir / "moved");
    auto r = read("a.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), "pinned");
}

// ---------- reading ----------

TEST_F(FileReaderTest, ReadsSmallTextFile)
{
    write(m_root / "a.txt", "hello\n");
    auto r = read("a.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), "hello\n");
}

TEST_F(FileReaderTest, ReadsNestedFile)
{
    write(m_root / "sub" / "b.txt", "nested");
    auto r = read("sub/b.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), "nested");
}

TEST_F(FileReaderTest, EmptyFileIsEmptyString)
{
    write(m_root / "empty.txt", "");
    auto r = read("empty.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_TRUE(r.value().empty());
}

TEST_F(FileReaderTest, BinaryContentIsByteExact)
{
    std::string bytes;
    for (int i = 0; i < 256; ++i)
    {
        bytes.push_back(static_cast<char>(i)); // includes NUL, CR, LF, 0xFF
    }
    bytes += bytes;
    write(m_root / "bin.dat", bytes);
    auto r = read("bin.dat");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), bytes);
}

TEST_F(FileReaderTest, LargeFileIsReadCompletely)
{
    std::string big(3 * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < big.size(); ++i)
    {
        big[i] = static_cast<char>('a' + (i % 26));
    }
    write(m_root / "big.txt", big);
    auto r = read("big.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    ASSERT_EQ(r.value().size(), big.size());
    EXPECT_EQ(r.value(), big);
}

// ---------- argument checks ----------

TEST_F(FileReaderTest, EmptyPathIsEINVAL)
{
    auto r = read("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EINVAL);
}

TEST_F(FileReaderTest, InvalidDirFdIsEINVAL)
{
    auto r = readRegularFileBeneath(-1, "a.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EINVAL);
}

// ---------- non-regular files ----------

TEST_F(FileReaderTest, MissingFileIsENOENT)
{
    auto r = read("nope.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(FileReaderTest, DirectoryIsRejectedAsENOENT)
{
    auto r = read("sub");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
}

TEST_F(FileReaderTest, FifoIsRejectedWithoutBlocking)
{
    ASSERT_EQ(::mkfifo((m_root / "pipe").c_str(), 0600), 0) << "mkfifo failed, errno " << errno;
    const auto start = std::chrono::steady_clock::now();
    auto r = read("pipe");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ENOENT);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST_F(FileReaderTest, UnreadableFileIsEACCES)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root bypasses file permissions";
    }
    write(m_root / "secret.txt", "x");
    fs::permissions(m_root / "secret.txt", fs::perms::none);
    auto r = read("secret.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EACCES);
}

// ---------- confinement (RESOLVE_BENEATH) ----------

TEST_F(FileReaderTest, DotDotEscapeIsEXDEV)
{
    auto r = read("../outside.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EXDEV);
}

TEST_F(FileReaderTest, DotDotThatStaysInsideIsAllowed)
{
    // RESOLVE_BENEATH only rejects escapes; "sub/../a.txt" never leaves the root.
    write(m_root / "a.txt", "inside");
    auto r = read("sub/../a.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), "inside");
}

TEST_F(FileReaderTest, AbsolutePathIsEXDEV)
{
    auto r = read((m_dir / "outside.txt").string());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EXDEV);
}

TEST_F(FileReaderTest, RelativeSymlinkInsideRootIsFollowed)
{
    write(m_root / "real.txt", "via link");
    fs::create_symlink("real.txt", m_root / "link.txt");
    auto r = read("link.txt");
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value(), "via link");
}

TEST_F(FileReaderTest, RelativeSymlinkEscapingRootIsEXDEV)
{
    fs::create_symlink("../outside.txt", m_root / "leak.txt");
    auto r = read("leak.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EXDEV);
}

TEST_F(FileReaderTest, AbsoluteSymlinkIsEXDEV)
{
    // Even an absolute link that points back inside the root is refused under RESOLVE_BENEATH.
    write(m_root / "a.txt", "inside");
    fs::create_symlink(m_root / "a.txt", m_root / "abs-inside.txt");
    fs::create_symlink("/etc/passwd", m_root / "passwd");
    for (const char *name: {"abs-inside.txt", "passwd"})
    {
        auto r = read(name);
        ASSERT_FALSE(r.ok()) << name;
        EXPECT_EQ(r.error().code(), EXDEV) << name;
    }
}

TEST_F(FileReaderTest, SymlinkedDirectoryEscapingRootIsEXDEV)
{
    fs::create_directory_symlink("..", m_root / "up");
    auto r = read("up/outside.txt");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), EXDEV);
}
