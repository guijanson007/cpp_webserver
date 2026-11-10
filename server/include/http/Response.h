#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace http
{
    enum class StatusCode : uint16_t
    {
        Ok = 200,
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

    [[nodiscard]] constexpr std::string_view statusCodeToReasonPhrase(StatusCode code) noexcept
    {
        switch (code)
        {
            case StatusCode::Ok:
                return "OK";
            case StatusCode::BadRequest:
                return "Bad Request";
            case StatusCode::NotFound:
                return "Not Found";
            case StatusCode::MethodNotAllowed:
                return "Method Not Allowed";
            case StatusCode::ContentTooLarge:
                return "Content Too Large";
            case StatusCode::UriTooLong:
                return "URI Too Long";
            case StatusCode::HeaderTooLarge:
                return "Request Header Fields Too Large";
            case StatusCode::InternalServerError:
                return "Internal Server Error";
            case StatusCode::NotImplemented:
                return "Not Implemented";
            case StatusCode::BadVersion:
                return "HTTP Version Not Supported";
            default:
                return "Unknown";
        }
    }

    struct Response
    {
        StatusCode status_code;
        std::string body;
        std::string headers;
        bool close_connection;

        Response(StatusCode sc, std::string b = "", std::string h = "", bool close = false) noexcept
            : status_code(sc), body(std::move(b)), headers(std::move(h)), close_connection(close)
        {
        }

        [[nodiscard]] std::string serialize() const
        {
            std::string extra_headers = headers;
            if (!extra_headers.empty() && !extra_headers.ends_with("\r\n"))
            {
                extra_headers += "\r\n";
            }

            return std::format("HTTP/1.1 {} {}\r\n"
                               "Content-Length: {}\r\n"
                               "{}"
                               "{}"
                               "\r\n"
                               "{}",
                               static_cast<uint16_t>(status_code), statusCodeToReasonPhrase(status_code), body.size(),
                               close_connection ? "Connection: close\r\n" : "", extra_headers, body);
        }
    };
} // namespace http
