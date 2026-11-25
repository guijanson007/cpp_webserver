#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include "common/Error.h"
#include "common/ParseNumber.h"
#include "common/Result.h"

class ServerConfigParser
{
public:
    static Result<int, std::string> parsePort(std::string_view portStr)
    {

        return parseInteger<int>(portStr, MIN_PORT, MAX_PORT);
    }

    static Result<int, std::string> parseMaxConnections(std::string_view maxConnectionsStr)
    {
        return parseInteger<int>(maxConnectionsStr, MIN_CONNECTIONS);
    }

    static Result<int, std::string> parseBacklogSize(std::string_view backlogSizeStr)
    {
        return parseInteger<int>(backlogSizeStr, MIN_BACKLOG);
    }

    static Result<std::filesystem::path> parseRootDirectory(std::string_view rootDirectoryStr)
    {
        if (rootDirectoryStr.empty())
        {
            return Error{std::make_error_code(std::errc::invalid_argument)};
        }

        std::filesystem::path path(rootDirectoryStr);
        std::error_code ec;
        const auto st = std::filesystem::status(path, ec);

        if (st.type() == std::filesystem::file_type::not_found)
        {
            return Error{std::make_error_code(std::errc::no_such_file_or_directory)};
        }

        if (ec)
        {
            return Error{ec};
        }

        if (!std::filesystem::is_directory(st))
        {
            return Error{std::make_error_code(std::errc::not_a_directory)};
        }

        return path;
    }

    static Result<std::chrono::seconds, std::string> parseIdleTimeout(std::string_view idleTimeoutStr)
    {
        auto result = parseInteger<int>(idleTimeoutStr, MIN_IDLE_TIMEOUT.count(), MAX_IDLE_TIMEOUT.count());

        if (!result)
        {
            return result.error();
        }

        return std::chrono::seconds(result.value());
    }

private:
    static constexpr std::chrono::duration MIN_IDLE_TIMEOUT = std::chrono::seconds{0}; // 0 seconds means no timeout
    static constexpr std::chrono::duration MAX_IDLE_TIMEOUT = std::chrono::seconds{3600}; // 1 hour
    static constexpr int MAX_PORT = 65535;
    static constexpr int MIN_PORT = 1;
    static constexpr int MIN_CONNECTIONS = 1;
    static constexpr int MIN_BACKLOG = 1;
};
