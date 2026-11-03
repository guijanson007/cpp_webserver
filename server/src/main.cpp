//
// Created by guijanson on 10/23/26.
//

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <string>
#include <sys/epoll.h>

#include <common/FileDescriptor.h>
#include <network/EpollReactor.h>
#include <server/Server.h>

int main()
{
    network::EpollReactor epollReactor; // might throw (expected if epoll_create1 fails)

    Server server("0.0.0.0", 8080, epollReactor);
    try
    {
        server.start();
    }
    catch (const std::exception &e)
    {
        // Handle exception
        std::cout << std::format("{}", e.what()) << std::endl;
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
