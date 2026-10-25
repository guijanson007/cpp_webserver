//
// Created by guijanson on 10/25/26.
//

#ifndef SERVER_FILEDESCRIPTOR_H
#define SERVER_FILEDESCRIPTOR_H

#include <unistd.h>
#include <utility>

class FileDescriptor {
public:
    FileDescriptor() = default;

    explicit FileDescriptor(int fd) : m_fd(fd) {}

    ~FileDescriptor() {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    FileDescriptor(const FileDescriptor &) = delete;

    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&o) noexcept
        : m_fd(std::exchange(o.m_fd, -1)) {}

    FileDescriptor &operator=(FileDescriptor &&o) noexcept {
        if (this != &o) {
            if (m_fd >= 0)
                ::close(m_fd);
            m_fd = std::exchange(o.m_fd, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const { return m_fd; }

private:
    int m_fd = -1;
};


#endif //SERVER_FILEDESCRIPTOR_H
