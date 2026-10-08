#pragma once

#include <cstddef>
#include <memory_resource>

namespace ruvia::detail {

class HttpClientResponseState;

// Called while publishing the final response head. Encoded response bodies
// must be collected to FIN before any bytes can be returned to the consumer.
void configureHttpClientResponseDecoding(HttpClientResponseState& state);

// Shared by HTTP/1, HTTP/2 and HTTP/3. Uses the parser-computed body plan and
// keeps decoded output bounded by the response's configured byte limit.
void decodeHttpClientResponseContentEncoding(HttpClientResponseState& state,
    bool contentSemanticsPresent, std::size_t maxDecodedBytes);

}  // namespace ruvia::detail
