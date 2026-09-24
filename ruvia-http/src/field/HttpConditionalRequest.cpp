#include "ruvia/http/HttpConditionalRequest.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HttpEntityTag.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/util/HttpOws.h"

namespace ruvia {

HttpConditionalHeaders httpConditionalHeaders(const HttpRequest& request) noexcept {
    return HttpConditionalHeaders{
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfUnmodifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfModifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kRange),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfRange),
        detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kIfRange)};
}

void HttpEtagFieldCondition::update(
    std::string_view value, std::string_view expected, bool strong) noexcept {
    present = true;
    ++lineCount;
    const auto trimmed = detail::httpTrimOws(value);
    if (trimmed == "*") {
        wildcard = true;
        if (lineCount != 1) {
            valid = false;
        }
        return;
    }
    if (wildcard) {
        valid = false;
    }
    const auto result = detail::httpParseEtagListMatches(value, expected, strong);
    valid = valid && result.valid;
    matched = matched || result.matched;
}

HttpEtagPreconditions httpEtagPreconditions(
    const HttpRequest& request, std::string_view etag) noexcept {
    HttpEtagPreconditions result;
    const bool hasIfMatch = detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kIfMatch);
    const bool hasIfNoneMatch =
        detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kIfNoneMatch);
    if (!hasIfMatch && !hasIfNoneMatch) {
        return result;
    }

    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them in one header scan and
    // retain whole-list validity without allocating a joined string.
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::HttpRequestAccess::headerKind(request, i);
        if (hasIfMatch && kind == std::to_underlying(detail::RequestHeaderKind::kIfMatch)) {
            result.ifMatch.update(headers[i].value(), etag, true);
        } else if (hasIfNoneMatch &&
                   kind == std::to_underlying(detail::RequestHeaderKind::kIfNoneMatch)) {
            result.ifNoneMatch.update(headers[i].value(), etag, false);
        }
    }
    return result;
}

bool httpDateNotModified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = detail::httpParseHttpDate(detail::httpTrimOws(header));
    return date.has_value() && modifiedSeconds <= *date;
}

bool httpDateUnmodified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = detail::httpParseHttpDate(detail::httpTrimOws(header));
    return !date.has_value() || modifiedSeconds <= *date;
}

bool httpIfRangeAllows(std::string_view header, std::string_view etag,
    std::time_t modifiedSeconds, bool dateValidatorStrong) noexcept {
    if (header.empty()) {
        return false;
    }
    const auto value = detail::httpTrimOws(header);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return detail::httpStrongEtagEquals(value, etag);
    }
    if (!dateValidatorStrong) {
        return false;
    }
    const auto date = detail::httpParseHttpDate(value);
    return date.has_value() && modifiedSeconds == *date;
}

}  // namespace ruvia
