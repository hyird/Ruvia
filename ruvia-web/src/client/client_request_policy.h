#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_client.h"

namespace ruvia::detail {
struct http_client_config_storage;
class http_client_request_storage;
struct client_received_cookie;

struct client_prepared_request_scratch final {
    static constexpr std::size_t inline_header_capacity = 16;
    static constexpr std::size_t inline_cookie_capacity = 256;

    explicit client_prepared_request_scratch(std::pmr::memory_resource* resource)
        : overflow_headers_(resource),
          overflow_cookie_header_(resource) {}

    std::array<http_header_view, inline_header_capacity> inline_headers_;
    std::pmr::vector<http_header_view> overflow_headers_;
    std::array<char, inline_cookie_capacity> inline_cookie_header_;
    std::pmr::string overflow_cookie_header_;
};

// Origin policy owns cookies and automatic headers independently of connection
// negotiation, cancellation, and transport retirement.
class client_request_policy final {
public:
    client_request_policy(const http_client_config_storage& config, std::pmr::memory_resource* resource);
    // The complete view borrows the request, policy text and both scratch buffers;
    // keep their owners alive through protocol serialization.
    [[nodiscard]] http_client_request_view prepare_request(
        const http_client_request_storage& request, client_prepared_request_scratch& scratch);
    void retain_response_cookies(std::string_view request_target, std::span<const http_header> headers);

private:
    struct stored_cookie final {
        stored_cookie(std::string_view name, std::string_view value,
            std::pmr::memory_resource* resource)
            : stored_cookie(name, value, "/", {}, resource) {}
        stored_cookie(std::string_view name, std::string_view value,
            std::string_view path, std::string_view domain,
            std::pmr::memory_resource* resource)
            : name_(name, resource),
              value_(value, resource),
              path_(path, resource),
              domain_(domain, resource) {}
        std::pmr::string name_;
        std::pmr::string value_;
        std::pmr::string path_;
        std::pmr::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        // From the client configuration rather than a Set-Cookie: sent even
        // when received cookies are ignored, never replaced by a response.
        bool configured_{true};
    };
    void add_cookie(std::string_view name, std::string_view value);
    void store_received_cookie(const client_received_cookie& cookie);
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
