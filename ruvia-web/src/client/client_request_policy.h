#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_header.h"

namespace ruvia::detail {
struct http_client_config_storage;
class http_client_request_storage;

// Origin policy owns cookies and automatic headers independently of connection
// negotiation, cancellation, and transport retirement.
class client_request_policy final {
public:
    client_request_policy(const http_client_config_storage& config, std::pmr::memory_resource* resource);
    void append_headers(const http_client_request_storage& request,
        std::pmr::vector<http_header_view>& headers, std::pmr::string& cookie_header);
    void retain_response_cookies(const http_client_request_storage& request, std::span<const http_header> headers);

private:
    struct stored_cookie final {
        stored_cookie(
            std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name_(name, resource),
              value_(value, resource),
              path_("/", resource),
              domain_(resource) {}
        std::pmr::string name_;
        std::pmr::string value_;
        std::pmr::string path_;
        std::pmr::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool host_only_{true};
        bool persistent_{true};
    };
    void add_cookie(std::string_view name, std::string_view value);
    void reserve_cookie_slot();
    void discard_expired(std::chrono::system_clock::time_point now);
    [[nodiscard]] static std::size_t storage_bytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(std::size_t replaced_bytes,
        std::size_t replacement_bytes, bool adding) const noexcept;
    const http_client_config_storage& config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<stored_cookie> cookies_;
    std::size_t cookie_bytes_{};
};
}  // namespace ruvia::detail
