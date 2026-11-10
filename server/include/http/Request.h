// Request.h
#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>

namespace http
{
    enum class HttpMethod
    {
        GET,
        POST,
        DELETE,
        HEAD,
    };

    struct SizeLimits
    {
        size_t maxRequestLineSize = 8192;
        size_t maxHeaderSize = 8192;
        size_t maxHeaderCount = 100;
    };


    [[nodiscard]] constexpr bool iequals(std::string_view lhs, std::string_view rhs) noexcept
    {
        if (lhs.size() != rhs.size())
            return false;
        for (size_t i = 0; i < lhs.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
                return false;
        }
        return true;
    }

    struct Request
    {
        std::string_view header;
        std::string_view body;
        std::string_view target;
        std::string_view version;
        HttpMethod method;

        [[nodiscard]] std::optional<std::string_view> getHeaderValue(std::string_view name) const
        {
            size_t cursor = 0;
            while (cursor < header.size())
            {
                size_t next = header.find("\r\n", cursor);
                if (next == std::string_view::npos)
                    break;

                std::string_view line = header.substr(cursor, next - cursor);
                cursor = next + 2;

                if (line.empty())
                    continue;

                size_t colon = line.find(':');
                if (colon != std::string_view::npos)
                {
                    std::string_view key = line.substr(0, colon);
                    if (iequals(key, name))
                    {
                        std::string_view value = line.substr(colon + 1);
                        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                            value.remove_prefix(1);
                        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                            value.remove_suffix(1);
                        return value;
                    }
                }
            }
            return std::nullopt;
        }
        [[nodiscard]] bool shouldKeepAlive() const
        {
            bool has_close = false;
            bool has_keep_alive = false;
            auto conn_header = getHeaderValue("Connection");

            if (conn_header)
            {
                std::string_view val = *conn_header;
                size_t start = 0;
                while (start < val.size())
                {
                    size_t end = val.find(',', start);
                    std::string_view token =
                            (end == std::string_view::npos) ? val.substr(start) : val.substr(start, end - start);

                    // Trim Optional Whitespace (OWS)
                    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                        token.remove_prefix(1);
                    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                        token.remove_suffix(1);

                    if (iequals(token, "close"))
                        has_close = true;
                    else if (iequals(token, "keep-alive"))
                        has_keep_alive = true;

                    if (end == std::string_view::npos)
                        break;
                    start = end + 1;
                }
            }

            if (version == "HTTP/1.0")
            {
                return has_keep_alive && !has_close;
            }
            else
            {
                return !has_close;
            }
        }
    };
} // namespace http
