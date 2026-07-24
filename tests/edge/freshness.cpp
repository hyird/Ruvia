// evaluateFreshness applies RFC 9111 shared-cache policy to an origin response:
// which responses an edge may store and for how long. These checks pin the
// storable-status gate, the no-store/private/no-cache refusals, the
// s-maxage > max-age > Expires lifetime precedence, and the age correction that
// rejects a response already stale on arrival.

#include <cstdio>

#include "ruvia/edge/detail/cache/Freshness.h"
#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpStatus.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

ruvia::CacheControl cc(std::string_view value) {
    return ruvia::parseCacheControl(value);
}

}  // namespace

int main() {
    using ruvia::edge::evaluateFreshness;
    using ruvia::edge::FreshnessInput;

    constexpr std::time_t kNow = 1'000'000'000;

    // max-age gives a concrete freshness deadline.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=60");
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable, "200 with max-age is cacheable");
        check(d.expiresAt == kNow + 60, "max-age sets expiry now + 60");
    }

    // s-maxage overrides max-age for a shared cache.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=60, s-maxage=600");
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.expiresAt == kNow + 600, "s-maxage wins over max-age in a shared cache");
    }

    // no-store, private, and no-cache are all refused by a shared cache.
    for (const char* directive : {"no-store", "private", "no-cache"}) {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc(directive);
        in.now = kNow;
        // Even paired with a long max-age, the refusal wins.
        in.cacheControl.maxAge = 3600;
        const auto d = evaluateFreshness(in);
        check(!d.cacheable, directive);
    }

    // A response with no explicit freshness signal is not stored (no heuristics).
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(!d.cacheable, "no freshness signal means not cacheable");
    }

    // A shared cache cannot store an authenticated response merely because it
    // has max-age. RFC 9111 section 3.5 requires an explicit shared-cache opt-in.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=60");
        in.requestHasAuthorization = true;
        in.now = kNow;
        check(!evaluateFreshness(in).cacheable, "Authorization plus max-age alone is not shared-cacheable");

        for (const char* directive : {"max-age=60, public", "max-age=60, must-revalidate", "s-maxage=60"}) {
            in.cacheControl = cc(directive);
            check(evaluateFreshness(in).cacheable, "explicit Authorization cache opt-in is honored");
        }
        in.cacheControl = cc("max-age=60, proxy-revalidate");
        check(!evaluateFreshness(in).cacheable, "proxy-revalidate alone does not authorize shared storage");
    }

    // A non-storable status is refused even when explicitly fresh.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kInternalServerError.value();
        in.cacheControl = cc("max-age=60");
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(!d.cacheable, "500 is never stored");
    }
    // ...but 404 is in the storable set.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kNotFound.value();
        in.cacheControl = cc("max-age=30");
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.expiresAt == kNow + 30, "404 with max-age is stored");
    }

    // Expires minus Date yields the lifetime when Cache-Control is absent.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.dateHeader = kNow;
        in.expiresHeader = kNow + 120;
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.expiresAt == kNow + 120, "Expires - Date sets lifetime");
    }

    // The Age header consumes part of the lifetime: a max-age=100 response that
    // is already 40s old expires in 60s, not 100s.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=100");
        in.ageHeader = 40;
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.expiresAt == kNow + 60, "Age is subtracted from remaining freshness");
        check(d.initialAge == 40, "corrected initial Age is retained for downstream responses");
    }

    // Upstream response delay contributes to corrected_age_value rather than
    // disappearing when the entry is stored.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=100");
        in.ageHeader = 40;
        in.requestTime = kNow - 5;
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.initialAge == 45, "response delay is included in corrected initial Age");
        check(d.expiresAt == kNow + 55, "response delay consumes the freshness lifetime");
    }

    // A response already older than its lifetime is not stored.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=30");
        in.ageHeader = 30;
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(!d.cacheable, "born-stale response is refused");
    }

    // stale-while-revalidate / stale-if-error windows are carried through.
    {
        FreshnessInput in;
        in.status = ruvia::http_status::kOk.value();
        in.cacheControl = cc("max-age=60, stale-while-revalidate=30, stale-if-error=120");
        in.now = kNow;
        const auto d = evaluateFreshness(in);
        check(d.cacheable && d.staleWhileRevalidate == 30 && d.staleIfError == 120, "stale-* windows are preserved");
    }

    if (failures == 0) {
        std::fprintf(stderr, "edge freshness: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
