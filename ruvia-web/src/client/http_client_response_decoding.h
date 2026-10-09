#pragma once

#include <cstddef>
#include <memory_resource>

namespace ruvia::detail {

class http_client_response_state;

// Called while publishing the final response head. Encoded response bodies
// must be collected to FIN before any bytes can be returned to the consumer.
void configure_http_client_response_decoding(http_client_response_state& state_value);

// Shared by HTTP/1, HTTP/2 and HTTP/3. Uses the parser-computed body plan and
// keeps decoded output bounded by the response's configured byte limit.
void decode_http_client_response_content_encoding(http_client_response_state& state_value,
    bool content_semantics_present, std::size_t max_decoded_bytes);

}  // namespace ruvia::detail
