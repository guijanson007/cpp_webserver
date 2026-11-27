// WEB-11: extension -> Content-Type lookup.
// API under test (written by the user in server/include/http/MimeTypes.h):
//   std::string_view http::mimeTypeForExtension(std::string_view ext);   // ext includes the dot
//   std::string_view http::mimeTypeForPath(const std::filesystem::path &);
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string_view>
#include <utility>

#include "http/MimeTypes.h"

namespace
{
    using http::mimeTypeForExtension;
    using http::mimeTypeForPath;

    constexpr std::string_view kOctet = "application/octet-stream";
} // namespace

TEST(MimeTest, EveryTicketExtensionIsMapped)
{
    const auto expected = std::to_array<std::pair<std::string_view, std::string_view>>({
            {".html", "text/html"},       {".css", "text/css"},   {".js", "application/javascript"},
            {".json", "application/json"}, {".txt", "text/plain"}, {".png", "image/png"},
            {".jpg", "image/jpeg"},       {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
            {".svg", "image/svg+xml"},    {".ico", "image/x-icon"},
    });
    for (const auto &[ext, mime]: expected)
    {
        EXPECT_EQ(mimeTypeForExtension(ext), mime) << "extension " << ext;
    }
}

TEST(MimeTest, LookupIsCaseInsensitive)
{
    EXPECT_EQ(mimeTypeForExtension(".HTML"), "text/html");
    EXPECT_EQ(mimeTypeForExtension(".Png"), "image/png");
    EXPECT_EQ(mimeTypeForPath("/srv/www/PHOTO.JPG"), "image/jpeg");
}

TEST(MimeTest, UnknownExtensionFallsBackToOctetStream)
{
    EXPECT_EQ(mimeTypeForExtension(".exe"), kOctet);
    EXPECT_EQ(mimeTypeForExtension(".htm"), kOctet); // not in the table; exact match only
    EXPECT_EQ(mimeTypeForExtension(".htmlx"), kOctet); // no prefix matching
    EXPECT_EQ(mimeTypeForExtension(""), kOctet);
    EXPECT_EQ(mimeTypeForExtension("."), kOctet);
}

TEST(MimeTest, ExtensionWithoutDotIsNotMatched)
{
    // The table keys carry the dot, as path::extension() returns it.
    EXPECT_EQ(mimeTypeForExtension("html"), kOctet);
}

TEST(MimeTest, PathUsesLastExtensionOnly)
{
    EXPECT_EQ(mimeTypeForPath("archive.tar.gz"), kOctet);
    EXPECT_EQ(mimeTypeForPath("app.min.js"), "application/javascript");
    EXPECT_EQ(mimeTypeForPath("report.html.txt"), "text/plain");
}

TEST(MimeTest, PathWithoutExtension)
{
    EXPECT_EQ(mimeTypeForPath("Makefile"), kOctet);
    EXPECT_EQ(mimeTypeForPath("/srv/www/"), kOctet);
    EXPECT_EQ(mimeTypeForPath(""), kOctet);
}

TEST(MimeTest, DotfileHasNoExtension)
{
    // path(".html").extension() is empty: a leading dot names the file, it is not an extension.
    EXPECT_EQ(mimeTypeForPath(".html"), kOctet);
    EXPECT_EQ(mimeTypeForPath("dir/.css"), kOctet);
}

TEST(MimeTest, DotInDirectoryNameIsIgnored)
{
    EXPECT_EQ(mimeTypeForPath("/srv/site.html/readme"), kOctet);
    EXPECT_EQ(mimeTypeForPath("/srv/v1.2/style.css"), "text/css");
}
