#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpClientRedirect.h"

namespace {

using ruvia::classifyHttpClientOriginAuthority;
using ruvia::HttpClientOriginAuthorityStatus;
using ruvia::HttpClientRedirectContentDisposition;
using ruvia::HttpClientRedirectTargetError;
using ruvia::HttpClientRequest;
using ruvia::HttpOrigin;
using ruvia::HttpScheme;
using ruvia::isHttpClientRedirectStatus;
using ruvia::lookupUniqueHttpClientResponseHeader;
using ruvia::planHttpClientRedirectRequest;
using ruvia::resolveHttpClientSameOriginRedirectTarget;

template <typename T>
concept HasAnyRvalueHttpClientHeaderLookupAccessor = requires(T&& result) { std::move(result).absent(); } || requires(T&& result) { std::move(result).found(); } || requires(T&& result) { std::move(result).repeated(); };

template <typename T>
concept HasAnyRvalueHttpClientRedirectTargetAccessor = requires(T&& result) { std::move(result).target(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesRvalueHttpClientRedirectTargetView = requires(T&& target) { std::move(target).value(); };

template <typename T>
concept ExposesRvalueHttpClientRedirectRequestMethod = requires(T&& plan) { std::move(plan).method(); };

template <typename T>
concept AcceptsTemporaryHttpClientResponseHeaderLookup = requires(T&& response) { lookupUniqueHttpClientResponseHeader(std::move(response), std::string_view{}); };

static_assert(!HasAnyRvalueHttpClientHeaderLookupAccessor<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(!HasAnyRvalueHttpClientRedirectTargetAccessor<ruvia::HttpClientRedirectTargetResult>);
static_assert(!ExposesRvalueHttpClientRedirectTargetView<ruvia::HttpClientRedirectTarget>);
static_assert(!ExposesRvalueHttpClientRedirectRequestMethod<ruvia::HttpClientRedirectRequestPlan>);
static_assert(!std::copy_constructible<ruvia::HttpClientRedirectRequestPlan>);
static_assert(std::move_constructible<ruvia::HttpClientRedirectRequestPlan>);
static_assert(!AcceptsTemporaryHttpClientResponseHeaderLookup<ruvia::HttpClientResponseHead>);

template <typename T>
concept HasHeaderValue = requires(const T& value) {
    { value.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasRedirectTargetError = requires(const T& value) {
    { value.error() } -> std::same_as<HttpClientRedirectTargetError>;
};

template <typename T>
concept HasRedirectStatus = requires(const T& value) { value.status(); };

static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientResponseHeaderLookupResult&>().found()), const ruvia::HttpClientResponseHeaderFound*>);
static_assert(!HasHeaderValue<ruvia::HttpClientResponseHeaderAbsent>);
static_assert(HasHeaderValue<ruvia::HttpClientResponseHeaderFound>);
static_assert(!HasHeaderValue<ruvia::HttpClientResponseHeaderRepeated>);
static_assert(!HasRedirectStatus<ruvia::HttpClientResponseHeaderLookupResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpClientRedirectTargetResult&>().target()), const ruvia::HttpClientRedirectTarget*>);
static_assert(!HasRedirectTargetError<ruvia::HttpClientRedirectTarget>);
static_assert(HasRedirectTargetError<ruvia::HttpClientRedirectTargetFailure>);
static_assert(!HasRedirectStatus<ruvia::HttpClientRedirectTargetResult>);

HttpOrigin originFor(std::string_view host, std::uint16_t port, HttpScheme scheme = HttpScheme::kHttp) {
    return scheme == HttpScheme::kHttps ? HttpOrigin::https(host, port) : HttpOrigin::http(host, port);
}

void checkResolvedTarget(ruvia::testing::TestContext& ruvia_ctx, const HttpOrigin& origin, std::string_view currentTarget, std::string_view location, std::string_view expected) {
    const auto result = resolveHttpClientSameOriginRedirectTarget(origin, currentTarget, location, std::pmr::get_default_resource());
    RUVIA_CHECK(result.target() != nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
    if (const auto* target = result.target()) {
        RUVIA_CHECK_EQ(target->value(), expected);
    }
}

void checkRedirectTargetFailure(ruvia::testing::TestContext& ruvia_ctx, const HttpOrigin& origin, std::string_view currentTarget, std::string_view location, HttpClientRedirectTargetError expected) {
    const auto result = resolveHttpClientSameOriginRedirectTarget(origin, currentTarget, location, std::pmr::get_default_resource());
    RUVIA_CHECK(result.target() == nullptr);
    RUVIA_CHECK(result.failure() != nullptr);
    if (const auto* failure = result.failure()) {
        RUVIA_CHECK(failure->error() == expected);
    }
}

}  // namespace

RUVIA_TEST(http_client_redirect_status_set) {
    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kMovedPermanently, ruvia::http_status::kFound, ruvia::http_status::kSeeOther, ruvia::http_status::kTemporaryRedirect, ruvia::http_status::kPermanentRedirect}) {
        RUVIA_CHECK(isHttpClientRedirectStatus(status));
    }
    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kOk, ruvia::http_status::kNoContent, ruvia::http_status::kMultipleChoices, ruvia::http_status::kNotModified, ruvia::http_status::kUseProxy, ruvia::HttpStatusCode::fromValue(306), ruvia::HttpStatusCode::fromValue(399), ruvia::http_status::kNotFound}) {
        RUVIA_CHECK(!isHttpClientRedirectStatus(status));
    }
}

