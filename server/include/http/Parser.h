// Parser.h
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include "http/Request.h"

namespace http
{
    struct Done
    {
        Request request;
        size_t consumed_bytes;
    };

    struct NeedMore
    {
    };
    enum class ParseError : uint16_t
    {
        BadRequest = 400,
        NotFound = 404,
        MethodNotAllowed = 405,
        ContentTooLarge = 413,
        UriTooLong = 414,
        HeaderTooLarge = 431,
        InternalServerError = 500,
        NotImplemented = 501,
        BadVersion = 505,
    };

    using ParseOutcome = std::variant<Done, NeedMore, ParseError>;

    [[nodiscard]] constexpr bool isValidToken(std::string_view token) noexcept
    {
        if (token.empty())
            return false;
        for (unsigned char c: token)
        {
            // 0x20 (space) to 0x7E (tilde) are valid ASCII characters for tokens, excluding special characters.
            if (c <= 0x20 || c >= 0x7F)
                return false;
            switch (c)
            {
                case '(':
                case ')':
                case '<':
                case '>':
                case '@':
                case ',':
                case ';':
                case ':':
                case '\\':
                case '"':
                case '/':
                case '[':
                case ']':
                case '?':
                case '=':
                case '{':
                case '}':
                    return false;
                default:
                    break;
            }
        }
        return true;
    }

    /*
     * Lifetime Contract:
     * The `Done` outcome holds a `Request` containing `std::string_view`s that point
     * directly into the provided `buffer`. These views are only valid as long as the
     * underlying buffer remains unmodified and unallocated.
     *
     * Performance Note:
     * Parsing is stateless. On `NeedMore`, the next call will rescan the buffer from
     * the beginning. This incurs an O(N^2) cost byte-by-byte for incomplete reads,
     * which is acceptable for typical head size limits (e.g., 8 KiB).
     */
    [[nodiscard]] inline ParseOutcome parse(std::string_view buffer, const SizeLimits &limits = {})
    {
        size_t line_end = 0;
        while (line_end < buffer.size())
        {
            char c = buffer[line_end];
            if (c == '\0')
                return ParseError::BadRequest;
            if (c == '\r')
            {
                if (line_end + 1 >= buffer.size())
                    return {NeedMore{}};
                if (buffer[line_end + 1] != '\n')
                    return ParseError::BadRequest;
                break;
            }
            if (c == '\n')
                return ParseError::BadRequest;
            ++line_end;
        }

        if (line_end >= buffer.size())
        {
            if (buffer.size() > limits.maxRequestLineSize)
                return ParseError::UriTooLong;
            return {NeedMore{}};
        }

        if (line_end > limits.maxRequestLineSize)
            return ParseError::UriTooLong;

        std::string_view request_line = buffer.substr(0, line_end);

        size_t method_end = request_line.find(' ');
        if (method_end == std::string_view::npos)
            return ParseError::BadRequest;

        std::string_view method_str = request_line.substr(0, method_end);
        HttpMethod method;
        if (method_str == "GET")
            method = HttpMethod::GET;
        else if (method_str == "POST")
            method = HttpMethod::POST;
        else if (method_str == "HEAD")
            method = HttpMethod::HEAD;
        else
            return ParseError::NotImplemented;

        size_t target_start = method_end + 1;
        size_t target_end = request_line.find(' ', target_start);
        if (target_end == std::string_view::npos)
            return ParseError::BadRequest;

        std::string_view target = request_line.substr(target_start, target_end - target_start);
        if (target.empty())
            return ParseError::BadRequest;

        std::string_view version = request_line.substr(target_end + 1);
        if (version != "HTTP/1.1" && version != "HTTP/1.0")
        {
            if (version.size() == 8 && version.starts_with("HTTP/") &&
                std::isdigit(static_cast<unsigned char>(version[5])) && version[6] == '.' &&
                std::isdigit(static_cast<unsigned char>(version[7])))
            {
                return ParseError::BadVersion;
            }
            return ParseError::BadRequest;
        }

        size_t cursor = line_end + 2;
        size_t header_count = 0;
        bool seen_content_length = false;
        bool seen_host = false;

        while (cursor < buffer.size())
        {
            if (cursor + 1 < buffer.size() && buffer[cursor] == '\r' && buffer[cursor + 1] == '\n')
            {
                cursor += 2;
                if (!seen_host && version == "HTTP/1.1")
                    return ParseError::BadRequest;

                std::string_view raw_headers = buffer.substr(line_end + 2, (cursor - 2) - (line_end + 2));
                return {Done{Request{.header = raw_headers,
                                     .body = {},
                                     .target = target,
                                     .version = version,
                                     .method = method},
                             cursor}};
            }

            if (cursor < buffer.size() && buffer[cursor] == '\r' && cursor + 1 == buffer.size())
            {
                return {NeedMore{}};
            }

            if (buffer[cursor] == ' ' || buffer[cursor] == '\t')
                return ParseError::BadRequest;

            size_t h_line_end = cursor;
            while (h_line_end < buffer.size())
            {
                char c = buffer[h_line_end];
                if (c == '\0')
                    return ParseError::BadRequest;
                if (c == '\r')
                {
                    if (h_line_end + 1 >= buffer.size())
                        return {NeedMore{}};
                    if (buffer[h_line_end + 1] != '\n')
                        return ParseError::BadRequest;
                    break;
                }
                if (c == '\n')
                    return ParseError::BadRequest;
                ++h_line_end;
            }

            if (h_line_end >= buffer.size())
            {
                if (buffer.size() > limits.maxHeaderSize)
                    return ParseError::HeaderTooLarge;
                return {NeedMore{}};
            }

            if (h_line_end + 4 > limits.maxHeaderSize)
                return ParseError::HeaderTooLarge;

            if (++header_count > limits.maxHeaderCount)
                return ParseError::HeaderTooLarge;

            std::string_view h_line = buffer.substr(cursor, h_line_end - cursor);
            cursor = h_line_end + 2;

            size_t colon = h_line.find(':');
            if (colon == 0 || colon == std::string_view::npos)
                return ParseError::BadRequest;
            if (h_line[colon - 1] == ' ' || h_line[colon - 1] == '\t')
                return ParseError::BadRequest;

            std::string_view name = h_line.substr(0, colon);
            if (!isValidToken(name))
                return ParseError::BadRequest;

            if (iequals(name, "Content-Length"))
            {
                if (seen_content_length)
                    return ParseError::BadRequest;
                seen_content_length = true;
            }
            else if (iequals(name, "Host"))
            {
                seen_host = true;
            }
        }

        if (buffer.size() > limits.maxHeaderSize)
            return ParseError::HeaderTooLarge;
        return {NeedMore{}};
    }
} // namespace http
