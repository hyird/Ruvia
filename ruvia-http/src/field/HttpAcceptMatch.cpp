#include "ruvia/http/HttpAcceptMatch.h"

#include "ruvia/http/detail/field/HttpAcceptMediaType.h"
#include "ruvia/http/detail/field/HttpAcceptToken.h"

namespace ruvia {

void HttpAcceptMatch::updateMediaType(std::string_view field, std::string_view offered) noexcept {
    detail::httpAccumulateMediaTypeAcceptance(field, offered, specificity_, quality_);
}

void HttpAcceptMatch::updateToken(std::string_view field, std::string_view offered,
    HttpAcceptTokenMatchMode mode) noexcept {
    detail::httpAccumulateTokenAcceptance(
        field, offered, mode == HttpAcceptTokenMatchMode::kLanguagePrefix, specificity_, quality_);
}

}  // namespace ruvia