RUVIA_TEST(http_client_redirect_request_plan_follows_rfc) {
    // 303 selects a retrieval request. HEAD remains HEAD; every other method
    // becomes GET. The representation and content-specific fields are dropped.
    {
        HttpClientRequest request;
        request.method = "PUT";
        request.content = ruvia::HttpClientRequestContent::bytes("payload");
        const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kSeeOther);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("GET"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kDrop);
    }
    {
        HttpClientRequest request;
        request.method = "HEAD";
        const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kSeeOther);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("HEAD"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kDrop);
    }

    // RFC 9110 permits the historical POST-to-GET rewrite for 301/302. Other
    // methods are not aliases for POST and retain both method and content.
    {
        HttpClientRequest request;
        request.method = "POST";
        request.content = ruvia::HttpClientRequestContent::bytes("payload");
        const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kFound);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("GET"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kDrop);
    }
    {
        HttpClientRequest request;
        request.method = "PUT";
        request.content = ruvia::HttpClientRequestContent::bytes("payload");
        const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kMovedPermanently);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("PUT"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kPreserve);
    }

    // Method tokens are case-sensitive: lowercase "post" is a distinct method.
    {
        HttpClientRequest request;
        request.method = "post";
        const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kMovedPermanently);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("post"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kPreserve);
    }

    // 307/308 never change method or content.
    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kTemporaryRedirect, ruvia::http_status::kPermanentRedirect}) {
        HttpClientRequest request;
        request.method = "POST";
        request.content = ruvia::HttpClientRequestContent::bytes("payload");
        const auto plan = planHttpClientRedirectRequest(request, status);
        RUVIA_CHECK_EQ(plan.method(), std::string_view("POST"));
        RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kPreserve);
    }
}

