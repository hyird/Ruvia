#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_pmr_string.h"

namespace ruvia::detail {

// Prepares one HTTP/1 request head (`prepare(std::span<char>)`) into the
// uninitialized storage of `buffer`, first at its retained capacity. A larger
// head asks for exactly its required size once, up to `max_head_bytes`; the
// caller reports the remaining too-small result.
template <typename prepare_type>
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error>
prepare_http1_request_head(
    http_io_buffer& buffer, std::size_t max_head_bytes, const prepare_type& prepare) {
    auto first = prepare(buffer.prepare((std::max)(buffer.capacity(), http_retained_pmr_string_bytes)));
    const auto* too_small = std::get_if<http1_client_request_buffer_too_small>(&first);
    if (too_small == nullptr || too_small->required_head_bytes() > max_head_bytes) {
        return first;
    }
    return prepare(buffer.prepare(too_small->required_head_bytes()));
}

}  // namespace ruvia::detail
