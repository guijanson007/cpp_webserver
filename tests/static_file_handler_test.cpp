// WEB-11: static files under a mount prefix, confined to the document root.
// API under test (written by the user in server/include/http/StaticFileHandler.h):
//   http::StaticFileHandler(const std::filesystem::path &root, std::string mountPrefix = "/");
//   http::Response operator()(const http::Request &) const;
//
// Layout built per test (m_dir is a fresh temp dir):
//   m_dir/secret.txt               outside the root, must never be served
//   m_dir/www-evil/secret.txt      sibling whose name shares the root's prefix
//   m_dir/www/                     the document root
//       index.html, style.css, data.bin, hello world.txt, sub/page.txt
//
// Requests are built by hand (not via http::parse) so the handler's own defenses are tested
// even for targets the parser might reject first.
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include "http/StaticFileHandler.h"

namespace fs = std::filesystem;
using namespace http;

namespace
{
    constexpr std::string_view kSecret = "TOP-SECRET-CONTENT";
    constexpr std::string_view kIndex = "<h1>index</h1>";

    Request makeRequest(std::string_view target, HttpMethod method = HttpMethod::GET, std::string_view header = "")
    {
        return Request{.header = header, .body = {}, .target = target, .version = "HTTP/1.1", .method = method};
    }

    class StaticFileHandlerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string tmpl = (fs::temp_directory_path() / "web11-static-XXXXXX").string();
            ASSERT_NE(::mkdtemp(tmpl.data()), nullptr) << "mkdtemp failed, errno " << errno;
            m_dir = tmpl;
            m_root = m_dir / "www";

            fs::create_directories(m_root / "sub");
            fs::create_directories(m_dir / "www-evil");
            write(m_dir / "secret.txt", kSecret);
            write(m_dir / "www-evil" / "secret.txt", kSecret);
            write(m_root / "index.html", kIndex);
            write(m_root / "style.css", "body{}");
            write(m_root / "data.bin", std::string("\x00\x01\xff", 3));
            write(m_root / "hello world.txt", "spaced");
            write(m_root / "sub" / "page.txt", "nested");
        }

        void TearDown() override
        {
            std::error_code ec;
            fs::remove_all(m_dir, ec);
        }

        static void write(const fs::path &p, std::string_view content)
        {
            std::ofstream(p, std::ios::binary) << content;
        }

        [[nodiscard]] StaticFileHandler handler() const { return {m_root, "/static/"}; }

        [[nodiscard]] Response get(std::string_view target) const { return handler()(makeRequest(target)); }

        // Every traversal attempt must fail and must not leak the secret, whatever 4xx it picks.
        void expectBlocked(std::string_view target) const
        {
            const Response resp = get(target);
            EXPECT_NE(resp.status_code, StatusCode::Ok) << "target served: " << target;
            EXPECT_EQ(resp.body.find(kSecret), std::string::npos) << "secret leaked for: " << target;
        }

        fs::path m_dir;
        fs::path m_root;
    };
} // namespace

// ---------- serving ----------

TEST_F(StaticFileHandlerTest, ServesFileWithContentType)
{
    const Response resp = get("/static/index.html");
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, kIndex);
    EXPECT_NE(resp.headers.find("Content-Type: text/html"), std::string::npos) << "headers: " << resp.headers;
}

TEST_F(StaticFileHandlerTest, ContentTypeFollowsExtension)
{
    EXPECT_NE(get("/static/style.css").headers.find("Content-Type: text/css"), std::string::npos);
    EXPECT_NE(get("/static/sub/page.txt").headers.find("Content-Type: text/plain"), std::string::npos);
}

TEST_F(StaticFileHandlerTest, UnknownExtensionIsOctetStream)
{
    const Response resp = get("/static/data.bin");
    ASSERT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, std::string("\x00\x01\xff", 3));
    EXPECT_NE(resp.headers.find("Content-Type: application/octet-stream"), std::string::npos);
}

TEST_F(StaticFileHandlerTest, ServesNestedFile)
{
    const Response resp = get("/static/sub/page.txt");
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "nested");
}

TEST_F(StaticFileHandlerTest, QueryStringIsIgnored)
{
    const Response resp = get("/static/index.html?v=3&x=../../secret.txt");
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, kIndex);
}

TEST_F(StaticFileHandlerTest, PercentEncodedNameIsDecoded)
{
    const Response resp = get("/static/hello%20world.txt");
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "spaced");
}

TEST_F(StaticFileHandlerTest, RedundantSlashesInsideRoot)
{
    EXPECT_EQ(get("/static/sub//page.txt").status_code, StatusCode::Ok);
}

