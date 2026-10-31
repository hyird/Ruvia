#include "ruvia/http/http_protocol_managed_fields.h"

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_field_syntax.h"

#include "field/http_request_field_validation.h"

namespace ruvia {

namespace {

[[nodiscard]] bool is_protocol_managed_request_field(detail::request_header_kind kind, std::string_view name) noexcept {
    switch (kind) {
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

}  // namespace

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    return is_protocol_managed_request_field(detail::classify_request_header(name), name);
}

bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    const auto kind = detail::classify_request_header(name);
    if (is_protocol_managed_request_field(kind, name)) {
        return false;
    }
    // TE 是唯一按 HTTP 版本区分的请求字段语义，已由托管字段边界排除。
    // 每次只检查一个字段；重复检测由调用方持有的字段集合负责。
    detail::request_field_state state(detail::request_field_framing::http1);
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) ==
           detail::request_field_status::ok;
}

}  // namespace ruvia
