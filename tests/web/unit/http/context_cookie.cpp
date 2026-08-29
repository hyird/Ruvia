#include "context_request_fixture.h"

// Setting response cookies, including prefixes and signatures.

RUVIA_TEST(context_set_cookie_serializes_all_attributes) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // The Set-Cookie serialization is a two-pass design: prepareSetCookie computes
    // an exact byte count, then writeSetCookie fills precisely that many bytes.
    // A maximal cookie exercises every branch of BOTH passes at once, so any
    // divergence between the size computed and the bytes written (an over- or
    // under-allocation, or a dropped attribute in one pass only) surfaces as a
    // wrong output string. Case A: __Host- prefix with all string/flag attributes
    // and a Max-Age (no Domain/Expires -- __Host- forbids Domain).
    const ruvia::CookieOptions host{
        .path = "/",
        .sameSite = ruvia::CookieSameSite::kStrict,
        .priority = ruvia::CookiePriority::kHigh,
        .maxAge = std::chrono::seconds(3600),
        .prefix = ruvia::CookiePrefix::kHost,
        .httpOnly = ruvia::CookieAttributePolicy::kEmit,
        .secure = ruvia::CookieAttributePolicy::kEmit,
        .partitioned = ruvia::CookieAttributePolicy::kEmit,
    };
    context.setCookie({.name = "id", .value = "abc", .attributes = host});
    const auto hostResponse = context.text("ok");
    RUVIA_CHECK_EQ(hostResponse.header("Set-Cookie"),
        std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                         "SameSite=Strict; Priority=High; Partitioned"));

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    const ruvia::CookieOptions secure{
        .path = "/app",
        .domain = "example.com",
        .sameSite = ruvia::CookieSameSite::kNone,
        .expires = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890)),
        .prefix = ruvia::CookiePrefix::kSecure,
        .secure = ruvia::CookieAttributePolicy::kEmit,
    };
    HttpRequest secureRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(secureRequest);
    RequestMemory secureMemory(worker);
    HttpRequestAccess::setResource(secureRequest, secureMemory.resource());
    auto secureContext =
        ContextAccess::make(secureMemory, secureRequest, ruvia::test::testContextServices());
    secureContext.setCookie({.name = "sess", .value = "xyz", .attributes = secure});
    const auto secureResponse = secureContext.text("ok");
    RUVIA_CHECK_EQ(secureResponse.header("Set-Cookie"),
        std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                         "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
}

RUVIA_TEST(context_set_cookie_preserves_same_name_different_path) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    ruvia::CookieOptions root;
    root.path = "/";
    ruvia::CookieOptions admin;
    admin.path = "/admin";
    context.setCookie({.name = "session", .value = "root-old", .attributes = root});
    context.setCookie({.name = "session", .value = "admin", .attributes = admin});
    context.setCookie({.name = "session", .value = "root-new", .attributes = root});
    const auto response = context.text("ok");

    std::size_t count = 0;
    bool hasRootOld = false;
    bool hasRootNew = false;
    bool hasAdmin = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        hasRootOld = hasRootOld || header.value() == "session=root-old; Path=/";
        hasRootNew = hasRootNew || header.value() == "session=root-new; Path=/";
        hasAdmin = hasAdmin || header.value() == "session=admin; Path=/admin";
    }
    RUVIA_CHECK_EQ(count, std::size_t{2});
    RUVIA_CHECK(!hasRootOld);
    RUVIA_CHECK(hasRootNew);
    RUVIA_CHECK(hasAdmin);
}

RUVIA_TEST(context_signed_cookie_with_prefix_verifies_round_trip) {
    WorkerMemory worker;
    HttpRequest writeRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(writeRequest);
    RequestMemory writeMemory(worker);
    HttpRequestAccess::setResource(writeRequest, writeMemory.resource());
    auto writeContext =
        ContextAccess::make(writeMemory, writeRequest, ruvia::test::testContextServices());

    const ruvia::CookieOptions options{
        .path = "/",
        .prefix = ruvia::CookiePrefix::kHost,
        .secure = ruvia::CookieAttributePolicy::kEmit,
    };
    writeContext.setSignedCookie(
        {.name = "session", .value = "user-1", .secret = "secret", .attributes = options});
    const auto writeResponse = writeContext.text("ok");
    const std::string setCookie(writeResponse.header("Set-Cookie").value_or(std::string_view{}));
    const std::string_view line(setCookie);
    const auto pair = line.substr(0, line.find(';'));
    RUVIA_CHECK(pair.starts_with("__Host-session="));

    // Present the cookie exactly as a browser sends it back.
    HttpRequest readRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(readRequest);
    const std::string cookieField(pair);
    RUVIA_CHECK(HttpRequestAccess::addHeader(readRequest, HttpHeaderView{"Cookie", cookieField},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory readMemory(worker);
    HttpRequestAccess::setResource(readRequest, readMemory.resource());
    auto readContext =
        ContextAccess::make(readMemory, readRequest, ruvia::test::testContextServices());

    const auto verified =
        readContext.req().signedCookie({.name = "__Host-session", .secret = "secret"});
    RUVIA_CHECK(verified.has_value());
    RUVIA_CHECK_EQ(*verified, std::string_view("user-1"));

    // An unprefixed signed cookie keeps verifying under its own name.
    HttpRequest bareWriteRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(bareWriteRequest);
    RequestMemory bareWriteMemory(worker);
    HttpRequestAccess::setResource(bareWriteRequest, bareWriteMemory.resource());
    auto bareWriteContext =
        ContextAccess::make(bareWriteMemory, bareWriteRequest, ruvia::test::testContextServices());
    bareWriteContext.setSignedCookie({.name = "plain", .value = "user-2", .secret = "secret"});
    const auto bareWriteResponse = bareWriteContext.text("ok");
    const std::string bare(bareWriteResponse.header("Set-Cookie").value_or(std::string_view{}));
    const std::string_view bareLine(bare);
    const std::string bareField(bareLine.substr(0, bareLine.find(';')));
    HttpRequest bareRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(bareRequest);
    RUVIA_CHECK(HttpRequestAccess::addHeader(bareRequest, HttpHeaderView{"Cookie", bareField},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory bareMemory(worker);
    HttpRequestAccess::setResource(bareRequest, bareMemory.resource());
    auto bareContext =
        ContextAccess::make(bareMemory, bareRequest, ruvia::test::testContextServices());
    const auto bareVerified = bareContext.req().signedCookie({.name = "plain", .secret = "secret"});
    RUVIA_CHECK(bareVerified.has_value());
    RUVIA_CHECK_EQ(*bareVerified, std::string_view("user-2"));
}

RUVIA_TEST(context_delete_cookie_with_prefix_is_response_only) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "__Host-session=user-1"},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const ruvia::CookieOptions options{
        .path = "/",
        .prefix = ruvia::CookiePrefix::kHost,
        .secure = ruvia::CookieAttributePolicy::kEmit,
    };
    const auto previous = context.req().cookie("__Host-session");
    RUVIA_CHECK(previous.has_value());
    RUVIA_CHECK_EQ(*previous, std::string_view("user-1"));
    context.deleteCookie({.name = "session", .attributes = options});
    const auto response = context.text("deleted");
    const auto setCookie = response.header("Set-Cookie");
    RUVIA_CHECK(setCookie.has_value());
    RUVIA_CHECK(setCookie.value_or(std::string_view{}).starts_with("__Host-session=;"));
    RUVIA_CHECK(setCookie.value_or(std::string_view{}).find("Max-Age=0") != std::string_view::npos);
}
