#include "ruvia/http/http1_connect.h"

#include "ruvia/http/http_ascii.h"

namespace ruvia {
std::variant<http1_response_head_plan, http_protocol_error> prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) noexcept {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) ||
        !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        return http_protocol_error(http_status::internal_server_error, "invalid HTTP/1 CONNECT response head");
    }
    for (const auto& header : response.headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "content-length") || http_ascii_equals_ignore_case(header.name(), "transfer-encoding")) {
            return http_protocol_error(http_status::internal_server_error, "CONNECT response cannot contain message framing fields");
        }
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10
                                ? plan_http10_request_connection(true, false)
                                : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}
}  // namespace ruvia
