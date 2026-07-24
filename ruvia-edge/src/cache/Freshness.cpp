#include "ruvia/edge/detail/cache/Freshness.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>

namespace ruvia::edge {

namespace {

// Status codes an edge cache may reuse when the response is explicitly fresh.
// This is the RFC 9111 "cacheable by default" set minus 206 (partial content,
// which needs range reassembly the MVP does not do). Any other status is only
// forwarded, never stored.
[[nodiscard]] bool statusIsStorable(int status) noexcept {
    switch (status) {
        case 200:  // OK
        case 203:  // Non-Authoritative Information
        case 204:  // No Content
        case 300:  // Multiple Choices
        case 301:  // Moved Permanently
        case 308:  // Permanent Redirect
        case 404:  // Not Found
        case 405:  // Method Not Allowed
        case 410:  // Gone
        case 414:  // URI Too Long
        case 501:  // Not Implemented
            return true;
        default:
            return false;
    }
}

// The response's explicit freshness lifetime in seconds, if any. s-maxage wins
// for a shared cache; then max-age; then a positive Expires-minus-Date delta
// (or Expires-minus-now when Date is absent). std::nullopt means the response
// carries no explicit freshness and -- since the MVP does no heuristic
// freshness -- must not be stored.
[[nodiscard]] std::optional<std::uint64_t> explicitLifetime(const FreshnessInput& input) noexcept {
    const ruvia::CacheControl& cc = input.cacheControl;
    if (cc.sMaxAge) {
        return *cc.sMaxAge;
    }
    if (cc.maxAge) {
        return *cc.maxAge;
    }
    if (input.expiresHeader) {
        const std::time_t base = input.dateHeader ? *input.dateHeader : input.now;
        if (*input.expiresHeader > base) {
            return static_cast<std::uint64_t>(*input.expiresHeader - base);
        }
        return std::uint64_t{0};  // Expires in the past: lifetime zero, born stale.
    }
    return std::nullopt;
}

}  // namespace

FreshnessDecision evaluateFreshness(const FreshnessInput& input) noexcept {
    const ruvia::CacheControl& cc = input.cacheControl;

    // Directives that forbid a shared cache from storing the response at all.
    // no-cache is treated as non-storable here because the MVP cannot revalidate
    // a stored copy, so a must-revalidate-before-use entry would only ever be
    // refetched -- storing it wastes capacity.
    if (cc.noStore || cc.isPrivate || cc.noCache) {
        return {};
    }
    // RFC 9111 section 3.5 permits a shared cache to store/reuse a response to
    // an authenticated request only when one of these response directives
    // explicitly opts into shared caching. `proxy-revalidate` alone does not.
    if (input.requestHasAuthorization && !cc.mustRevalidate && !cc.isPublic && !cc.sMaxAge) {
        return {};
    }
    if (!statusIsStorable(input.status)) {
        return {};
    }

    const std::optional<std::uint64_t> lifetime = explicitLifetime(input);
    if (!lifetime) {
        return {};  // No explicit freshness signal: do not store.
    }

    // Corrected initial age (RFC 9111 section 4.2.3): the larger of the age the
    // origin declared and the apparent age from the Date header. Resident time
    // is ~0 at the moment of storing, so remaining freshness is lifetime minus
    // this initial age.
    std::uint64_t apparentAge = 0;
    if (input.dateHeader && input.now > *input.dateHeader) {
        apparentAge = static_cast<std::uint64_t>(input.now - *input.dateHeader);
    }
    const std::uint64_t responseDelay = input.requestTime > 0 && input.now > input.requestTime ? static_cast<std::uint64_t>(input.now - input.requestTime) : std::uint64_t{0};
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t correctedAgeValue = input.ageHeader > maximum - responseDelay ? maximum : input.ageHeader + responseDelay;
    const std::uint64_t initialAge = std::max(correctedAgeValue, apparentAge);

    if (initialAge >= *lifetime) {
        return {};  // Already stale on arrival: not worth storing.
    }
    const std::uint64_t remaining = *lifetime - initialAge;

    FreshnessDecision decision;
    decision.cacheable = true;
    decision.expiresAt = input.now + static_cast<std::time_t>(remaining);
    decision.initialAge = initialAge;
    decision.staleWhileRevalidate = cc.staleWhileRevalidate.value_or(0);
    decision.staleIfError = cc.staleIfError.value_or(0);
    return decision;
}

FreshnessInput buildFreshnessInput(std::uint16_t status, const Headers& headers, std::time_t now, std::time_t requestTime, bool requestHasAuthorization) {
    FreshnessInput input;
    input.status = status;
    input.now = now;
    input.requestTime = requestTime;
    input.requestHasAuthorization = requestHasAuthorization;

    CacheControlFieldParser cacheControl;
    for (const auto& [name, value] : headers) {
        if (iequals(name, "cache-control")) {
            cacheControl.update(value);
        } else if (iequals(name, "date")) {
            input.dateHeader = parseHttpDate(value);
        } else if (iequals(name, "expires")) {
            input.expiresHeader = parseHttpDate(value);
        } else if (iequals(name, "age")) {
            std::uint64_t age = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            const auto parsed = std::from_chars(begin, end, age);
            if (parsed.ec == std::errc{} && parsed.ptr == end) {
                input.ageHeader = std::max(input.ageHeader, age);
            }
        }
    }
    input.cacheControl = cacheControl.finish();
    return input;
}

}  // namespace ruvia::edge
