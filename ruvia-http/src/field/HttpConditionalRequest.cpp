#include "ruvia/http/detail/field/HttpConditionalRequest.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

HttpEtagPreconditions httpEtagPreconditions(
    const HttpRequest& request, std::string_view etag) noexcept {
    HttpEtagPreconditions result;
    const bool hasIfMatch = requestHasKnownHeader(request, RequestKnownHeader::kIfMatch);
    const bool hasIfNoneMatch = requestHasKnownHeader(request, RequestKnownHeader::kIfNoneMatch);
    if (!hasIfMatch && !hasIfNoneMatch) {
        return result;
    }

    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them in one header scan and
    // retain whole-list validity without allocating a joined string.
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = HttpRequestAccess::headerKind(request, i);
        if (hasIfMatch && kind == std::to_underlying(RequestHeaderKind::kIfMatch)) {
            result.ifMatch.update(headers[i].value(), etag, true);
        } else if (hasIfNoneMatch &&
                   kind == std::to_underlying(RequestHeaderKind::kIfNoneMatch)) {
            result.ifNoneMatch.update(headers[i].value(), etag, false);
        }
    }
    return result;
}

}  // namespace ruvia::detail
