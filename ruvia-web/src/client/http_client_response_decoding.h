#pragma once

#include <cstddef>

namespace ruvia::detail {

class http_client_response_state;

// Called while publishing the final response head. Encoded response bodies
// must be collected to FIN before any bytes can be returned to the consumer.
void configure_http_client_response_decoding(http_client_response_state& state_value);

// Used by HTTP/1 responses. Uses the parser-computed body plan and
// keeps decoded output bounded by the response's configured byte limit.
void decode_http_client_response_body(http_client_response_state& state_value,
    bool content_semantics_present, std::size_t max_decoded_bytes);

}  // namespace ruvia::detail
