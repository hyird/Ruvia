#pragma once

#include "http2/http2_field_list.h"
#include "response/http_binary_response_fields.h"

namespace ruvia::detail {

inline void append_http2_response_trailers(
    http2_field_list& fields, const http_response_trailer_section& section) {
    fields.clear();
    fields.reserve(section.fields().size());
    fields.add_lowercase_fields(section.fields());
}

// The HPACK decode callback for a client-role response trailer section, the
// counterpart of http2_on_decoded_request_trailer. Defined by the connection's
// response-header decoding, which owns what a decoded response may carry.
[[nodiscard]] bool http2_on_decoded_response_trailer(
    void* target, std::string_view name, std::string_view value);

}  // namespace ruvia::detail
