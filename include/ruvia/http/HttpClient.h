#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {
inline constexpr std::string_view kDefaultHttpClientAlias = "default";
}  // namespace ruvia::detail

namespace ruvia {

struct HttpClientConfig {
    std::pmr::string host;
    std::uint16_t port{80};
    bool tls{false};
    std::size_t poolSizePerWorker{4};
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds requestTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
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
    std::initializer_list<FetchRequestHeader> headers;
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
