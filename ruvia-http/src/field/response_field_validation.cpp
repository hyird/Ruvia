#include "field/response_field_validation.h"

#include "ruvia/http/detail/server/http_response_trailers.h"

#include "coding/http_content_coding.h"
#include "field/http_media_type.h"

namespace ruvia::detail {

response_field_status validate_response_field(request_header_kind kind, std::string_view value,
    http_field_list_role role, response_field_state& state) noexcept {
    switch (kind) {
        case request_header_kind::content_type:
            if (role == http_field_list_role::sender) {
                if (!is_valid_http_content_type_field_value(value)) {
                    return response_field_status::invalid;
                }
                if (state.content_type_seen_) {
                    return response_field_status::repeated_singleton;
                }
                state.content_type_seen_ = true;
            }
            break;
        case request_header_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, role)) {
                return response_field_status::invalid;
            }
            break;
        case request_header_kind::trailer:
            if (role == http_field_list_role::sender &&
                !is_valid_http_response_trailer_field_value(value, role)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail
