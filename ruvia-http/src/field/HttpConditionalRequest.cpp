#include "ruvia/http/detail/field/HttpConditionalRequest.h"

#include "ruvia/http/detail/util/AsciiCase.h"

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
    for (const auto& header : request.headers()) {
        if (hasIfMatch && httpAsciiEqualsIgnoreCase(header.name(), "If-Match")) {
            result.ifMatch.update(header.value(), etag, true);
        } else if (hasIfNoneMatch && httpAsciiEqualsIgnoreCase(header.name(), "If-None-Match")) {
            result.ifNoneMatch.update(header.value(), etag, false);
        }
    }
    return result;
}

}  // namespace ruvia::detail
