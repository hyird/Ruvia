#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"
#include "http_header.h"
#include "http_request.h"
#include "http_request_expectation.h"

namespace ruvia {

class http3_connection;

// A validated HTTP/3 request head (RFC 9114 §4.3.1), published by a server
// http3_connection as http3_request_head_event. Field names and values are
// packed into one contiguous byte arena (`storage_`) with integer slice
// descriptors so decoding a head requires no per-header heap allocations and
// moving the head never invalidates slices.
class http3_request_head final {
public:
    http3_request_head(http3_request_head&&) noexcept = default;
    http3_request_head& operator=(http3_request_head&&) = delete;
    http3_request_head(const http3_request_head&) = delete;
    http3_request_head& operator=(const http3_request_head&) = delete;

    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view scheme() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view scheme() const&& = delete;
    [[nodiscard]] std::string_view authority() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view authority() const&& = delete;
    [[nodiscard]] std::string_view path() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view path() const&& = delete;
    [[nodiscard]] std::string_view protocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view protocol() const&& = delete;
    // CONNECT, with or without :protocol (RFC 9114 §4.4, RFC 9220).
    [[nodiscard]] bool connect() const noexcept;
    // The HEADERS frame ended the request stream: no content follows.
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }
    [[nodiscard]] bool early_data() const noexcept {
        return early_data_;
    }
    [[nodiscard]] http_request_content_indication content() const noexcept {
        return stream_ended_ || declared_content_length_ == std::uint64_t{0} ? http_request_content_indication::no_content : http_request_content_indication::will_follow;
    }
    [[nodiscard]] std::optional<std::uint64_t> declared_content_length() const noexcept {
        return declared_content_length_;
    }
    [[nodiscard]] http_server_expectation_plan expectation_plan(http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content(), policy);
    }

    // Views in the request borrow this head; it must outlive the request.
    [[nodiscard]] http_request build_request(borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(borrowed_text body = {}) const&& = delete;

    template <typename bytes_type>
        requires(!std::convertible_to<bytes_type&&, borrowed_text> && std::convertible_to<bytes_type&&, borrowed_bytes>)
    [[nodiscard]] http_request build_request(bytes_type&& body RUVIA_LIFETIMEBOUND) const& RUVIA_LIFETIMEBOUND {
        return build_request(borrowed_text(as_chars(borrowed_bytes(std::forward<bytes_type>(body)).view())));
    }
    template <typename bytes_type>
        requires(!std::convertible_to<bytes_type&&, borrowed_text> && std::convertible_to<bytes_type&&, borrowed_bytes>)
    http_request build_request(bytes_type&&) const&& = delete;

private:
    friend class http3_connection;

    struct field_slice final {
        std::uint32_t name_offset_{0};
        std::uint32_t name_size_{0};
        std::uint32_t value_offset_{0};
        std::uint32_t value_size_{0};
    };

    struct regular_field final {
        std::uint32_t index_;
        detail::http_field_kind kind_;
    };

    http3_request_head(std::int64_t stream_id, std::string storage, std::vector<field_slice> fields, bool stream_ended, bool early_data);
    [[nodiscard]] static std::optional<http3_request_head> try_parse(std::int64_t stream_id, std::string storage, std::vector<field_slice> fields, bool stream_ended, bool early_data);
    [[nodiscard]] bool validate();
    [[nodiscard]] std::string_view field_name(std::uint32_t index) const noexcept;
    [[nodiscard]] std::string_view field_value(std::uint32_t index) const noexcept;

    static constexpr std::uint32_t absent = 0xffffffffU;

    std::string storage_;
    std::vector<field_slice> fields_;
    std::vector<regular_field> regular_;
    std::string cookie_;
    std::int64_t stream_id_{-1};
    std::uint32_t method_{absent};
    std::uint32_t scheme_{absent};
    std::uint32_t authority_{absent};
    std::uint32_t path_{absent};
    std::uint32_t protocol_{absent};
    std::uint32_t host_{absent};
    std::optional<std::uint64_t> declared_content_length_;
    std::uint32_t first_cookie_{absent};
    http_request_expectations expectations_{};
    bool stream_ended_{false};
    bool early_data_{false};
};

}  // namespace ruvia
