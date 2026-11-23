#pragma once

#include <filesystem>
#include <string_view>
#include "common/ParseNumber.h"

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

        if (ec)
        {
            return Error{ec};
        }

        if (!std::filesystem::exists(st))
        {
            return Error{std::make_error_code(std::errc::no_such_file_or_directory)};
        }

        if (!std::filesystem::is_directory(st))
        {
            return Error{std::make_error_code(std::errc::not_a_directory)};
        }

        return path;
    }

private:
    static constexpr int MAX_PORT = 65535;
    static constexpr int MIN_PORT = 1;
    static constexpr int MIN_CONNECTIONS = 1;
    static constexpr int MIN_BACKLOG = 1;
};
