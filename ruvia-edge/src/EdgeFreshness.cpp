#include "ruvia/edge/EdgeFreshness.h"

#include <algorithm>

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
    const std::uint64_t initialAge = std::max(input.ageHeader, apparentAge);

    if (initialAge >= *lifetime) {
        return {};  // Already stale on arrival: not worth storing.
    }
    const std::uint64_t remaining = *lifetime - initialAge;

    FreshnessDecision decision;
    decision.cacheable = true;
    decision.expiresAt = input.now + static_cast<std::time_t>(remaining);
    decision.staleWhileRevalidate = cc.staleWhileRevalidate.value_or(0);
    decision.staleIfError = cc.staleIfError.value_or(0);
    return decision;
}

}  // namespace ruvia::edge
