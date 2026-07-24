#include "ruvia/edge/detail/proxy/RequestDirectives.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"

namespace ruvia::edge {

RequestDirectives requestDirectives(std::span<const HttpHeaderView> headers) noexcept {
    RequestDirectives directives;
    directives.hasAuthorization = findRequestHeader(headers, "authorization").has_value();
    directives.hasCondition = findRequestHeader(headers, "if-match").has_value() || findRequestHeader(headers, "if-none-match").has_value() || findRequestHeader(headers, "if-modified-since").has_value() || findRequestHeader(headers, "if-unmodified-since").has_value() || findRequestHeader(headers, "if-range").has_value();

    CacheControlFieldParser parser;
    bool hasCacheControl = false;
    for (const auto& field : headers) {
        if (iequals(field.name(), "cache-control")) {
            hasCacheControl = true;
            parser.update(field.value());
        }
    }
    directives.cacheControl = parser.finish();

    bool legacyPragmaNoCache = false;
    if (!hasCacheControl) {
        for (const auto& field : headers) {
            if (iequals(field.name(), "pragma") && ruvia::detail::httpHasToken(field.value(), "no-cache")) {
                legacyPragmaNoCache = true;
                break;
            }
        }
    }
    // The edge currently chooses not to calculate request-specific freshness
    // constraints. Forwarding is conservative and preserves the client's
    // preference; max-stale merely widens what the client accepts and needs no
    // forced validation.
    directives.forcesValidation = directives.cacheControl.noCache || directives.cacheControl.maxAge.has_value() || directives.cacheControl.minFresh.has_value() || legacyPragmaNoCache;
    return directives;
}

}  // namespace ruvia::edge
