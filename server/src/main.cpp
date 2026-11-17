//
// Created by guijanson on 10/23/26.
//

#include <arpa/inet.h>
#include <common/FileDescriptor.h>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <network/EpollReactor.h>
#include <server/Server.h>
#include <sys/epoll.h>
#include "common/Error.h"
#include "common/ParseNumber.h"
#include "http/Router.h"
#include "server/ServerConfig.h"

constexpr int MAX_PORT = 65535;
constexpr int MIN_PORT = 1;
constexpr int MIN_CONNECTIONS = 1;
constexpr int MIN_BACKLOG = 1;

Result<ServerConfig, std::string> parseArguments(int argc, char *argv[])
{
    ServerConfig config{};

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--port" && i + 1 < argc)
        {
            auto valRes = parseInteger<int>(argv[++i], MIN_PORT, MAX_PORT);
            if (!valRes)
                return std::format("Invalid port: {}", valRes.error());

            config.port = valRes.value();
        }
        else if (arg == "--max-connections" && i + 1 < argc)
        {
            auto valRes = parseInteger<int>(argv[++i], MIN_CONNECTIONS);
            if (!valRes)
                return std::format("Invalid max connections: {}", valRes.error());

            config.maxConnections = valRes.value();
        }
        else if ((arg == "--backlog" || arg == "--backlog-size") && i + 1 < argc)
        {
            auto valRes = parseInteger<int>(argv[++i], MIN_BACKLOG);
            if (!valRes)
                return std::format("Invalid backlog size: {}", valRes.error());

            config.backlogSize = valRes.value();
        }
        else
        {
            std::string errorMsg = std::format("Unknown or incomplete argument: {}", arg);
            errorMsg += "\nUsage: " + std::string(argv[0]) +
                        " [--port <port>] [--max-connections <max_connections>] [--backlog <backlog_size>]";
            return errorMsg;
        }
    }
    return config;
}

int main(int argc, char *argv[])
{
    /* Optimization: Disable synchronization between C and C++ standard streams for performance. */
    std::ios_base::sync_with_stdio(false);

    network::EpollReactor epollReactor; // might throw (expected if epoll_create1 fails)

    // Exception-free parsing
    auto configResult = parseArguments(argc, argv);
    if (!configResult)
    {
        std::cerr << std::format("Error parsing arguments: {}\n", configResult.error());
        return EXIT_FAILURE;
    }

    // Configure the router
    http::Router router;
    router.add(http::HttpMethod::GET, "/", [](const http::Request &)
               { return http::Response(http::StatusCode::Ok, "Hello, World!", "Content-Type: text/plain", false); });
    router.add(http::HttpMethod::POST, "/echo",
               [](const http::Request &request)
               {
                   return http::Response(http::StatusCode::Ok, std::string(request.body), "Content-Type: text/plain",
                                         false);
               });

    Server server(configResult.value(), epollReactor, router);

    if (auto result = server.start(); !result)
    {
        std::cerr << std::format("Failed to start server: {}", result.error().message()) << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << std::format("Server is listening on port {}...", server.getPort()) << std::endl;
    while (true)
    {
        if (!epollReactor.run())
        {
            break;
        }
    }

    return EXIT_SUCCESS;
}
