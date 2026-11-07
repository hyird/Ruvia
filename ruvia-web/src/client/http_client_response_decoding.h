#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"

namespace ruvia {
class operation_timeout;
}  // namespace ruvia

namespace ruvia::detail {

class client_request_policy;
class http_client_response_state;

// One received final response head, after its protocol driver stored the
// response fields in the state's headers_.
struct http_client_final_head final {
    http_received_status status_;
    http_protocol_version protocol_version_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    // Target of a request addressed to the fixed origin, whose jar retains the
    // head's Set-Cookie fields; nullopt for a tunnel to another authority.
    std::optional<std::string_view> origin_target_;
    // The protocol's verdict that this head opens the request's tunnel: a 2xx
    // to CONNECT, or the HTTP/1 101 upgrade of CONNECT-UDP (RFC 9298 section 3).
    bool opens_tunnel_{false};
};

// The final-head publication shared by HTTP/1 and HTTP/2: stops an upload the
// response completed, records status, version and body plan, retains origin
// cookies, settles tunnel acceptance (rejecting an invalid CONNECT-UDP head),
// selects collection of an encoded body, then wakes the head consumer.
void publish_http_client_final_head(
    http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head);

// Ends a received body: decodes a collected Content-Encoding within the
// response byte limit, failing with timeout when `timeout` expired before or
// during decoding.
void finish_http_client_response_body(http_client_response_state& state_value, const operation_timeout& timeout,
    bool content_semantics_present, std::size_t max_decoded_bytes);

}  // namespace ruvia::detail
