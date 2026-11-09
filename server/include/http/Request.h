#pragma once

#include <common/Result.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "network/Connection.h"

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
        size_t maxRequestLineSize;
        size_t maxHeaderSize;
        size_t maxHeaderCount;
    };

    enum class ErrorCode : uint16_t
    {
        Malformed = 400,
        HeaderTooLarge = 431,
        UnknownMethod = 501,
        BadVersion = 505,
    };

    class Request
    {
    public:
        Request(std::string_view header, std::string_view body, std::string_view target, std::string_view version,
                HttpMethod method, network::Connection &connection)
            : header(header), body(body), target(target), version(version), method(method), connection(connection)
        {
        }

        [[nodiscard]] std::string_view getHeader() const { return header; }
        [[nodiscard]] std::string_view getBody() const { return body; }
        [[nodiscard]] std::string_view getTarget() const { return target; }
        [[nodiscard]] std::string_view getVersion() const { return version; }
        [[nodiscard]] HttpMethod getMethod() const { return method; }
        [[nodiscard]] network::Connection &getConnection() { return connection; }

    private:
        std::string_view header;
        std::string_view body;
        std::string_view target;
        std::string_view version;
        HttpMethod method;
        network::Connection &connection;
    };
} // namespace http
