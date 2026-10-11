#pragma once

#include <algorithm>
#include <string_view>

#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request_content_semantics.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http3_request_payload_allowed(std::string_view method, bool content_type_present) noexcept {
    switch (http_request_content_semantics(method)) {
        case http_request_content_semantics::no_additional_requirements:
            return true;
        case http_request_content_semantics::forbidden:
            return false;
        case http_request_content_semantics::content_type_required:
            return content_type_present;
    }
    return false;
}

[[nodiscard]] inline bool http3_request_payload_allowed(const http3_message_head& head) noexcept {
    const bool content_type_present = http_request_content_semantics(head.method_) == http_request_content_semantics::content_type_required &&
                                      std::ranges::any_of(head.headers_, [](const http_header& field) {
                                          return http_ascii_equals_ignore_case(field.name(), "content-type");
                                      });
    return http3_request_payload_allowed(head.method_, content_type_present);
}

// A declared length, including zero, explicitly carries content. Without one,
// HTTP/3 has no per-HEADERS END_STREAM bit: a forbidden/missing-type payload is
// rejected by the body state only if non-empty DATA actually arrives.
[[nodiscard]] inline bool http3_request_content_metadata_valid(const http3_message_head& head) noexcept {
    // Extended CONNECT uses DATA for the selected tunnel protocol. Preserve
    // the existing receive contract for syntactically valid length metadata;
    // it never constrains tunnel bytes or creates an HTTP request body.
    if (head.method_ == "CONNECT" && !head.protocol_.empty()) {
        return true;
    }
    return !head.content_length_ || http3_request_payload_allowed(head);
}

}  // namespace ruvia::detail
