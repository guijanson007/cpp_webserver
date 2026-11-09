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
#include "server/ServerConfig.h"

ServerConfig parseArguments(int argc, char *argv[])
{
    // Start with default values
    ServerConfig config{};

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--port" && i + 1 < argc)
        {
            config.port = std::stoi(argv[++i]);
        }
        else if (arg == "--max-connections" && i + 1 < argc)
        {
            config.maxConnections = std::stoi(argv[++i]);
        }
        else if ((arg == "--backlog" || arg == "--backlog-size") && i + 1 < argc)
        {
            config.backlogSize = std::stoi(argv[++i]);
        }
        else
        {
            throw std::invalid_argument("Unknown or incomplete argument: " + arg);
        }
    }

    return config;
}

int main(int argc, char *argv[])
{
    /* Optimization: Disable synchronization between C and C++ standard streams for performance. */
    std::ios_base::sync_with_stdio(false);

    network::EpollReactor epollReactor; // might throw (expected if epoll_create1 fails)

    ServerConfig config;
    try
    {

        config = parseArguments(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::cerr << std::format("Error parsing arguments: {}", e.what()) << std::endl;
        return EXIT_FAILURE;
    }

    Server server(config, epollReactor);

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
