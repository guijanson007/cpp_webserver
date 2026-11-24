#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include "common/Error.h"
#include "common/FileDescriptor.h"
#include "common/Result.h"

using SharedDirFd = std::shared_ptr<const FileDescriptor>;


[[nodiscard]] inline Result<SharedDirFd> openDirectory(const std::filesystem::path &path)
{
    const int fd = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
    {
        return Error{std::error_code(errno, std::generic_category())};
    }
    return std::make_shared<const FileDescriptor>(fd);
}

[[nodiscard]] inline Result<std::string> readRegularFileBeneath(int dirfd, std::string_view relativePath)
{
    if (dirfd < 0 || relativePath.empty())
    {
        return Error{std::make_error_code(std::errc::invalid_argument)};
    }

    // Null-terminated copy for the syscall
    const std::string pathStr(relativePath);

    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;

    const long rawFd = ::syscall(SYS_openat2, dirfd, pathStr.c_str(), &how, sizeof(how));

    if (rawFd < 0)
    {
        return Error{std::error_code(errno, std::generic_category())};
    }

    FileDescriptor fd(static_cast<int>(rawFd));

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0)
    {
        return Error{std::error_code(errno, std::generic_category())};
    }

    if (!S_ISREG(st.st_mode))
    {
        return Error{std::make_error_code(std::errc::no_such_file_or_directory)};
    }

    std::string content;
    if (st.st_size > 0)
    {
        content.resize(static_cast<size_t>(st.st_size));
        size_t totalRead = 0;

        while (totalRead < content.size())
        {
            const ssize_t n = ::read(fd.get(), content.data() + totalRead, content.size() - totalRead);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return Error{std::error_code(errno, std::generic_category())};
            }
            if (n == 0)
            {
                content.resize(totalRead);
                break;
            }
            totalRead += static_cast<size_t>(n);
        }
    }

    return content;
}
