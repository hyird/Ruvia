#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
[[nodiscard]] std::string_view normalize_http_proxy_request_target(
    std::string_view method,
    borrowed_text target RUVIA_LIFETIMEBOUND,
    std::string& storage RUVIA_LIFETIMEBOUND);
[[nodiscard]] std::string_view normalize_http_proxy_request_target(
    http_known_method method,
    borrowed_text target RUVIA_LIFETIMEBOUND,
    std::string& storage RUVIA_LIFETIMEBOUND);
[[nodiscard]] std::string_view normalize_http_proxy_request_target(
    const http_request& request RUVIA_LIFETIMEBOUND,
    std::string& storage RUVIA_LIFETIMEBOUND);
void normalize_http_proxy_request_target(const http_request&& request, std::string& storage) = delete;
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
// One Max-Forwards field value, or the single Max-Forwards line of a header
// block: absent without one, invalid when repeated.
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(std::string_view field_value, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(const http_header_range_view& headers, std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;

enum class http_connection_fields_status : std::uint8_t { valid,
    invalid,
    budget_exceeded };

// Zero-allocation, case-insensitive set borrowing Connection field bytes.
// Inputs must remain unchanged and alive while the set is used. Failure is
// sticky; forwarding callers must reject any non-valid status.
class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    static constexpr std::size_t capacity = 128;
    static constexpr std::size_t max_input_bytes = 16384;

    void update(borrowed_text connection_field_value) noexcept;
    void update_headers(const http_header_range_view& headers) noexcept;

    template <typename source_type>
        requires http_header_field_source<source_type> && (!std::is_lvalue_reference_v<source_type&&>) && (!std::ranges::borrowed_range<source_type>)
    void update_headers(source_type&&) = delete;
    void update_headers(std::initializer_list<http_header_view>) = delete;

    [[nodiscard]] constexpr http_connection_fields_status status() const noexcept {
        return status_;
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
        return count_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] std::span<const std::string_view> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return {tokens_.data(), count_};
    }
    std::span<const std::string_view> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;

    [[nodiscard]] std::string rebuild_connection_field(bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    std::array<std::string_view, capacity> tokens_{};
    std::size_t count_{0};
    std::size_t input_bytes_{0};
    http_connection_fields_status status_{http_connection_fields_status::valid};
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(borrowed_text connection_field_value) noexcept;
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(const http_header_range_view& headers) noexcept;
template <typename source_type>
    requires http_header_field_source<source_type> && (!std::is_lvalue_reference_v<source_type&&>) && (!std::ranges::borrowed_range<source_type>)
http_connection_nominated_fields parse_http_connection_nominated_fields(source_type&&) = delete;
http_connection_nominated_fields parse_http_connection_nominated_fields(std::initializer_list<http_header_view>) = delete;

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