TEST_F(StaticFileHandlerTest, DotSegmentsAreRejected)
{
    // Policy: any segment starting with '.' is refused, including "." itself.
    EXPECT_EQ(get("/static/./index.html").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/static/sub/./page.txt").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/static/sub/../index.html").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, HiddenFilesAndDirectoriesAreRejected)
{
    fs::create_directories(m_root / ".git");
    write(m_root / ".git" / "config", kSecret);
    write(m_root / ".htaccess", kSecret);
    write(m_root / "sub" / ".env", kSecret);
    expectBlocked("/static/.git/config");
    expectBlocked("/static/.htaccess");
    expectBlocked("/static/sub/.env");
    expectBlocked("/static/%2egit/config"); // decoded before the check
    expectBlocked("/static/%2Ehtaccess");
}

TEST_F(StaticFileHandlerTest, DotInsideNameIsAllowed)
{
    write(m_root / "v1.2.txt", "versioned");
    const Response resp = get("/static/v1.2.txt");
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, "versioned");
}

TEST_F(StaticFileHandlerTest, HeadReturnsSameStatusAndHeadersAsGet)
{
    // The handler builds the full response; Server strips the body for HEAD.
    const Response g = get("/static/index.html");
    const Response h = handler()(makeRequest("/static/index.html", HttpMethod::HEAD));
    EXPECT_EQ(h.status_code, StatusCode::Ok);
    EXPECT_EQ(h.headers, g.headers);
    EXPECT_EQ(h.body.size(), g.body.size()); // Content-Length must match GET
}

TEST_F(StaticFileHandlerTest, RelativeSymlinkInsideRootIsServed)
{
    fs::create_symlink("index.html", m_root / "alias.html");
    fs::create_symlink("../index.html", m_root / "sub" / "up-alias.html"); // leaves sub/, stays in the root
    for (std::string_view t: {"/static/alias.html", "/static/sub/up-alias.html"})
    {
        const Response resp = get(t);
        EXPECT_EQ(resp.status_code, StatusCode::Ok) << t;
        EXPECT_EQ(resp.body, kIndex) << t;
    }
}

