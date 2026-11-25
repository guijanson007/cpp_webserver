#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <unordered_map>
#include "Request.h"
#include "Response.h"

namespace http
{
    // Define the concept for the handler type
    template<typename T>
    concept RequestHandler = requires(T t, const Request &req) {
        // The handler must be callable with a Request and return exactly a Response
        { t(req) } -> std::same_as<Response>;
        // e.g: callable as Response(const Request&)
    };

    // Transparent string hasher for zero-allocation lookups
    struct StringViewHash
    {
        using is_transparent = void; // Enables heterogeneous lookup

        [[nodiscard]] size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
    };

    class Router
    {
    public:
        using HandlerFunc = std::function<Response(const Request &)>;

        template<RequestHandler H>
        void add(HttpMethod method, std::string_view path, H handler)
        {
            // This maps Path -> Array of Methods -> Handler
            m_routes[std::string(path)][static_cast<uint8_t>(method)] = std::move(handler);
        }

        template<RequestHandler H>
        void mount(std::string_view prefix, H handler)
        {
            m_mounts.push_back(Mount{std::string(prefix), std::move(handler)});
            // Longest prefix wins when multiple mounts overlap
            std::stable_sort(m_mounts.begin(), m_mounts.end(),
                             [](const Mount &a, const Mount &b) { return a.prefix.size() > b.prefix.size(); });
        }

        [[nodiscard]] Response route(const Request &request) const
        {
            const auto query_pos = request.target.find('?');
            const std::string_view path =
                    (query_pos == std::string_view::npos) ? request.target : request.target.substr(0, query_pos);

            try
            {
                // 1. Exact routes always win
                if (auto it = m_routes.find(path); it != m_routes.end())
                {
                    return dispatchExact(it->second, request);
                }

                // 2. Prefix mounts (GET / HEAD)
                for (const auto &m: m_mounts)
                {
                    if (matchesPrefix(path, m.prefix))
                    {
                        if (request.method != HttpMethod::GET && request.method != HttpMethod::HEAD)
                        {
                            return {StatusCode::MethodNotAllowed, "", "Allow: GET, HEAD", !request.shouldKeepAlive()};
                        }
                        return m.handler(request);
                    }
                }

                return {StatusCode::NotFound, "", "", !request.shouldKeepAlive()};
            }
            // std::bad_alloc and other exceptions can be thrown by std::function, std::string, etc.
            catch (const std::exception &e)
            {
                std::cerr << "Exception during request handling: " << e.what() << std::endl;
                return {StatusCode::InternalServerError, "", "", true};
            }
            catch (...)
            {
                std::cerr << "Unknown exception during request handling" << std::endl;
                return {StatusCode::InternalServerError, "", "", true};
            }
        }

    private:
        struct Mount
        {
            std::string prefix;
            HandlerFunc handler;
        };

        [[nodiscard]] static bool matchesPrefix(std::string_view path, std::string_view prefix) noexcept
        {
            if (!path.starts_with(prefix))
                return false;
            if (prefix.ends_with('/') || path.size() == prefix.size())
                return true;
            return path[prefix.size()] == '/';
        }

        [[nodiscard]] static Response dispatchExact(const std::array<std::optional<HandlerFunc>, MAX_METHODS> &methods,
                                                    const Request &request)
        {
            const auto method_idx = static_cast<uint8_t>(request.method);

            if (method_idx >= MAX_METHODS || !methods[method_idx])
            {
                if (request.method == HttpMethod::HEAD && methods[static_cast<uint8_t>(HttpMethod::GET)])
                {
                    return methods[static_cast<uint8_t>(HttpMethod::GET)].value()(request);
                }

                std::string allowHeader = "Allow: ";
                bool hasPrevious = false;

                if (methods[static_cast<uint8_t>(HttpMethod::GET)])
                {
                    allowHeader += "GET, HEAD";
                    hasPrevious = true;
                }
                else if (methods[static_cast<uint8_t>(HttpMethod::HEAD)])
                {
                    allowHeader += "HEAD";
                    hasPrevious = true;
                }

                if (methods[static_cast<uint8_t>(HttpMethod::POST)])
                {
                    if (hasPrevious)
                        allowHeader += ", ";
                    allowHeader += "POST";
                }

                return {StatusCode::MethodNotAllowed, "", allowHeader, !request.shouldKeepAlive()};
            }

            return methods[method_idx].value()(request);
        }

        std::unordered_map<std::string, std::array<std::optional<HandlerFunc>, MAX_METHODS>, StringViewHash,
                           std::equal_to<>>
                m_routes;
        std::vector<Mount> m_mounts;
    };
} // namespace http
