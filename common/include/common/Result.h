#pragma once

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include "common/Error.h"

template<typename T>
concept ValidValueType = (!std::is_void_v<T>) && std::is_nothrow_move_constructible_v<T>;

template<typename E>
concept ValidErrorType = std::is_nothrow_move_constructible_v<E>;

// Primary template: T is non-void and distinct from E
template<typename T, ValidErrorType E = Error>
    requires(std::is_void_v<T> || (ValidValueType<T> && !std::is_same_v<T, E>) )
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

    [[nodiscard]] constexpr T value_or(T &&default_value) const
    {
        return std::holds_alternative<T>(m_data) ? std::get<T>(m_data) : std::move(default_value);
    }

private:
    std::variant<T, E> m_data;
};

template<ValidErrorType E>
    requires(!std::is_same_v<void, E>)
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