TEST_F(StaticFileHandlerTest, AbsoluteSymlinkIsRefusedEvenWhenItPointsInside)
{
    // RESOLVE_BENEATH refuses absolute links outright, so deploy roots with relative links only.
    fs::create_symlink(m_root / "index.html", m_root / "abs-alias.html");
    EXPECT_EQ(get("/static/abs-alias.html").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, RelativeRootIsResolvedAtConstruction)
{
    const fs::path old = fs::current_path();
    fs::current_path(m_dir);
    const StaticFileHandler h(fs::path("www"), "/static/");
    fs::current_path(old); // later cwd changes must not matter
    EXPECT_EQ(h(makeRequest("/static/index.html")).status_code, StatusCode::Ok);
}

// ---------- not found ----------

TEST_F(StaticFileHandlerTest, MissingFileIs404)
{
    EXPECT_EQ(get("/static/missing.txt").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/static/sub/missing.txt").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, DirectoryIs404)
{
    EXPECT_EQ(get("/static/").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/static/sub").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/static/sub/").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, FileUsedAsDirectoryIs404)
{
    EXPECT_EQ(get("/static/index.html/x").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, FifoIs404WithoutBlocking)
{
    ASSERT_EQ(::mkfifo((m_root / "pipe").c_str(), 0600), 0);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(get("/static/pipe").status_code, StatusCode::NotFound);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST_F(StaticFileHandlerTest, TargetOutsideMountPrefixIs404)
{
    EXPECT_EQ(get("/other/index.html").status_code, StatusCode::NotFound);
    EXPECT_EQ(get("/index.html").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, MissingRootFailsClosed)
{
    const StaticFileHandler h(m_dir / "no-such-root", "/static/");
    EXPECT_EQ(h(makeRequest("/static/index.html")).status_code, StatusCode::NotFound);
}

// ---------- traversal ----------

TEST_F(StaticFileHandlerTest, DotDotSegmentsAreBlocked)
{
    expectBlocked("/static/../secret.txt");
    expectBlocked("/static/sub/../../secret.txt");
    expectBlocked("/static/..");
    expectBlocked("/static/../www-evil/secret.txt");
}

TEST_F(StaticFileHandlerTest, PercentEncodedTraversalIsBlocked)
{
    expectBlocked("/static/%2e%2e/secret.txt");
    expectBlocked("/static/%2E%2E/secret.txt");
    expectBlocked("/static/..%2fsecret.txt");
    expectBlocked("/static/..%2Fsecret.txt");
    expectBlocked("/static/%2e%2e%2fsecret.txt");
    expectBlocked("/static/.%2e/secret.txt");
}

TEST_F(StaticFileHandlerTest, AbsolutePathAfterPrefixIsBlocked)
{
    // root / "/etc/passwd" == "/etc/passwd": a leading slash would replace the root entirely.
    const std::string abs = "/static/" + (m_dir / "secret.txt").string(); // "/static//tmp/.../secret.txt"
    expectBlocked(abs);
    expectBlocked("/static/%2F" + (m_dir / "secret.txt").relative_path().string());
}

TEST_F(StaticFileHandlerTest, BackslashIsBlocked)
{
    expectBlocked("/static/..\\secret.txt");
    expectBlocked("/static/..%5csecret.txt");
    expectBlocked("/static/..%5Csecret.txt");
}

TEST_F(StaticFileHandlerTest, NulByteIsBlocked)
{
    expectBlocked("/static/index.html%00.txt");
    expectBlocked(std::string_view("/static/index.html\0.txt", 23));
}

TEST_F(StaticFileHandlerTest, MalformedPercentEncodingIsRejected)
{
    for (std::string_view t: {"/static/%", "/static/%4", "/static/%zz", "/static/index%2.html", "/static/%g0"})
    {
        EXPECT_NE(get(t).status_code, StatusCode::Ok) << t;
    }
}

TEST_F(StaticFileHandlerTest, SymlinkEscapingRootIsBlocked)
{
    fs::create_symlink(m_dir / "secret.txt", m_root / "leak-abs.txt");
    fs::create_symlink("../secret.txt", m_root / "leak-rel.txt");
    fs::create_symlink("../../secret.txt", m_root / "sub" / "leak-deep.txt");
    expectBlocked("/static/leak-abs.txt");
    expectBlocked("/static/leak-rel.txt");
    expectBlocked("/static/sub/leak-deep.txt");
}

TEST_F(StaticFileHandlerTest, SymlinkedDirectoryEscapingRootIsBlocked)
{
    fs::create_directory_symlink(m_dir, m_root / "up-abs");
    fs::create_directory_symlink("..", m_root / "up-rel");
    expectBlocked("/static/up-abs/secret.txt");
    expectBlocked("/static/up-rel/secret.txt");
    // Out of the root and back in: refused too, the kernel checks every step.
    EXPECT_EQ(get("/static/up-rel/www/index.html").status_code, StatusCode::NotFound);
}

TEST_F(StaticFileHandlerTest, SiblingDirectorySharingRootPrefixIsBlocked)
{
    // "/tmp/x/www-evil" starts with the string "/tmp/x/www": a string prefix check would let it through.
    fs::create_symlink("../www-evil/secret.txt", m_root / "evil.txt");
    expectBlocked("/static/evil.txt");
}

TEST_F(StaticFileHandlerTest, RootRenamedAfterStartupStillServesTheSameDirectory)
{
    // The root fd pins the directory; a path swapped in afterwards is not followed.
    const StaticFileHandler h = handler();
    fs::rename(m_root, m_dir / "www-old");
    fs::create_directory(m_root);
    write(m_root / "index.html", "IMPOSTOR");
    const Response resp = h(makeRequest("/static/index.html"));
    EXPECT_EQ(resp.status_code, StatusCode::Ok);
    EXPECT_EQ(resp.body, kIndex);
}

TEST_F(StaticFileHandlerTest, AbsoluteSymlinkToSystemFileIsBlocked)
{
    fs::create_symlink("/etc/passwd", m_root / "passwd");
    EXPECT_NE(get("/static/passwd").status_code, StatusCode::Ok);
}

// ---------- method and connection ----------

TEST_F(StaticFileHandlerTest, PostIs405WithAllow)
{
    const Response resp = handler()(makeRequest("/static/index.html", HttpMethod::POST));
    EXPECT_EQ(resp.status_code, StatusCode::MethodNotAllowed);
    EXPECT_NE(resp.headers.find("Allow: GET, HEAD"), std::string::npos) << "headers: " << resp.headers;
}

TEST_F(StaticFileHandlerTest, ConnectionCloseIsReflected)
{
    const Response keep = get("/static/index.html");
    EXPECT_FALSE(keep.close_connection);

    const Response close =
            handler()(makeRequest("/static/index.html", HttpMethod::GET, "Host: x\r\nConnection: close\r\n"));
    EXPECT_TRUE(close.close_connection);
}
