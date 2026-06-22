#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
inline constexpr std::string_view kDefaultHttpClientAlias = "default";
}  // namespace ruvia::detail

namespace ruvia {

struct HttpClientConfig {
    // Host name or unbracketed address only; keep the port in port.
    std::pmr::string host;
    // Must be non-zero.
    std::uint16_t port{80};
    bool tls{false};
    // Must be greater than zero.
    std::size_t poolSizePerWorker{4};
    // Set to 0 to disable the corresponding timeout.
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds requestTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
    // Set to 0 to disable the response body limit.
    std::size_t maxResponseBodyBytes{kDefaultMaxBufferedBodyBytes};
};

struct FetchRequestHeader {
    std::string_view name;   // borrowed; must remain valid through co_await
    std::string_view value;  // borrowed; must remain valid through co_await
};

struct FetchResponseHeader {
    std::pmr::string name;
    std::pmr::string value;
    FetchResponseHeader() = default;
    FetchResponseHeader(std::pmr::string n, std::pmr::string v)
        : name(std::move(n)), value(std::move(v)) {}
    FetchResponseHeader(std::string_view n, std::string_view v, std::pmr::memory_resource* resource)
        : name(n.data(), n.size(), resource), value(v.data(), v.size(), resource) {}
};

struct FetchOptions {
    std::string_view method{"GET"};
    // Borrowed header table; elements and pointed-to strings must remain valid through co_await.
    std::span<const FetchRequestHeader> headers{};
    std::string_view body{};  // borrowed; must remain valid through co_await
    std::chrono::milliseconds timeout{0};
};

class FetchResponse final {
public:
    explicit FetchResponse(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : headers(resource), body(resource) {}

    FetchResponse(const FetchResponse&) = delete;
    FetchResponse& operator=(const FetchResponse&) = delete;
    FetchResponse(FetchResponse&&) noexcept = default;
    FetchResponse& operator=(FetchResponse&&) noexcept = default;

    int statusCode{0};
    std::pmr::vector<FetchResponseHeader> headers;
    std::pmr::string body;
};

namespace detail {

struct HttpClientDefinition final {
    std::pmr::string alias;
    HttpClientConfig config;
};

}  // namespace detail

}  // namespace ruvia

#endif  // RUVIA_ENABLE_HTTP_CLIENT
