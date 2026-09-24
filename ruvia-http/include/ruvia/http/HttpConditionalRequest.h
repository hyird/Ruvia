#pragma once

#include <cstddef>
#include <ctime>
#include <string_view>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"
namespace ruvia {

struct HttpConditionalMethodPlan final {
    bool evaluatesPreconditions;
    bool usesNotModifiedResponse;
    bool evaluatesIfModifiedSince;
    bool evaluatesRange;
};

[[nodiscard]] inline constexpr HttpConditionalMethodPlan httpConditionalMethodPlan(
    HttpKnownMethod method) noexcept {
    switch (method) {
        case HttpKnownMethod::kGet:
            return {true, true, true, true};
        case HttpKnownMethod::kHead:
            return {true, true, true, false};
        case HttpKnownMethod::kPost:
        case HttpKnownMethod::kPut:
        case HttpKnownMethod::kDelete:
        case HttpKnownMethod::kPatch:
            return {true, false, false, false};
        case HttpKnownMethod::kOptions:
        case HttpKnownMethod::kConnect:
        case HttpKnownMethod::kUnknown:
            return {false, false, false, false};
    }
    return {false, false, false, false};
}

// Precondition and range fields borrowed from the request. Presence of If-Range
// is tracked separately because an empty value is still a present field.
struct HttpConditionalHeaders final {
    std::string_view ifUnmodifiedSince;
    std::string_view ifModifiedSince;
    std::string_view range;
    std::string_view ifRange;
    bool hasIfRange;
};

[[nodiscard]] HttpConditionalHeaders httpConditionalHeaders(const HttpRequest& request) noexcept;

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match.
struct HttpEtagFieldCondition final {
    bool present{false};
    bool valid{true};
    bool matched{false};
    bool wildcard{false};
    std::size_t lineCount{0};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;

    [[nodiscard]] bool matches() const noexcept {
        return valid && ((wildcard && lineCount == 1) || (!wildcard && matched));
    }
};

struct HttpEtagPreconditions final {
    HttpEtagFieldCondition ifMatch;
    HttpEtagFieldCondition ifNoneMatch;
};

[[nodiscard]] HttpEtagPreconditions httpEtagPreconditions(
    const HttpRequest& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4. A malformed date is ignored.
[[nodiscard]] bool httpDateNotModified(
    std::string_view header, std::time_t modifiedSeconds) noexcept;

[[nodiscard]] bool httpDateUnmodified(
    std::string_view header, std::time_t modifiedSeconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since.
[[nodiscard]] bool httpIfRangeAllows(std::string_view header, std::string_view etag,
    std::time_t modifiedSeconds, bool dateValidatorStrong) noexcept;

}  // namespace ruvia
