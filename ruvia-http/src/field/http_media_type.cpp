#include "ruvia/http/http_media_type.h"

#include "field/http_media_type.h"

namespace ruvia {

std::string_view http_media_type_only(borrowed_text value) noexcept {
    return detail::http_media_type_only(value.view());
}

bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    return detail::is_valid_http_content_type_field_value(value);
}

}  // namespace ruvia