RUVIA_TEST(http_client_redirect_request_plan_owns_preserved_method) {
    std::string method = "PROPFIND";
    HttpClientRequest request;
    request.method = method;

    const auto plan = planHttpClientRedirectRequest(request, ruvia::http_status::kTemporaryRedirect);
    for (char& ch : method) {
        ch = 'X';
    }

    RUVIA_CHECK_EQ(plan.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(plan.contentDisposition() == HttpClientRedirectContentDisposition::kPreserve);
}

RUVIA_TEST(http_client_response_header_lookup_distinguishes_empty_and_repeated) {
    auto head = ruvia::detail::HttpClientResponseHeadAccess::make(ruvia::http_status::kFound, ruvia::HttpProtocolVersion::kHttp11, std::pmr::get_default_resource());
    auto& headers = ruvia::detail::HttpClientResponseHeadAccess::headers(head);
    headers.emplace_back(ruvia::detail::HttpClientResponseHeaderAccess::make("Location", "", std::pmr::get_default_resource()));

    const auto empty = lookupUniqueHttpClientResponseHeader(head, "location");
    RUVIA_CHECK(empty.absent() == nullptr);
    RUVIA_CHECK(empty.found() != nullptr);
    RUVIA_CHECK(empty.repeated() == nullptr);
    if (const auto* found = empty.found()) {
        RUVIA_CHECK(found->value().empty());
    }
    const auto missing = lookupUniqueHttpClientResponseHeader(head, "missing");
    RUVIA_CHECK(missing.absent() != nullptr);
    RUVIA_CHECK(missing.found() == nullptr);
    RUVIA_CHECK(missing.repeated() == nullptr);

    headers.emplace_back(ruvia::detail::HttpClientResponseHeaderAccess::make("LOCATION", "/second", std::pmr::get_default_resource()));
    const auto repeated = lookupUniqueHttpClientResponseHeader(head, "Location");
    RUVIA_CHECK(repeated.absent() == nullptr);
    RUVIA_CHECK(repeated.found() == nullptr);
    RUVIA_CHECK(repeated.repeated() != nullptr);
}

RUVIA_TEST(http_client_authority_matches_typed_origin) {
    const auto nonDefault = originFor("example.com", 8080);
    const auto is = [](const HttpOrigin& origin, std::string_view authority) { return classifyHttpClientOriginAuthority(origin, authority); };
    RUVIA_CHECK(is(nonDefault, "example.com:8080") == HttpClientOriginAuthorityStatus::kSameOrigin);
    for (const std::string_view different : {"example.com", "example.com:9090", "other.com:8080", "example.com:0", "example.com:"}) {
        RUVIA_CHECK(is(nonDefault, different) == HttpClientOriginAuthorityStatus::kDifferentOrigin);
    }
    RUVIA_CHECK(is(nonDefault, "user@example.com:8080") == HttpClientOriginAuthorityStatus::kInvalidAuthority);
    RUVIA_CHECK(is(nonDefault, "example.com:99999") == HttpClientOriginAuthorityStatus::kInvalidAuthority);

    RUVIA_CHECK(is(HttpOrigin::http("example.com"), "example.com") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(HttpOrigin::http("example.com"), "EXAMPLE.com:") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(HttpOrigin::http("example.com"), "exa%6dple.com") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(HttpOrigin::http("!example"), "%21example") == HttpClientOriginAuthorityStatus::kDifferentOrigin);
    RUVIA_CHECK(is(HttpOrigin::https("example.com"), "example.com") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(HttpOrigin::http("example.com", 0), "example.com:0") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(HttpOrigin::http("example.com", 0), "example.com") == HttpClientOriginAuthorityStatus::kDifferentOrigin);

    const auto v6 = originFor("[::1]", 8080);
    RUVIA_CHECK(is(v6, "[::1]:8080") == HttpClientOriginAuthorityStatus::kSameOrigin);
    RUVIA_CHECK(is(v6, "[::2]:8080") == HttpClientOriginAuthorityStatus::kDifferentOrigin);
    RUVIA_CHECK(is(v6, "[::1]:") == HttpClientOriginAuthorityStatus::kDifferentOrigin);

    const auto future = HttpOrigin::http("[v1.future]");
    RUVIA_CHECK(is(future, "[V1.FUTURE]:") == HttpClientOriginAuthorityStatus::kSameOrigin);
}

RUVIA_TEST(http_client_same_origin_redirect_resolves_uri_references) {
    const auto origin = HttpOrigin::http("example.com");
    constexpr std::string_view current = "/base/dir/page?old=1";

    checkResolvedTarget(ruvia_ctx, origin, current, "/new/path", "/new/path");
    checkResolvedTarget(ruvia_ctx, origin, current, "http://example.com/next", "/next");
    checkResolvedTarget(ruvia_ctx, origin, current, "//example.com/rel", "/rel");
    checkResolvedTarget(ruvia_ctx, origin, current, "http://EXA%6dPLE.com:/normalized", "/normalized");
    checkResolvedTarget(ruvia_ctx, origin, current, "next", "/base/dir/next");
    checkResolvedTarget(ruvia_ctx, origin, current, "../other/./item", "/base/other/item");
    checkResolvedTarget(ruvia_ctx, origin, current, "?new=2", "/base/dir/page?new=2");
    checkResolvedTarget(ruvia_ctx, origin, current, "#fragment", current);
    checkResolvedTarget(ruvia_ctx, origin, current, "/next#part/one?x=%2F:@!$&'()*+,;=", "/next");
    checkResolvedTarget(ruvia_ctx, origin, current, "", current);
    checkResolvedTarget(ruvia_ctx, origin, current, "/a/../b#section", "/b");
}

RUVIA_TEST(http_client_same_origin_redirect_reports_rejection_reason) {
    const auto origin = HttpOrigin::http("example.com");

    checkRedirectTargetFailure(ruvia_ctx, origin, "/current", "http://evil.com/next", HttpClientRedirectTargetError::kNotSameOrigin);
    checkRedirectTargetFailure(ruvia_ctx, origin, "/current", "https://example.com/next", HttpClientRedirectTargetError::kNotSameOrigin);
    for (const std::string_view invalid : {"https://user@example.com/next", "https://example.com:99999/next", "http://user@example.com/next", "http://example.com:99999/next", "http:/broken", "/next#bad fragment", "/next#%zz", "/next#[bad]", "/next#first#second"}) {
        checkRedirectTargetFailure(ruvia_ctx, origin, "/current", invalid, HttpClientRedirectTargetError::kInvalidLocation);
    }
    checkRedirectTargetFailure(ruvia_ctx, origin, "*", "/next", HttpClientRedirectTargetError::kInvalidCurrentTarget);
}

RUVIA_TEST(http_client_same_origin_redirect_supports_ipvfuture) {
    const auto origin = HttpOrigin::http("[v1.future]");
    checkResolvedTarget(ruvia_ctx, origin, "/current", "http://[V1.FUTURE]:/next", "/next");
}

RUVIA_TEST(http_client_redirect_relative_resolution_matches_rfc3986_examples) {
    const auto origin = HttpOrigin::http("a");
    constexpr std::string_view current = "/b/c/d;p?q";

    struct Example final {
        std::string_view reference;
        std::string_view target;
    };
    constexpr Example examples[] = {
        {"g", "/b/c/g"},
        {"./g", "/b/c/g"},
        {"g/", "/b/c/g/"},
        {"/g", "/g"},
        {"?y", "/b/c/d;p?y"},
        {"g?y", "/b/c/g?y"},
        {"#s", "/b/c/d;p?q"},
        {"g#s", "/b/c/g"},
        {";p", "/b/c/;p"},
        {"", "/b/c/d;p?q"},
        {".", "/b/c/"},
        {"./", "/b/c/"},
        {"..", "/b/"},
        {"../", "/b/"},
        {"../g", "/b/g"},
        {"../..", "/"},
        {"../../g", "/g"},
        {"../../../g", "/g"},
        {"/./g", "/g"},
        {"/../g", "/g"},
        {"g/./h", "/b/c/g/h"},
        {"g/../h", "/b/c/h"},
        {"g;x=1/../y", "/b/c/y"},
        {"/a//.", "/a//"},
        {"g//.", "/b/c/g//"},
        {"g?y/./x", "/b/c/g?y/./x"},
    };

    for (const auto& example : examples) {
        checkResolvedTarget(ruvia_ctx, origin, current, example.reference, example.target);
    }
}

namespace {

using ruvia::HttpClientRedirectResolutionError;
using ruvia::resolveHttpClientRedirectTarget;

struct ExpectedResolvedRedirect final {
    HttpScheme scheme;
    std::string_view host;
    std::uint16_t port;
    std::string_view target;
    bool crossOrigin;
};

void checkResolvedRedirect(ruvia::testing::TestContext& ruvia_ctx, const HttpOrigin& origin, std::string_view currentTarget, std::string_view location, const ExpectedResolvedRedirect& expected) {
    const auto result = resolveHttpClientRedirectTarget(origin, currentTarget, location, std::pmr::get_default_resource());
    RUVIA_CHECK(result.failure() == nullptr);
    RUVIA_CHECK(result.resolved() != nullptr);
    if (const auto* resolved = result.resolved()) {
        RUVIA_CHECK(resolved->scheme() == expected.scheme);
        RUVIA_CHECK_EQ(resolved->host(), expected.host);
        RUVIA_CHECK_EQ(resolved->port(), expected.port);
        RUVIA_CHECK_EQ(resolved->target(), expected.target);
        RUVIA_CHECK_EQ(resolved->crossOrigin(), expected.crossOrigin);
    }
}

void checkRedirectResolutionFailure(ruvia::testing::TestContext& ruvia_ctx, const HttpOrigin& origin, std::string_view currentTarget, std::string_view location, HttpClientRedirectResolutionError expected) {
    const auto result = resolveHttpClientRedirectTarget(origin, currentTarget, location, std::pmr::get_default_resource());
    RUVIA_CHECK(result.resolved() == nullptr);
    RUVIA_CHECK(result.failure() != nullptr);
    if (const auto* failure = result.failure()) {
        RUVIA_CHECK(failure->error() == expected);
    }
}

}  // namespace

RUVIA_TEST(http_client_redirect_resolution_same_origin_stays_relative) {
    const auto origin = originFor("example.com", 80);
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b?old=1", "c?x=1", {HttpScheme::kHttp, "example.com", 80, "/a/c?x=1", false});
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "http://example.com/x", {HttpScheme::kHttp, "example.com", 80, "/x", false});
    // Explicit default port and a case-different host are the same origin.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "HTTP://EXAMPLE.COM:80/x", {HttpScheme::kHttp, "EXAMPLE.COM", 80, "/x", false});
}

