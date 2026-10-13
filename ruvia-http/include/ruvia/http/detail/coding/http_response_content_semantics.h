#pragma once

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

[[nodiscard]] constexpr http_response_content_semantics classify_http_response_content_semantics(
    http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content ||
        status_code == http_status::not_modified) {
        return http_response_content_semantics::without_content;
    }
    return http_response_content_semantics::with_content;
}

}  // namespace ruvia::detail
