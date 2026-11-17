#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
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
            routes[std::string(path)][static_cast<uint8_t>(method)] = std::move(handler);
        }

        [[nodiscard]] Response route(const Request &request) const
        {
            // ignore the query string for routing purposes
            auto query_pos = request.target.find('?');
            auto it = routes.find(query_pos == std::string_view::npos ? request.target
                                                                      : request.target.substr(0, query_pos));
            if (it == routes.end())
            {
                return {StatusCode::NotFound, "", "", !request.shouldKeepAlive()}; // 404 Not Found
            }

            const auto &methods = it->second;
            const auto method_idx = static_cast<uint8_t>(request.method);

            if (method_idx >= MAX_METHODS || !methods[method_idx])
            {
                // Allow HEAD requests to be handled by the GET handler if it exists
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
                    {
                        allowHeader += ", ";
                    }
                    allowHeader += "POST";
                }

                // RFC requirement: If the target exists but the method is unsupported, return 405
                return {StatusCode::MethodNotAllowed, "", allowHeader, !request.shouldKeepAlive()};
            }

            // Execute the handler
            return methods[method_idx].value()(request);
        }

    private:
        // Fast path lookup, array index for method lookup
        std::unordered_map<std::string, std::array<std::optional<HandlerFunc>, MAX_METHODS>, StringViewHash,
                           std::equal_to<>>
                routes; // Enable heterogeneous lookup with std::string_view
    };
} // namespace http
