#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia {

class http3_connection;

// A validated HTTP/3 request head (RFC 9114 §4.3.1), published by a server
// http3_connection as http3_request_head_event. nghttp3 rejects the
// framing-level violations of §4.1.2 and §4.3.1 -- uppercase names,
// misplaced, repeated or missing pseudo-header fields, connection-specific
// fields, TE other than "trailers", malformed Content-Length -- and the
// connection adds the request semantics every HTTP version shares in
// ruvia-http (target grammar, :authority/Host agreement, the recipient field
// rules, Expect, Cookie joining). A head it cannot represent as an
// http_request is a malformed request the connection answers itself with
// H3_MESSAGE_ERROR (§4.1.2); no head is published for it.
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
        borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(
        borrowed_text body = {}) const&& = delete;

private:
    friend class http3_connection;
    struct regular_field final {
        std::uint32_t index_;
        detail::http_field_kind kind_;
    };

    http3_request_head(std::int64_t stream_id, std::vector<http_header> fields, bool stream_ended,
        bool early_data);
    // nullopt: the head is malformed (RFC 9114 §4.1.2).
    [[nodiscard]] static std::optional<http3_request_head> try_create(std::int64_t stream_id,
        std::vector<http_header> fields, bool stream_ended, bool early_data);
    [[nodiscard]] bool validate();
    [[nodiscard]] std::string_view field_value(std::uint32_t index) const noexcept;

    static constexpr std::uint32_t absent = 0xffffffffU;

    std::vector<http_header> fields_;
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

// RFC 9220 uses the extended CONNECT request in place of the HTTP/1 key exchange.
[[nodiscard]] websocket_handshake_validation_result validate_http3_websocket_handshake(
    const http3_request_head& head, const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept;

}  // namespace ruvia
