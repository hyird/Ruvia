#include "ruvia/http/http_parse_error.h"

#include "ruvia/http/http_request_body_failure.h"

namespace ruvia {

http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept {
    switch (error) {
        case http_parse_error::header_too_large:
            return http_protocol_error(
                http_status::request_header_fields_too_large, "request header is too large");
        case http_parse_error::body_too_large:
            return http_request_body_failure::too_large().protocol_error();
        case http_parse_error::invalid_request_line:
            return http_protocol_error(http_status::bad_request, "invalid request line");
        case http_parse_error::unsupported_http_version:
            return http_protocol_error(
                http_status::http_version_not_supported, "unsupported HTTP version");
        case http_parse_error::invalid_request_target:
            return http_protocol_error(http_status::bad_request, "invalid request target");
        case http_parse_error::invalid_header:
            return http_protocol_error(http_status::bad_request, "invalid request header");
        case http_parse_error::invalid_connection:
            return http_protocol_error(http_status::bad_request, "invalid Connection header");
        case http_parse_error::invalid_upgrade:
            return http_protocol_error(http_status::bad_request, "invalid Upgrade header");
        case http_parse_error::too_many_headers:
            return http_protocol_error(
                http_status::request_header_fields_too_large, "too many request headers");
        case http_parse_error::missing_host:
            return http_protocol_error(http_status::bad_request, "missing Host header");
        case http_parse_error::invalid_host:
            return http_protocol_error(http_status::bad_request, "invalid Host header");
        case http_parse_error::invalid_content_length:
        case http_parse_error::conflicting_content_length:
            return http_protocol_error(http_status::bad_request, "invalid Content-Length header");
        case http_parse_error::invalid_transfer_encoding:
            return http_protocol_error(http_status::bad_request, "invalid Transfer-Encoding header");
        case http_parse_error::unsupported_transfer_encoding:
            return http_protocol_error(http_status::not_implemented, "unsupported transfer encoding");
        case http_parse_error::invalid_chunk_size:
            return http_protocol_error(http_status::bad_request, "invalid chunk size");
        case http_parse_error::chunk_size_overflow:
            return http_protocol_error(http_status::bad_request, "chunk size is too large");
        case http_parse_error::invalid_chunk_extension:
            return http_protocol_error(http_status::bad_request, "invalid chunk extension");
        case http_parse_error::invalid_chunk_crlf:
            return http_protocol_error(http_status::bad_request, "invalid chunk delimiter");
        case http_parse_error::invalid_trailer:
            return http_protocol_error(http_status::bad_request, "invalid chunk trailer");
    }
    return http_protocol_error(http_status::bad_request, "invalid HTTP request");
}

}  // namespace ruvia
