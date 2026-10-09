#pragma once

#include <string_view>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

// Keep protocol implementation code on the public response contract's single
// authoritative classification.
using http_response_content_semantics_type = ::ruvia::http_response_content_semantics;

[[nodiscard]] constexpr http_response_content_semantics_type classify_http_response_content_semantics(
    http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics_type::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics_type::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics_type::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content ||
        status_code == http_status::not_modified) {
        return http_response_content_semantics_type::without_content;
    }
    return http_response_content_semantics_type::with_content;
}

[[nodiscard]] inline http_response_content_semantics_type classify_http_response_content_semantics(
    std::string_view request_method, http_status_code status_code) noexcept {
    return classify_http_response_content_semantics(classify_http_method(request_method), status_code);
}

}  // namespace ruvia::detail
