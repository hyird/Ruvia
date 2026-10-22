#include "ruvia/http/http_accept_match.h"

#include "field/http_accept_media_type.h"
#include "field/http_accept_token.h"
#include "field/http_media_type.h"

namespace ruvia {

std::string_view http_media_type_only(borrowed_text value) noexcept {
    return detail::http_media_type_only(value.view());
}

bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    return detail::is_valid_http_content_type_field_value(value);
}

void http_accept_match::update_media_type(std::string_view field, std::string_view offered) noexcept {
    detail::http_accumulate_media_type_acceptance(field, offered, specificity_, quality_);
}

void http_accept_match::update_token(std::string_view field, std::string_view offered,
    http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(
        field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
}

}  // namespace ruvia
