#include "ruvia/http/http_accept_match.h"

#include "field/http_accept_media_type.h"
#include "field/http_accept_token.h"

namespace ruvia {

void http_accept_match::update_media_type(std::string_view field, std::string_view offered) noexcept {
    detail::http_accumulate_media_type_acceptance(field, offered, specificity_, quality_);
}

void http_accept_match::update_token(std::string_view field, std::string_view offered,
    http_accept_token_match_mode mode) noexcept {
    detail::http_accumulate_token_acceptance(
        field, offered, mode == http_accept_token_match_mode::language_prefix, specificity_, quality_);
}

}  // namespace ruvia
