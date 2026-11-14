#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_message.h"

namespace ruvia {

class http_request;

// Normalizes an incoming request target for proxy forwarding: converts
// absolute-form targets to origin-form (with "/" when the path is empty and
// preserving any query), converts an OPTIONS target with an empty path and no
// query to "*", and preserves all other forms verbatim.
[[nodiscard]] std::string normalize_http_proxy_request_target(std::string_view method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(http_known_method method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(const http_request& request);

enum class http_max_forwards_status : std::uint8_t {
    absent,
    valid,
    invalid,
};

// Parsed Max-Forwards hop count (RFC 9110 section 7.6.2).
class http_max_forwards_result final {
public:
    [[nodiscard]] static constexpr http_max_forwards_result absent() noexcept {
        return http_max_forwards_result(http_max_forwards_status::absent, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result invalid() noexcept {
        return http_max_forwards_result(http_max_forwards_status::invalid, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result valid(std::uint64_t current) noexcept {
        return http_max_forwards_result(http_max_forwards_status::valid, current);
    }

    [[nodiscard]] constexpr http_max_forwards_status status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool is_absent() const noexcept {
        return status_ == http_max_forwards_status::absent;
    }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return status_ == http_max_forwards_status::valid;
    }
    [[nodiscard]] constexpr bool is_invalid() const noexcept {
        return status_ == http_max_forwards_status::invalid;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> current() const noexcept {
        return is_valid() ? std::optional<std::uint64_t>(current_) : std::nullopt;
    }
    [[nodiscard]] constexpr bool exhausted() const noexcept {
        return is_valid() && current_ == 0;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> decremented() const noexcept {
        return (is_valid() && current_ > 0) ? std::optional<std::uint64_t>(current_ - 1) : std::nullopt;
    }
    [[nodiscard]] std::string format_decremented() const;

private:
    constexpr http_max_forwards_result(http_max_forwards_status status, std::uint64_t current) noexcept :
        current_(current),
        status_(status) {}

    std::uint64_t current_{0};
    http_max_forwards_status status_{http_max_forwards_status::absent};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_max_forwards_value(std::string_view field_value, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(std::string_view field_value, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(std::span<const http_header> headers, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(std::span<const http_header_view> headers, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(const http_request& request, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;

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

    [[nodiscard]] std::string rebuild_connection_field(bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(std::string_view connection_field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(const headers_type& headers) {
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

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name, const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name, const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

}  // namespace ruvia
