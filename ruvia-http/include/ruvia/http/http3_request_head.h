#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_trailers.h"

namespace ruvia {

// A validated HTTP/3 request head (RFC 9114 §4.3.1). nghttp3 has already
// rejected the framing-level violations of §4.1.2 and §4.3.1 -- uppercase
// names, misplaced, repeated or missing pseudo-header fields, connection-
// specific fields, TE other than "trailers", malformed Content-Length -- with
// H3_MESSAGE_ERROR. parse() adds the request semantics every HTTP version
// shares in ruvia-http (target grammar, :authority/Host agreement, the
// recipient field rules, Expect, Cookie joining) and fails closed on anything
// it cannot represent as an http_request.
class http3_request_head final {
public:
    [[nodiscard]] static std::optional<http3_request_head> parse(
        http3_headers_event event, std::pmr::memory_resource* resource);

    http3_request_head(http3_request_head&&) noexcept = default;
    http3_request_head& operator=(http3_request_head&&) noexcept = default;
    http3_request_head(const http3_request_head&) = delete;
    http3_request_head& operator=(const http3_request_head&) = delete;

    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] std::string_view scheme() const noexcept;
    [[nodiscard]] std::string_view authority() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view protocol() const noexcept;
    // CONNECT, with or without :protocol (RFC 9114 §4.4, RFC 9220).
    [[nodiscard]] bool connect() const noexcept;
    // The HEADERS frame ended the request stream: no content follows.
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }
    [[nodiscard]] http_request_content_indication content() const noexcept {
        return stream_ended_ || declared_content_length_ == std::uint64_t{0}
                   ? http_request_content_indication::no_content
                   : http_request_content_indication::will_follow;
    }
    [[nodiscard]] std::optional<std::uint64_t> declared_content_length() const noexcept {
        return declared_content_length_;
    }
    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content(), policy);
    }

    // Views in the request borrow this head; it must outlive the request.
    [[nodiscard]] http_request build_request(
        std::pmr::memory_resource* resource, std::string_view body = {}) const;

private:
    struct regular_field final {
        std::uint32_t index_;
        detail::request_header_kind kind_;
    };

    explicit http3_request_head(http3_headers_event event, std::pmr::memory_resource* resource);
    [[nodiscard]] bool validate();
    [[nodiscard]] std::string_view field_value(std::uint32_t index) const noexcept;

    static constexpr std::uint32_t absent = 0xffffffffU;

    std::pmr::vector<http3_field> fields_;
    std::pmr::vector<regular_field> regular_;
    std::pmr::string cookie_;
    std::int64_t stream_id_{-1};
    std::uint32_t method_{absent};
    std::uint32_t scheme_{absent};
    std::uint32_t authority_{absent};
    std::uint32_t path_{absent};
    std::uint32_t protocol_{absent};
    std::uint32_t host_{absent};
    std::optional<std::uint64_t> declared_content_length_;
    http_request_expectations expectations_{};
    bool stream_ended_{false};
};

// Appends a received request trailer section. Fields not permitted in a
// trailer section are discarded (RFC 9110 §6.5.1); pseudo-header fields are
// malformed (RFC 9114 §4.3) and leave `trailers` unchanged.
[[nodiscard]] std::optional<http_request_trailer_error> append_http3_request_trailers(
    http_request_trailers& trailers, std::span<const http3_field> fields);

}  // namespace ruvia
