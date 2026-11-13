#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

// Case-insensitive set of field/option tokens parsed from one or more
// Connection header lines (RFC 9110 section 7.6.1).
class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    void update(std::string_view connection_field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

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
    [[nodiscard]] std::span<const std::string> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return tokens_;
    }
    std::span<const std::string> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;

    [[nodiscard]] std::string rebuild_connection_field(
        bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const headers_type& headers) {
    http_connection_nominated_fields nominated;
    nominated.update_headers(headers);
    return nominated;
}

// Protocol hop-by-hop fields (RFC 9110 section 7.6.1): Connection, Keep-Alive,
// Proxy-Connection, TE, Transfer-Encoding, Trailer, Upgrade,
// Proxy-Authenticate, and Proxy-Authorization.
[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;

// Standard connection control/hop-by-hop tokens ("close", "keep-alive", and the
// protocol hop-by-hop field names).
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

}  // namespace ruvia
