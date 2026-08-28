#pragma once

#include <cstddef>
#include <ctime>
#include <string_view>

#include "ruvia/http/HttpRequest.h"

// Evaluating a file request's precondition fields (RFC 9110 sections 13.1 and
// 13.1.5) against a file's validators: the If-Match / If-None-Match outcomes,
// the two date comparisons, and whether an If-Range still authorises serving a
// range. Answering these needs only the request and the file's etag and mtime,
// never a response.

namespace ruvia {

// The precondition fields a file response reads, borrowed from the request.
struct FileConditionalHeaders final {
    std::string_view ifUnmodifiedSince;
    std::string_view ifModifiedSince;
    std::string_view range;
    std::string_view ifRange;
    bool hasIfRange;
};

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match.
struct EtagFieldCondition final {
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

struct FileEtagConditions final {
    EtagFieldCondition ifMatch;
    EtagFieldCondition ifNoneMatch;
};

[[nodiscard]] FileConditionalHeaders fileConditionalHeaders(const HttpRequest& request) noexcept;

[[nodiscard]] FileEtagConditions fileEtagConditions(const HttpRequest& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4.
[[nodiscard]] bool httpDateNotModified(std::string_view header, std::time_t modifiedSeconds) noexcept;
[[nodiscard]] bool httpDateUnmodified(std::string_view header, std::time_t modifiedSeconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since -- see the definition.
[[nodiscard]] bool ifRangeAllows(std::string_view header, std::string_view etag, std::time_t modifiedSeconds, bool dateValidatorStrong) noexcept;

}  // namespace ruvia
