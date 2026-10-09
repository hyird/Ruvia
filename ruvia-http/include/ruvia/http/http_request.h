#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/attributes.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/request/http_request_header_block.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_protocol_version.h"

namespace ruvia {

enum class http_request_target_form : std::uint8_t {
    origin,
    absolute,
    authority,
    asterisk,
    // HTTP/2 and HTTP/3 carry target components as pseudo-fields rather than
    // one HTTP/1 request-target token.
    http2,
    http3,
};

class http_request;
class http3_server_request;

namespace detail {

struct http_request_access;

}  // namespace detail

class http_request final {
public:
    // Owns the compact descriptor block; field text still borrows protocol input.
    // The block's PMR resource must outlive the request. Moving transfers it.
    http_request(const http_request&) = delete;
    http_request& operator=(const http_request&) = delete;
    http_request(http_request&&) noexcept = default;
    http_request& operator=(http_request&&) noexcept = default;

    [[nodiscard]] std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] http_known_method known_method() const noexcept {
        return known_method_;
    }

    // Releases owned header descriptors and clears all borrowed message views.
    // Useful when a runtime reuses a request object before its input storage or
    // PMR resource is retired.
    void reset() noexcept {
        headers_ = {};
        method_ = {};
        known_method_ = http_known_method::unknown;
        target_ = {};
        scheme_ = {};
        authority_ = {};
        path_ = {};
        query_string_ = {};
        protocol_version_ = http_protocol_version::http11;
        target_form_ = http_request_target_form::origin;
        cached_headers_.fill(0);
        body_ = {};
    }

    [[nodiscard]] std::string_view target() const noexcept {
        return target_;
    }

    [[nodiscard]] std::string_view scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] std::string_view authority() const noexcept {
        return authority_;
    }

    [[nodiscard]] http_request_target_form target_form() const noexcept {
        return target_form_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view query_string() const noexcept {
        return query_string_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    // Semantic regular fields, not a byte-for-byte copy of the wire header
    // block. HTTP/1 preserves accepted field-name spelling but removes field
    // framing/OWS and replaces a conflicting Host value for absolute- or
    // authority-form targets. HTTP/2 names are lowercase, pseudo-fields are
    // exposed through method()/scheme()/authority()/target(), split Cookie
    // fields are coalesced, and Host is synthesized from a valid :authority
    // when absent. Repeated regular fields retain wire order.
    [[nodiscard]] std::span<const http_header_view> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_.fields();
    }
    [[nodiscard]] std::span<const http_header_view> headers() const&& = delete;

    [[nodiscard]] std::span<const std::byte> body_bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    [[nodiscard]] std::span<const std::byte> body_bytes() const&& = delete;

    // Case-insensitive semantic lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
    // Exact raw lookup over the encoded query string. Both `raw_name` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(
        std::string_view raw_name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;

private:
    friend struct detail::http_request_access;
    friend class http3_server_request;

    static constexpr std::size_t cached_header_slots = detail::request_header_kind_count - 1;

    http_request() noexcept = default;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    std::string_view method_;
    http_known_method known_method_{http_known_method::unknown};
    std::string_view target_;
    std::string_view scheme_;
    std::string_view authority_;
    std::string_view path_;
    std::string_view query_string_;
    http_protocol_version protocol_version_{http_protocol_version::http11};
    http_request_target_form target_form_{http_request_target_form::origin};
    detail::http_request_header_block headers_{};
    // One-based indices; zero means absent. No duplicated string views.
    std::array<std::uint8_t, cached_header_slots> cached_headers_{};
    std::span<const std::byte> body_{};
    std::pmr::memory_resource* resource_{nullptr};
};

// Header descriptors live in the caller-selected PMR resource rather than the
// request's coroutine frame. The protocol field-count limit is independent of
// this object's layout.

// Constructs a semantic request directly, without parsing a wire-format head.
// Header names/values and body bytes are borrowed from the caller; the exact
// header span count is used to allocate descriptors from `resource`. The caller
// must keep all supplied text/body storage and the resource alive while the
// returned request is in use. Invalid method, target, or header fields are
// reported as http_parse_error; body framing fields are intentionally not
// interpreted because this API represents an already-parsed request.
[[nodiscard]] std::pair<http_request, std::optional<http_parse_error>> make_parsed_http_request(
    std::string_view method, std::string_view target, std::span<const http_header_view> headers,
    std::span<const std::byte> body, std::pmr::memory_resource* resource);

}  // namespace ruvia
