#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"

namespace ruvia {

// HTTP/1 chunk framing with response trailer semantics fixed at construction.
// Results use the same typed contract as the role-neutral framing core. Consume
// borrowed body/trailer views before mutating or discarding their input prefix.
class http_response_chunked_body_decoder final {
public:
    explicit http_response_chunked_body_decoder(protocol_byte_limit body_limit_value)
        : decoder_({.body_limit_ = body_limit_value, .trailer_role_ = http1_chunk_trailer_role::response}) {}

    [[nodiscard]] http1_chunk_decode_result decode(std::string_view input) {
        return decoder_.decode(input);
    }

    [[nodiscard]] http1_chunk_decode_result decode(std::string_view input, std::size_t max_body_bytes) {
        return decoder_.decode(input, max_body_bytes);
    }

    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&) = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&, std::size_t) = delete;

private:
    http1_chunked_body_decoder decoder_;
};

// Visit normalized response trailer fields from a validated HTTP/1 trailer block.
template <typename visitor_type>
[[nodiscard]] inline bool visit_http_response_trailers(std::string_view block, visitor_type&& visitor) {
    return detail::visit_http_response_trailer_fields(block, std::forward<visitor_type>(visitor));
}

}  // namespace ruvia
