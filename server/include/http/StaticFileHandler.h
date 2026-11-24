#pragma once

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include "common/FileReader.h"
#include "http/MimeTypes.h"
#include "http/Request.h"
#include "http/Response.h"

namespace http
{
    class StaticFileHandler
    {
    public:
        StaticFileHandler(const std::filesystem::path &rootDirectory, std::string mountPrefix = "/")
            : m_mountPrefix(std::move(mountPrefix))
        {
            if (auto dirResult = openDirectory(rootDirectory))
            {
                m_rootFd = std::move(dirResult.value());
            }
            // If opening fails, m_rootFd stays null -> fail closed (404)
        }

        [[nodiscard]] Response operator()(const Request &request) const
        {
            const bool closeConn = !request.shouldKeepAlive();

            if (!m_rootFd || !m_rootFd->valid())
            {
                return {StatusCode::NotFound, "", "", closeConn};
            }

            if (request.method != HttpMethod::GET && request.method != HttpMethod::HEAD)
            {
                return {StatusCode::MethodNotAllowed, "", "Allow: GET, HEAD", closeConn};
            }

            const auto queryPos = request.target.find('?');
            std::string_view rawPath =
                    (queryPos == std::string_view::npos) ? request.target : request.target.substr(0, queryPos);

            if (!rawPath.starts_with(m_mountPrefix))
            {
                return {StatusCode::NotFound, "", "", closeConn};
            }

            rawPath.remove_prefix(m_mountPrefix.size());

            auto decodedOpt = percentDecode(rawPath);
            if (!decodedOpt)
            {
                return {StatusCode::NotFound, "", "", closeConn};
            }

            const std::string &relStr = *decodedOpt;
            if (!isSafeRelativePath(relStr))
            {
                return {StatusCode::NotFound, "", "", closeConn};
            }

            // Single atomic kernel lookup confined to m_rootFd
            auto fileResult = readRegularFileBeneath(m_rootFd->get(), relStr);
            if (!fileResult)
            {
                return {StatusCode::NotFound, "", "", closeConn};
            }

            const std::string_view mime = mimeTypeForPath(std::filesystem::path(relStr));
            return {StatusCode::Ok, std::move(fileResult.value()), std::format("Content-Type: {}", mime), closeConn};
        }

    private:
        SharedDirFd m_rootFd;
        std::string m_mountPrefix;

        [[nodiscard]] static std::optional<std::string> percentDecode(std::string_view input)
        {
            auto hexVal = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F')
                    return 10 + (c - 'A');
                return -1;
            };

            std::string out;
            out.reserve(input.size());

            for (size_t i = 0; i < input.size(); ++i)
            {
                if (input[i] == '%')
                {
                    if (i + 2 >= input.size())
                        return std::nullopt;
                    const int hi = hexVal(input[i + 1]);
                    const int lo = hexVal(input[i + 2]);
                    if (hi < 0 || lo < 0)
                        return std::nullopt;
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                }
                else
                {
                    out.push_back(input[i]);
                }
            }
            return out;
        }

        [[nodiscard]] static bool isSafeRelativePath(std::string_view path) noexcept
        {
            if (path.empty() || path.front() == '/')
            {
                return false;
            }

            size_t segStart = 0;
            for (size_t i = 0; i <= path.size(); ++i)
            {
                if (i < path.size())
                {
                    const char c = path[i];
                    if (c == '\0' || c == '\\')
                    {
                        return false;
                    }
                }

                if (i == path.size() || path[i] == '/')
                {
                    const std::string_view segment = path.substr(segStart, i - segStart);
                    // Reject "..", ".", and hidden files/directories (e.g., .git, .htaccess)
                    if (segment.starts_with('.'))
                    {
                        return false;
                    }
                    segStart = i + 1;
                }
            }

            return true;
        }
    };
} // namespace http
