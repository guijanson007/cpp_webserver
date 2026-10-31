#include <client/Client.h>
#include <csignal>
#include <cstdlib>

static volatile sig_atomic_t shouldQuit = 0;

void quitProgram(int signum)
{
    std::cout << "\nExiting program...\n";
    shouldQuit = 1;
}

int main(int argc, char **argv)
{
    int fd_socket = -1;
    int port = 0;
    char *ip = nullptr;
    std::string message;

    if (argc != 3)
    {
        throw std::runtime_error("ERROR: The number of arguments must be 2 (IP and port)\n");
    }

    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = quitProgram;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    ip = argv[1];
    port = atoi(argv[2]);

    Client client(ip, port);
    client.start();

    std::cout << "Connected to server\n";

    while (shouldQuit == 0)
    {
        std::cout << "Enter the message to send to the server: ";
        if (!std::getline(std::cin, message))
        {
            break;
        }
        client.sendMessage(message);
    }

    return 0;
}
