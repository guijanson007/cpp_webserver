#pragma once

#include <filesystem>
#include <string>

struct ServerConfig
{
    std::string ip{"0.0.0.0"};
    int port{8080}; // Default port
    int maxConnections{100}; // Default maximum number of connections
    int backlogSize{10'000}; // Default backlog size
    std::filesystem::path rootDirectory{"."}; // Default root directory
};
