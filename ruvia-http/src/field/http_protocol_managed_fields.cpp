#include "ruvia/http/http_protocol_managed_fields.h"

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"

namespace ruvia {

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    switch (detail::classify_request_header(name)) {
        case detail::request_header_kind::host:
        case detail::request_header_kind::content_length:
        case detail::request_header_kind::transfer_encoding:
        case detail::request_header_kind::trailer:
        case detail::request_header_kind::expect:
        case detail::request_header_kind::connection:
        case detail::request_header_kind::te:
        case detail::request_header_kind::upgrade:
            return true;
        case detail::request_header_kind::other:
            // Keep-Alive and Proxy-Connection have no request identity.
            return detail::http_field_name_in(name, detail::http_connection_specific_fields);
        default:
            return false;
    }
}

}  // namespace ruvia
