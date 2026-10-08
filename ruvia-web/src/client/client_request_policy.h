#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpHeader.h"

namespace ruvia {
class HttpClientResponse;
}

namespace ruvia::detail {
struct HttpClientConfigStorage;
class HttpClientRequestStorage;

// Origin policy owns cookies and automatic headers independently of connection
// negotiation, cancellation, and transport retirement.
class client_request_policy final {
public:
    client_request_policy(const HttpClientConfigStorage& config, std::pmr::memory_resource* resource);
    void append_headers(const HttpClientRequestStorage& request,
        std::pmr::vector<HttpHeaderView>& headers, std::pmr::string& cookie_header);
    void retain_response_cookies(const HttpClientRequestStorage& request, const HttpClientResponse& response);

private:
    struct stored_cookie final {
        stored_cookie(
            std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource),
              value(value, resource),
              path("/", resource),
              domain(resource) {}
        std::pmr::string name;
        std::pmr::string value;
        std::pmr::string path;
        std::pmr::string domain;
        std::optional<std::chrono::system_clock::time_point> expires;
        bool secure{false};
        bool host_only{true};
        bool persistent{true};
    };
    void add_cookie(std::string_view name, std::string_view value);
    [[nodiscard]] static std::size_t storage_bytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(std::size_t replaced_bytes,
        std::size_t replacement_bytes, bool adding) const noexcept;
    const HttpClientConfigStorage& config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<stored_cookie> cookies_;
    std::size_t cookie_bytes_{};
};
}  // namespace ruvia::detail
