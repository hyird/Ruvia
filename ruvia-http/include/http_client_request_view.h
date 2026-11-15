#pragma once

#include <cstdint>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_request.h"

namespace ruvia {

// Transport-independent sender policy for RFC 9110's only standardized
// request expectation. HTTP/1 and HTTP/2 writers both own generation of the
// Expect field so callers cannot create a wire/state-machine mismatch.
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

struct http_client_request_view {
    // Zero-cost field wrapper for request text retained by the sans-I/O
    // request/response transaction. String literals, string_view values, and
    // owning-string lvalues remain valid inputs; owning-string temporaries are
    // rejected before they can leave a dangling view in the request.
    ::ruvia::borrowed_text method_{"GET"};
    ::ruvia::borrowed_text target_{"/"};
    // Borrowed header table; its elements and strings must remain alive and
    // unchanged until synchronous preparation/submission consumes them. The
    // HTTP/1 writer permits metadata to overlap its emitted head and owns the
    // small set of facts needed by the later response exchange.
    borrowed_http_headers headers_{};
    // none() sends no content framing; bytes() borrows the content, which must
    // remain alive and unchanged until the runtime sends it. Streaming and
    // known-length-without-bytes plans name content the runtime writes later.
    http_request_content_plan content_{http_request_content_plan::none()};
};

// Borrowed only until open_tunnel() returns its cold operation. Normal CONNECT
// uses authority-form and leaves protocol/target empty. Extended CONNECT uses
// protocol, authority and an origin-form target on HTTP/2.
struct http_client_tunnel_request_view final {
    ::ruvia::borrowed_text authority_{};
    ::ruvia::borrowed_text protocol_{};
    ::ruvia::borrowed_text target_{};
    borrowed_http_headers headers_{};
};

}  // namespace ruvia
