#include "ruvia/web/detail/http/static/FileConditionalRequest.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConditionalRequest.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HttpEntityTag.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {

void EtagFieldCondition::update(
    std::string_view value,
    std::string_view expected,
    bool strong) noexcept {
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
    const auto result = detail::httpParseEtagListMatches(
        value, expected, strong);
    valid = valid && result.valid;
    matched = matched || result.matched;
}

FileEtagConditions fileEtagConditions(
    const HttpRequest& request,
    std::string_view etag) noexcept {
    FileEtagConditions result;
    const bool hasIfMatch = detail::requestHasKnownHeader(
        request, detail::RequestKnownHeader::kIfMatch);
    const bool hasIfNoneMatch = detail::requestHasKnownHeader(
        request, detail::RequestKnownHeader::kIfNoneMatch);
    if (!hasIfMatch && !hasIfNoneMatch) {
        return result;
    }

    // Both conditions are RFC list fields. Multiple field lines are equivalent
    // to comma-joining their values (RFC 9110 §5.3), but the request keeps
    // zero-copy views into separate wire lines. Fold them in one header scan and
    // retain whole-list validity without allocating a joined string.
    for (const auto& header : request.headers()) {
        if (hasIfMatch && detail::httpAsciiEqualsIgnoreCase(
                header.name(), "If-Match")) {
            result.ifMatch.update(header.value(), etag, true);
        } else if (hasIfNoneMatch && detail::httpAsciiEqualsIgnoreCase(
                       header.name(), "If-None-Match")) {
            result.ifNoneMatch.update(header.value(), etag, false);
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

bool ifRangeAllows(
    std::string_view header,
    std::string_view etag,
    std::time_t modifiedSeconds,
    bool dateValidatorStrong) noexcept {
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
    // An If-Range date requires an EXACT match against Last-Modified (RFC 9110
    // §13.1.5 / RFC 7233 §3.2: "the comparison ... uses an exact match"), NOT the
    // "<=" not-modified-since comparison. If-Range's job is to confirm the client
    // still holds the byte-identical representation before a range is stitched in;
    // a representation whose Last-Modified is merely older (a rollback or a restore
    // that moves mtime backwards) is a DIFFERENT entity, and serving a 206 from it
    // would corrupt the client's reassembled copy. Only equality means "unchanged".
    const auto date = detail::httpParseHttpDate(value);
    return date.has_value() && modifiedSeconds == *date;
}

FileConditionalHeaders fileConditionalHeaders(const HttpRequest& request) noexcept {
    return FileConditionalHeaders{
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfUnmodifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfModifiedSince),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kRange),
        detail::requestKnownHeader(request, detail::RequestKnownHeader::kIfRange),
        detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kIfRange)};
}

}  // namespace ruvia
