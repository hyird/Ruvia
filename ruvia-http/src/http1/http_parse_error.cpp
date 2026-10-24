#include "ruvia/http/http_protocol_error.h"

namespace ruvia {

http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept {
    switch (error) {
        case http_parse_error::header_too_large:
            return http_protocol_error(http_status::request_header_fields_too_large,
                "header_too_large", "request header is too large");
        case http_parse_error::uri_too_long:
            return http_protocol_error(
                http_status::uri_too_long, "uri_too_long", "request target is too long");
        case http_parse_error::invalid_request_line:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_line", "invalid request line");
        case http_parse_error::unsupported_http_version:
            return http_protocol_error(http_status::http_version_not_supported,
                "unsupported_http_version", "unsupported HTTP version");
        case http_parse_error::invalid_request_target:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_target", "invalid request target");
        case http_parse_error::invalid_header:
            return http_protocol_error(
                http_status::bad_request, "invalid_header", "invalid request header");
        case http_parse_error::invalid_connection:
            return http_protocol_error(
                http_status::bad_request, "invalid_connection", "invalid Connection header");
        case http_parse_error::invalid_upgrade:
            return http_protocol_error(
                http_status::bad_request, "invalid_upgrade", "invalid Upgrade header");
        case http_parse_error::missing_host:
            return http_protocol_error(
                http_status::bad_request, "missing_host", "missing Host header");
        case http_parse_error::invalid_host:
            return http_protocol_error(
                http_status::bad_request, "invalid_host", "invalid Host header");
        case http_parse_error::invalid_content_length:
            return http_protocol_error(http_status::bad_request, "invalid_content_length",
                "invalid Content-Length header");
        case http_parse_error::conflicting_content_length:
            return http_protocol_error(http_status::bad_request, "conflicting_content_length",
                "conflicting Content-Length header");
        case http_parse_error::invalid_transfer_encoding:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_encoding",
                "invalid Transfer-Encoding header");
        case http_parse_error::unsupported_transfer_encoding:
            return http_protocol_error(http_status::not_implemented,
                "unsupported_transfer_encoding", "unsupported transfer encoding");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request", "invalid HTTP request");
}

}  // namespace ruvia
