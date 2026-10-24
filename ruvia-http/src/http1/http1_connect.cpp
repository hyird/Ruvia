#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_server_semantics.h"

namespace ruvia {
http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) ||
        !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        throw http_protocol_error(http_status::internal_server_error,
            "invalid_connect_response_head", "invalid HTTP/1 CONNECT response head");
    }
    for (const auto& header : response.headers()) {
        if (detail::http_field_name_in(header.name(), detail::http_message_framing_fields)) {
            throw http_protocol_error(http_status::internal_server_error,
                "connect_response_framing_fields",
                "CONNECT response cannot contain message framing fields");
        }
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10
                                ? plan_http10_request_connection(true, false)
                                : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}
}  // namespace ruvia
