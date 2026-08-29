#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/field/HttpConditionalRequest.h"
#include "ruvia/http/detail/field/HttpEntityTag.h"

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 9110) for static file responses.

// httpFormatDate must emit RFC 7231 IMF-fixdate with English day/month names
// independent of the process locale (regression: it used strftime %a/%b).

// httpWriteImfFixdate is the single owner of HTTP date formatting (used by both
// httpFormatDate/Last-Modified and the response Date header cache). Test it in
// isolation with a hand-built tm so no gmtime dependency is involved.

// The response Date header cache must emit "Date: <IMF-fixdate>\r\n" with a valid
// English date reflecting the current second (guards the shared formatter on the
// hot per-response path).

// Preconditions and the validators they compare: entity tags, strong and weak.

RUVIA_TEST(conditional_method_plan_follows_precondition_and_range_semantics) {
    using ruvia::detail::httpConditionalMethodPlan;

    const auto get = httpConditionalMethodPlan(ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(get.evaluatesPreconditions);
    RUVIA_CHECK(get.usesNotModifiedResponse);
    RUVIA_CHECK(get.evaluatesIfModifiedSince);
    RUVIA_CHECK(get.evaluatesRange);

    const auto head = httpConditionalMethodPlan(ruvia::HttpKnownMethod::kHead);
    RUVIA_CHECK(head.evaluatesPreconditions);
    RUVIA_CHECK(head.usesNotModifiedResponse);
    RUVIA_CHECK(head.evaluatesIfModifiedSince);
    RUVIA_CHECK(!head.evaluatesRange);

    for (const auto method : {ruvia::HttpKnownMethod::kPost, ruvia::HttpKnownMethod::kPut,
             ruvia::HttpKnownMethod::kDelete, ruvia::HttpKnownMethod::kPatch}) {
        const auto plan = httpConditionalMethodPlan(method);
        RUVIA_CHECK(plan.evaluatesPreconditions);
        RUVIA_CHECK(!plan.usesNotModifiedResponse);
        RUVIA_CHECK(!plan.evaluatesIfModifiedSince);
        RUVIA_CHECK(!plan.evaluatesRange);
    }

    for (const auto method : {ruvia::HttpKnownMethod::kOptions, ruvia::HttpKnownMethod::kConnect,
             ruvia::HttpKnownMethod::kUnknown}) {
        const auto plan = httpConditionalMethodPlan(method);
        RUVIA_CHECK(!plan.evaluatesPreconditions);
        RUVIA_CHECK(!plan.usesNotModifiedResponse);
        RUVIA_CHECK(!plan.evaluatesIfModifiedSince);
        RUVIA_CHECK(!plan.evaluatesRange);
    }
}

RUVIA_TEST(etag_strong_comparison) {
    using ruvia::detail::httpStrongEtagEquals;
    // Strong compare: both must be strong (no "W/") and octet-equal.
    RUVIA_CHECK(httpStrongEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpStrongEtagEquals(R"("abc")", R"("abd")"));    // different tag
    RUVIA_CHECK(!httpStrongEtagEquals(R"(W/"abc")", R"("abc")"));  // one weak -> never strong-equal
    RUVIA_CHECK(
        !httpStrongEtagEquals(R"(W/"abc")", R"(W/"abc")"));  // both weak -> not a strong match
}

RUVIA_TEST(etag_weak_comparison) {
    using ruvia::detail::httpWeakEtagEquals;
    // Weak compare: equal after stripping an optional "W/" prefix.
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"("abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"(W/"abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpWeakEtagEquals(R"(W/"abc")", R"(W/"abd")"));  // different opaque tags
}

RUVIA_TEST(etag_weak_prefix_detection) {
    using ruvia::detail::httpIsWeakEtag;
    RUVIA_CHECK(httpIsWeakEtag(R"(W/"abc")"));
    RUVIA_CHECK(!httpIsWeakEtag(R"("abc")"));
    RUVIA_CHECK(!httpIsWeakEtag("W"));  // too short to be the "W/" marker
}

RUVIA_TEST(etag_list_parses_opaque_commas_and_rejects_malformed_suffixes) {
    using ruvia::detail::httpEtagListMatches;
    using ruvia::detail::httpParseEtagListMatches;
    RUVIA_CHECK(httpEtagListMatches(R"("stale,tag", "current")", R"("current")", true));
    RUVIA_CHECK(httpEtagListMatches(R"("stale", W/"current")", R"("current")", false));
    RUVIA_CHECK(!httpEtagListMatches(R"("stale, "current")", R"("current")", true));
    RUVIA_CHECK(!httpEtagListMatches(R"("current" trailing)", R"("current")", false));
    const auto malformedAfterMatch =
        httpParseEtagListMatches(R"("current", malformed)", R"("current")", true);
    RUVIA_CHECK(!malformedAfterMatch.valid);
    RUVIA_CHECK(!malformedAfterMatch.matched);
}
