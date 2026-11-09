#pragma once

#include <optional>
#include <variant>
#include "common/Error.h"

template<typename T, typename E = Error>
class Result
{
public:
    constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : m_data(std::in_place_type<T>, std::move(value))
    {
    }
    constexpr Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : m_data(std::in_place_type<E>, std::move(error))
    {
    }

    explicit constexpr operator bool() const noexcept { return std::holds_alternative<T>(m_data); }
    [[nodiscard]] constexpr bool ok() const noexcept { return std::holds_alternative<T>(m_data); }
    [[nodiscard]] constexpr const T &value() const & { return std::get<T>(m_data); }
    [[nodiscard]] constexpr T &value() & { return std::get<T>(m_data); }
    [[nodiscard]] constexpr T &&value() && { return std::get<T>(std::move(m_data)); }
    [[nodiscard]] constexpr const E &error() const & { return std::get<E>(m_data); }
    [[nodiscard]] constexpr E &error() & { return std::get<E>(m_data); }
    [[nodiscard]] constexpr E &&error() && { return std::get<E>(std::move(m_data)); }
    [[nodiscard]] constexpr T value_or(const T &default_value) const
    {
        return std::holds_alternative<T>(m_data) ? std::get<T>(m_data) : default_value;
    }

private:
    std::variant<T, E> m_data;
};

template<typename E>
class Result<void, E>
{
public:
    constexpr Result() noexcept : m_error(std::nullopt) {}
    constexpr Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>) : m_error(std::move(error)) {}

    explicit constexpr operator bool() const noexcept { return !m_error.has_value(); }
    [[nodiscard]] constexpr bool ok() const noexcept { return !m_error.has_value(); }
    [[nodiscard]] constexpr const E &error() const & noexcept { return *m_error; }
    [[nodiscard]] constexpr E &error() & noexcept { return *m_error; }
    [[nodiscard]] constexpr E &&error() && noexcept { return *std::move(m_error); }

private:
    std::optional<E> m_error;
};
