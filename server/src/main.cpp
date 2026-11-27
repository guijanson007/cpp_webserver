//
// Created by guijanson on 10/23/26.
//

#include <arpa/inet.h>
#include <common/FileDescriptor.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <network/EpollReactor.h>
#include <server/Server.h>
#include <sys/epoll.h>
#include "common/Error.h"
#include "http/Router.h"
#include "http/StaticFileHandler.h"
#include "server/ServerConfig.h"
#include "server/ServerConfigParser.h"

Result<ServerConfig, std::string> parseArguments(std::span<char *> argv)
{
    ServerConfig config{};

    for (size_t i = 1; i < argv.size(); ++i)
    {
        std::string arg = argv[i];

        if (arg == "--port" && i + 1 < argv.size())
        {
            auto port = ServerConfigParser::parsePort(argv[++i]);
            if (!port)
                return std::format("Invalid port: {}", port.error());

            config.port = port.value();
        }
        else if (arg == "--max-connections" && i + 1 < argv.size())
        {
            auto maxConnections = ServerConfigParser::parseMaxConnections(argv[++i]);
            if (!maxConnections)
                return std::format("Invalid max connections: {}", maxConnections.error());

            config.maxConnections = maxConnections.value();
        }
        else if ((arg == "--backlog" || arg == "--backlog-size") && i + 1 < argv.size())
        {
            auto backlog = ServerConfigParser::parseBacklogSize(argv[++i]);
            if (!backlog)
                return std::format("Invalid backlog size: {}", backlog.error());

            config.backlogSize = backlog.value();
        }
        else if (arg == "--root" && i + 1 < argv.size())
        {
            auto rootDirectory = ServerConfigParser::parseRootDirectory(argv[++i]);

            if (!rootDirectory)
            {
                return std::format("Invalid root directory: {}", rootDirectory.error().message());
            }

            config.rootDirectory = std::move(rootDirectory.value());
        }
        else if (arg == "--idle-timeout" && i + 1 < argv.size())
        {
            auto idleTimeout = ServerConfigParser::parseIdleTimeout(argv[++i]);
            if (!idleTimeout)
                return std::format("Invalid idle timeout: {}", idleTimeout.error());

            config.idleTimeout = idleTimeout.value();
        }
        else
        {
            std::string errorMsg = std::format("Unknown or incomplete argument: {}", arg);
            errorMsg += "\nUsage: " + std::string(argv[0]) +
                        " [--port <port>] [--max-connections <max_connections>] [--backlog <backlog_size>] [--root "
                        "<root_directory>] [--idle-timeout <seconds>]";
            return errorMsg;
        }
    }
    return config;
}

void registerRoutes(http::Router &router, const std::filesystem::path &rootDirectory)
{
    router.add(http::HttpMethod::GET, "/", [](const http::Request &)
               { return http::Response(http::StatusCode::Ok, "Hello, World!", "Content-Type: text/plain", false); });

    router.add(http::HttpMethod::POST, "/echo",
               [](const http::Request &request)
               {
                   return http::Response(http::StatusCode::Ok, std::string(request.body), "Content-Type: text/plain",
                                         false);
               });

    router.mount("/static/", http::StaticFileHandler(rootDirectory, "/static/"));
}

int main(int argc, char *argv[])
{
    /* Optimization: Disable synchronization between C and C++ standard streams for performance. */
    std::ios_base::sync_with_stdio(false);

    try
    {
        network::EpollReactor epollReactor; // might throw (expected if epoll_create1 fails)
        // Exception-free parsing
        auto configResult = parseArguments(std::span(argv, argc));
        if (!configResult)
        {
            std::cerr << std::format("Error parsing arguments: {}\n", configResult.error());
            return EXIT_FAILURE;
        }

        // Configure the router
        const ServerConfig &config = configResult.value();

        if (!config.rootDirectory)
        {
            std::cerr << "Error: Root directory not specified. Use --root <root_directory> to specify the root "
                         "directory.\n";
            return EXIT_FAILURE;
        }

        http::Router router;
        registerRoutes(router, *config.rootDirectory);

        Server server(configResult.value(), epollReactor, std::move(router));

        if (auto result = server.start(); !result)
        {
            std::cerr << std::format("Failed to start server: {}", result.error().message()) << "\n";
            return EXIT_FAILURE;
        }

        std::cout << std::format("Server is listening on port {}...", server.getPort()) << "\n";
        while (true)
        {
            if (!epollReactor.run())
            {
                break;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << std::format("Failed to create EpollReactor: {}\n", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
