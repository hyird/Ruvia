#include "context_request_fixture.h"

// Setting response cookies, including prefixes and signatures.

RUVIA_TEST(context_set_cookie_serializes_all_attributes) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // The Set-Cookie serialization is a two-pass design: prepareSetCookie computes
    // an exact byte count, then writeSetCookie fills precisely that many bytes.
    // A maximal cookie exercises every branch of BOTH passes at once, so any
    // divergence between the size computed and the bytes written (an over- or
    // under-allocation, or a dropped attribute in one pass only) surfaces as a
    // wrong output string. Case A: __Host- prefix with all string/flag attributes
    // and a Max-Age (no Domain/Expires -- __Host- forbids Domain).
    ruvia::CookieOptions host;
    host.prefix = ruvia::CookiePrefix::kHost;
    host.secure = true;
    host.path = "/";
    host.httpOnly = true;
    host.sameSite = ruvia::CookieSameSite::kStrict;
    host.maxAge = std::chrono::seconds(3600);
    host.priority = ruvia::CookiePriority::kHigh;
    host.partitioned = true;
    context.setCookie("id", "abc", host);
    const auto hostResponse = context.text("ok");
    RUVIA_CHECK_EQ(hostResponse.header("Set-Cookie"), std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                                                                       "SameSite=Strict; Priority=High; Partitioned"));

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    ruvia::CookieOptions secure;
    secure.prefix = ruvia::CookiePrefix::kSecure;
    secure.secure = true;
    secure.path = "/app";
    secure.domain = "example.com";
    secure.sameSite = ruvia::CookieSameSite::kNone;
    secure.expires = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890));
    HttpRequest secureRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(secureRequest);
    RequestMemory secureMemory(worker);
    HttpRequestAccess::setResource(secureRequest, secureMemory.resource());
    auto secureContext = ContextAccess::make(secureMemory, secureRequest);
    secureContext.setCookie("sess", "xyz", secure);
    const auto secureResponse = secureContext.text("ok");
    RUVIA_CHECK_EQ(secureResponse.header("Set-Cookie"), std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                                                                         "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
}

RUVIA_TEST(context_signed_cookie_with_prefix_verifies_round_trip) {
    WorkerMemory worker;
    HttpRequest writeRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(writeRequest);
    RequestMemory writeMemory(worker);
    HttpRequestAccess::setResource(writeRequest, writeMemory.resource());
    auto writeContext = ContextAccess::make(writeMemory, writeRequest);

    ruvia::CookieOptions options;
    options.prefix = ruvia::CookiePrefix::kHost;
    options.secure = true;
    options.path = "/";
    writeContext.setSignedCookie("session", "user-1", "secret", options);
    const auto writeResponse = writeContext.text("ok");
    const std::string setCookie(writeResponse.header("Set-Cookie").value_or(std::string_view{}));
    const std::string_view line(setCookie);
    const auto pair = line.substr(0, line.find(';'));
    RUVIA_CHECK(pair.starts_with("__Host-session="));

    // Present the cookie exactly as a browser sends it back.
    HttpRequest readRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(readRequest);
    const std::string cookieField(pair);
    RUVIA_CHECK(HttpRequestAccess::addHeader(readRequest, HttpHeaderView{"Cookie", cookieField}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory readMemory(worker);
    HttpRequestAccess::setResource(readRequest, readMemory.resource());
    auto readContext = ContextAccess::make(readMemory, readRequest);

    const auto verified = readContext.req().signedCookie("__Host-session", "secret");
    RUVIA_CHECK(verified.has_value());
    RUVIA_CHECK_EQ(*verified, std::string_view("user-1"));

    // An unprefixed signed cookie keeps verifying under its own name.
    HttpRequest bareWriteRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(bareWriteRequest);
    RequestMemory bareWriteMemory(worker);
    HttpRequestAccess::setResource(bareWriteRequest, bareWriteMemory.resource());
    auto bareWriteContext = ContextAccess::make(bareWriteMemory, bareWriteRequest);
    bareWriteContext.setSignedCookie("plain", "user-2", "secret");
    const auto bareWriteResponse = bareWriteContext.text("ok");
    const std::string bare(bareWriteResponse.header("Set-Cookie").value_or(std::string_view{}));
    const std::string_view bareLine(bare);
    const std::string bareField(bareLine.substr(0, bareLine.find(';')));
    HttpRequest bareRequest = HttpRequestAccess::make();
    HttpRequestAccess::reset(bareRequest);
    RUVIA_CHECK(HttpRequestAccess::addHeader(bareRequest, HttpHeaderView{"Cookie", bareField}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory bareMemory(worker);
    HttpRequestAccess::setResource(bareRequest, bareMemory.resource());
    auto bareContext = ContextAccess::make(bareMemory, bareRequest);
    const auto bareVerified = bareContext.req().signedCookie("plain", "secret");
    RUVIA_CHECK(bareVerified.has_value());
    RUVIA_CHECK_EQ(*bareVerified, std::string_view("user-2"));
}

RUVIA_TEST(context_delete_cookie_with_prefix_is_response_only) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "__Host-session=user-1"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    ruvia::CookieOptions options;
    options.prefix = ruvia::CookiePrefix::kHost;
    options.secure = true;
    options.path = "/";
    const auto previous = context.req().cookie("__Host-session");
    RUVIA_CHECK(previous.has_value());
    RUVIA_CHECK_EQ(*previous, std::string_view("user-1"));
    context.deleteCookie("session", options);
    const auto response = context.text("deleted");
    const auto setCookie = response.header("Set-Cookie");
    RUVIA_CHECK(setCookie.has_value());
    RUVIA_CHECK(setCookie.value_or(std::string_view{}).starts_with("__Host-session=;"));
    RUVIA_CHECK(setCookie.value_or(std::string_view{}).find("Max-Age=0") != std::string_view::npos);
}