RUVIA_TEST(http_client_redirect_resolution_classifies_cross_origin) {
    const auto origin = originFor("example.com", 80);
    // Different host, default port for the located scheme.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "https://other.example/path?q=1", {HttpScheme::kHttps, "other.example", 443, "/path?q=1", true});
    // Same host, different port.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "http://example.com:8080/x", {HttpScheme::kHttp, "example.com", 8080, "/x", true});
    // Scheme change alone crosses the origin even on the same host.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "https://example.com/x", {HttpScheme::kHttps, "example.com", 443, "/x", true});
    // A protocol-relative reference keeps the scheme but moves authority.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "//other.example/p", {HttpScheme::kHttp, "other.example", 80, "/p", true});
    // Path normalization and fragment stripping apply across origins too.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "https://other.example/a/../b#frag", {HttpScheme::kHttps, "other.example", 443, "/b", true});
    // IPv6 literals keep their brackets, matching the HttpOrigin contract.
    checkResolvedRedirect(ruvia_ctx, origin, "/a/b", "http://[::1]:8080/x", {HttpScheme::kHttp, "[::1]", 8080, "/x", true});
}

RUVIA_TEST(http_client_redirect_resolution_builds_borrowing_origin) {
    const auto origin = originFor("example.com", 80);
    const auto result = resolveHttpClientRedirectTarget(origin, "/a/b", "https://other.example:8443/x", std::pmr::get_default_resource());
    RUVIA_CHECK(result.resolved() != nullptr);
    if (const auto* resolved = result.resolved()) {
        const auto nextOrigin = resolved->origin();
        RUVIA_CHECK(nextOrigin.scheme() == HttpScheme::kHttps);
        RUVIA_CHECK_EQ(nextOrigin.host(), std::string_view("other.example"));
        RUVIA_CHECK_EQ(nextOrigin.port(), std::uint16_t{8443});
    }
}

RUVIA_TEST(http_client_redirect_resolution_reports_typed_failures) {
    const auto origin = originFor("example.com", 80);
    checkRedirectResolutionFailure(ruvia_ctx, origin, "/a/b", "ftp://example.com/file", HttpClientRedirectResolutionError::kUnsupportedScheme);
    checkRedirectResolutionFailure(ruvia_ctx, origin, "/a/b", "mailto:someone@example.com", HttpClientRedirectResolutionError::kUnsupportedScheme);
    // Userinfo remains rejected: RFC 9110 deprecates it and clients must not
    // leak credentials embedded by the peer.
    checkRedirectResolutionFailure(ruvia_ctx, origin, "/a/b", "https://user@other.example/", HttpClientRedirectResolutionError::kInvalidLocation);
    checkRedirectResolutionFailure(ruvia_ctx, origin, "/a/b", "http:opaque-without-authority", HttpClientRedirectResolutionError::kInvalidLocation);
    checkRedirectResolutionFailure(ruvia_ctx, origin, "not-a-target", "/x", HttpClientRedirectResolutionError::kInvalidCurrentTarget);
}
