#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <utility>
#include "http/Request.h"

namespace http
{
    [[nodiscard]] inline std::string_view mimeTypeForExtension(std::string_view ext) noexcept
    {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> kMimeTable{{
                {".html", "text/html"},
                {".css", "text/css"},
                {".js", "application/javascript"},
                {".json", "application/json"},
                {".txt", "text/plain"},
                {".png", "image/png"},
                {".jpg", "image/jpeg"},
                {".jpeg", "image/jpeg"},
                {".gif", "image/gif"},
                {".svg", "image/svg+xml"},
                {".ico", "image/x-icon"},
        }};

        for (const auto &[suffix, mime]: kMimeTable)
        {
            if (iequals(ext, suffix))
            {
                return mime;
            }
        }
        return "application/octet-stream";
    }

    [[nodiscard]] inline std::string_view mimeTypeForPath(const std::filesystem::path &path)
    {
        const std::string ext = path.extension().string();
        return mimeTypeForExtension(ext);
    }
} // namespace http
