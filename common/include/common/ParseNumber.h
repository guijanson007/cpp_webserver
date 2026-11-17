#pragma once

#include <charconv>
#include <concepts>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include "Result.h"

template<std::integral T>
Result<T, std::string> parseInteger(std::string_view str, T min = std::numeric_limits<T>::min(),
                                    T max = std::numeric_limits<T>::max())
{
    if (str.empty())
    {
        return std::string("Empty value provided.");
    }

    T value{};
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);

    if (ec != std::errc() || ptr != str.data() + str.size())
    {
        return std::format("Invalid integer format: {}", str);
    }

    if (value < min || value > max)
    {
        return std::format("Value out of range. Expected {}-{}, got {}", min, max, value);
    }

    return value;
}

// ParseNumber.h
