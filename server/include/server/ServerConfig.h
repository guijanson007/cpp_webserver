#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

struct ServerConfig
{
    std::string ip{"0.0.0.0"};
    int port{8080}; // Default port
    int maxConnections{100}; // Default maximum number of connections
    int backlogSize{10'000}; // Default backlog size
    std::optional<std::filesystem::path> rootDirectory; // Default root directory
    std::chrono::seconds idleTimeout{60}; // Default idle timeout
    std::chrono::milliseconds sweepInterval{1000}; // how often the server checks expired connections
};
