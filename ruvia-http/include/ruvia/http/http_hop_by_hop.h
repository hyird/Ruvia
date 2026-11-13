#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

// Case-insensitive set of field/option tokens parsed from one or more
// Connection header lines (RFC 9110 section 7.6.1).
class http_connection_nominated_fields final {
public:
    explicit http_connection_nominated_fields(std::pmr::memory_resource* resource = nullptr)
        : tokens_(detail::http_pmr_resource_or_default(resource)) {}

    void update(std::string_view connection_field_value);
    void update_headers(std::span<const http_header> headers);
    void update_headers(std::span<const http_header_view> headers);
    void update_headers(const http_request& request);
    void update_headers(const http_response_headers& headers);
    void update_headers(const http_response& response);

    [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
    [[nodiscard]] constexpr bool has_close() const noexcept {
        return has_close_;
    }
    [[nodiscard]] constexpr bool has_keep_alive() const noexcept {
        return has_keep_alive_;
    }
    [[nodiscard]] constexpr bool has_upgrade() const noexcept {
        return has_upgrade_;
    }
    [[nodiscard]] constexpr bool has_te() const noexcept {
        return has_te_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return tokens_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return tokens_.size();
    }
    [[nodiscard]] std::span<const std::pmr::string> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return tokens_;
    }
    std::span<const std::pmr::string> tokens() const&& = delete;

    [[nodiscard]] std::pmr::vector<std::pmr::string> non_standard_options(
        std::pmr::memory_resource* resource = nullptr) const;

    [[nodiscard]] std::pmr::string rebuild_connection_field(bool retain_upgrade = false,
        bool include_non_standard = true, std::pmr::memory_resource* resource = nullptr) const;

private:
    std::pmr::vector<std::pmr::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::span<const http_header> headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::span<const http_header_view> headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_request& request, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_response_headers& headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const http_response& response, std::pmr::memory_resource* resource = nullptr);

// Protocol hop-by-hop fields (RFC 9110 section 7.6.1): Connection, Keep-Alive,
// Proxy-Connection, TE, Transfer-Encoding, Trailer, Upgrade,
// Proxy-Authenticate, and Proxy-Authorization.
[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;

// Standard connection control/hop-by-hop tokens ("close", "keep-alive", and the
// protocol hop-by-hop field names).
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_request_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_response_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

[[nodiscard]] std::pmr::vector<std::pmr::string> extract_http_non_standard_connection_options(
    std::string_view connection_field_value, std::pmr::memory_resource* resource = nullptr);

[[nodiscard]] std::pmr::string rebuild_http_connection_field(
    std::string_view connection_field_value, bool retain_upgrade = false,
    bool include_non_standard = true, std::pmr::memory_resource* resource = nullptr);

[[nodiscard]] inline std::pmr::string rebuild_http_connection_field(
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false,
    bool include_non_standard = true, std::pmr::memory_resource* resource = nullptr) {
    return nominated.rebuild_connection_field(retain_upgrade, include_non_standard, resource);
}

}  // namespace ruvia
