#pragma once

#include <cstdint>
#include <optional>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_request_content_plan.h"

namespace ruvia {

// Transport-independent sender policy for RFC 9110's only standardized
// request expectation. Every writer owns generation of the Expect field so
// callers cannot create a wire/state-machine mismatch.
enum class http_client_request_expectation : std::uint8_t {
    none,
    continue_,
};

// RFC 9110 section 2.2 distinguishes forwarding received fields from generating
// them. Forwarded Content-Type, Origin and CORS request values are preserved;
// Content-Encoding and HTTP/1 Upgrade tolerate legal empty list members.
// This never relaxes field-byte safety, singleton counts or writer ownership.
// The forwarding driver still removes/rebuilds hop-by-hop fields (section 7.6.1).
// OPTIONS Content-Type is required for generated content only (section 9.3.7).
enum class http_request_field_source : std::uint8_t { generated,
    forwarded };

// What a request head asks for, derived from its method and protocol alone.
enum class http_request_head_kind : std::uint8_t {
    // Any method but CONNECT, without a protocol.
    regular,
    // CONNECT without a protocol: a tunnel to authority_ (RFC 9110 section
    // 9.3.6). It has no scheme and no target.
    connect,
    // CONNECT with a protocol: Extended CONNECT (RFC 8441, RFC 9220), carried
    // by HTTP/2 and HTTP/3 only.
    extended_connect,
    // GET with a protocol: the HTTP/1.1 Upgrade request for that protocol.
    // The HTTP/1 writer generates it for connect-udp (RFC 9298 section 3.2).
    protocol_upgrade,
};

// The one borrowed description of a request head to send: consumed by
// prepare_http1_client_request(), http2_connection::submit_request_head(),
// http2_connection::submit_push_request() and
// http3_connection::submit_request_head(), and by the CONNECT-UDP validators.
// String literals, string_view values and owning-string lvalues are valid
// inputs; owning-string temporaries are rejected before they can leave a
// dangling view. Every view must remain alive and unchanged until the
// synchronous preparation or submission that consumes it returns.
// A consumer reads the fields its protocol and the head's kind() define:
// - HTTP/1 takes the scheme and authority of a regular or protocol_upgrade
//   request from its http_origin_view argument; scheme_ and authority_ are
//   read by HTTP/2 and HTTP/3. A connect head names its tunnel destination in
//   authority_ for every version.
// - content_ and expectation_ belong to regular requests; the other kinds
//   carry no request content.
struct http_request_head_view final {
    borrowed_text method_{"GET"};
    // Extended CONNECT :protocol, or the HTTP/1.1 Upgrade token.
    std::optional<borrowed_text> protocol_{};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    // origin-form (or asterisk-form) request target, the :path of HTTP/2 and
    // HTTP/3.
    borrowed_text target_{"/"};
    // Borrowed header table; the HTTP/1 writer permits it to overlap its
    // emitted head.
    borrowed_http_headers headers_{};
    // none() sends no content framing; bytes() borrows the content, which must
    // remain alive and unchanged until the runtime sends it. Streaming and
    // known-length-without-bytes plans name content the runtime writes later.
    http_request_content_plan content_{http_request_content_plan::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};

    // nullopt: a protocol on a method other than CONNECT or GET describes no
    // request; every consumer rejects it.
    [[nodiscard]] constexpr std::optional<http_request_head_kind> kind() const noexcept {
        const bool connect = method_ == "CONNECT";
        if (!protocol_.has_value()) {
            return connect ? http_request_head_kind::connect : http_request_head_kind::regular;
        }
        if (connect) {
            return http_request_head_kind::extended_connect;
        }
        if (method_ == "GET") {
            return http_request_head_kind::protocol_upgrade;
        }
        return std::nullopt;
    }
};

}  // namespace ruvia
