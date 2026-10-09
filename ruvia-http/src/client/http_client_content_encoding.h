#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_codec.h"

#include "coding/http_content_codec_result.h"
#include "coding/http_content_coding.h"

namespace ruvia::detail {

template <typename headers_type>
[[nodiscard]] inline http_content_coding_field_result http_client_content_coding_of(
    const headers_type& headers, std::pmr::memory_resource* resource) {
    return http_content_coding_from_headers(headers, resource);
}

[[nodiscard]] inline http_content_coding_field_result http_client_response_content_coding(
    const http_client_response_head& head, std::pmr::memory_resource* resource) {
    return http_client_content_coding_of(head.headers(), resource);
}

[[nodiscard]] inline http_content_decode_result decode_http_client_response_content_encoding(
    const http_client_response_head& head, std::string_view encoded_content,
    std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    // The immutable parsed head and externally driven encoded bytes remain
    // separate. A decoded representation has different Content-Encoding and
    // Content-Length metadata, so return independently owned bytes.
    const auto parsed_coding = http_client_content_coding_of(head.headers(), resource);
    if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
        return http_content_decode_result_access::failure(http_content_decode_error::unsupported_coding);
    }
    return decode_http_content(
        parsed_coding.codings(), encoded_content,
        {.max_decoded_bytes_ = max_decoded_bytes, .resource_ = resource});
}

}  // namespace ruvia::detail
