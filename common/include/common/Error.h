#pragma once

#include <string>
#include <system_error>

class Error
{
public:
    explicit Error(std::error_code error_code) : m_error_code(error_code) {}
    Error(int error_number) : m_error_code(error_number, std::generic_category()) {}

    [[nodiscard]] int code() const { return m_error_code.value(); }

    [[nodiscard]] std::string message() const { return m_error_code.message(); }

private:
    std::error_code m_error_code;
};
