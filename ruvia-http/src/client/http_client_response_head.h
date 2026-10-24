#pragma once

#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_exchange_state.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_length.h"
#include "coding/http_transfer_encoding.h"
#include "parser/http_header_descriptor_storage.h"
// The two steps between an HTTP/1 client response head on the wire and the plan
// the parser acts on: reading the status line and header fields into borrowed
// values, then deciding from them (and the request that produced them) how the
// body is framed and whether the connection survives.

namespace ruvia::detail {

struct http1_client_parsed_status_line final {
    http_status_code status_code_;
    http_protocol_version protocol_version_;
};

struct http1_client_parsed_response_head final {
    explicit http1_client_parsed_response_head(const http1_client_parsed_status_line& status_line,
        std::pmr::memory_resource* resource) noexcept;

    http_status_code status_code_;
    http_protocol_version protocol_version_;
    bool content_length_field_present_{false};
    bool saw_transfer_encoding_{false};
    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
    http_content_length_state content_length_;
    http_transfer_encoding_state transfer_encoding_;

    http_header_descriptor_storage<http_header_view> headers_;
};

using http1_client_status_line_parse_result_type =
    std::variant<http1_client_parsed_status_line, http1_client_response_parse_error>;
using http1_client_response_head_parse_result_type =
    std::variant<http1_client_parsed_response_head, http1_client_response_parse_error>;
using http1_client_response_planning_result_type =
    std::variant<http1_client_response_plan, http1_client_response_parse_error>;

// Parse the status line and header fields of one complete head section.
[[nodiscard]] http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    std::string_view head_section, const http1_client_exchange_state& exchange_state,
    std::pmr::memory_resource* resource);

// Decide the response plan a parsed head implies for this request.
[[nodiscard]] http1_client_response_planning_result_type plan_http1_client_response(
    const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase);

}  // namespace ruvia::detail
