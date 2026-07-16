#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/SetCookiePlan.h"

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

template <typename Text>
concept CookiePathAccepts = requires(
    ruvia::CookieOptions& options,
    Text&& text) {
    options.path = std::forward<Text>(text);
};

template <typename Text>
concept CookieDomainAccepts = requires(
    ruvia::CookieOptions& options,
    Text&& text) {
    options.domain = std::forward<Text>(text);
};

template <typename Name, typename Value, typename Options>
concept CanConstructSetCookiePlan = requires(
    Name&& name,
    Value&& value,
    Options&& options) {
    ruvia::detail::SetCookiePlan(
        std::forward<Name>(name),
        std::forward<Value>(value),
        std::forward<Options>(options));
};

static_assert(CookiePathAccepts<std::string&>);
static_assert(CookieDomainAccepts<const std::string&>);
static_assert(CookiePathAccepts<std::pmr::string&>);
static_assert(CookieDomainAccepts<const std::pmr::string&>);
static_assert(!CookiePathAccepts<std::string>);
static_assert(!CookiePathAccepts<const std::string>);
static_assert(!CookieDomainAccepts<std::string>);
static_assert(!CookieDomainAccepts<const std::string>);
static_assert(!CookiePathAccepts<std::pmr::string>);
static_assert(!CookieDomainAccepts<std::pmr::string>);
static_assert(CanConstructSetCookiePlan<
    std::string&,
    const std::string&,
    ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<
    std::string,
    std::string_view,
    ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<
    std::string_view,
    const std::string,
    ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<
    std::pmr::string,
    std::string_view,
    ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<
    std::string_view,
    std::string_view,
    ruvia::CookieOptions>);
static_assert(!CanConstructSetCookiePlan<
    std::string_view,
    std::string_view,
    const ruvia::CookieOptions>);

void check(bool condition) {
    if (!condition) {
        ++failures;
    }
}

[[nodiscard]] ruvia::HttpRequest makeRequest() {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    return parsed.request;
}

[[nodiscard]] ruvia::HttpRequest parsePublicRequest(std::string_view input) {
    const auto result = ruvia::Http1RequestParser().parse(input);
    if (const auto* parsed = result.parsed()) {
        return parsed->request();
    }
    ++failures;
    return makeRequest();
}

template <typename Callable>
void checkThrowsInvalidArgument(Callable&& callable) {
    try {
        callable();
        ++failures;
    } catch (const std::invalid_argument&) {
    } catch (...) {
        ++failures;
    }
}

// Full option serialization is deterministic: the fixed Expires renders a
// fixed IMF-fixdate and typed attributes have one canonical wire spelling.
void exerciseSetCookieSerialization(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    ruvia::CookieOptions options;
    options.secure = true;
    options.httpOnly = true;
    options.sameSite = ruvia::CookieSameSite::kNone;
    options.priority = ruvia::CookiePriority::kHigh;
    options.partitioned = true;
    options.prefix = ruvia::CookiePrefix::kHost;
    options.expires = std::chrono::system_clock::from_time_t(259200);
    options.maxAge = std::chrono::seconds(3600);
    context.setCookie("chip", "value", options);
    const auto response = context.text("ok");
    check(response.header("Set-Cookie") ==
        "__Host-chip=value; Path=/; Max-Age=3600; Expires=Sun, 04 Jan 1970 00:00:00 GMT; "
        "HttpOnly; Secure; SameSite=None; Priority=High; Partitioned");
}

void exerciseSetCookieWritesResponseHeader(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    ruvia::CookieOptions options;
    options.httpOnly = true;
    options.sameSite = ruvia::CookieSameSite::kLax;
    context.setCookie("session", "id", options);
    auto response = context.text("hi");
    check(response.header("Set-Cookie") ==
        "session=id; Path=/; HttpOnly; SameSite=Lax");
}

void exerciseCookieValidationThrows(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    checkThrowsInvalidArgument([&] { context.setCookie("bad;name", "v"); });
    checkThrowsInvalidArgument([&] { context.setCookie("name", "va;lue"); });

    ruvia::CookieOptions hostWithoutSecure;
    hostWithoutSecure.prefix = ruvia::CookiePrefix::kHost;
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", hostWithoutSecure); });

    ruvia::CookieOptions hostWithDomain;
    hostWithDomain.prefix = ruvia::CookiePrefix::kHost;
    hostWithDomain.secure = true;
    hostWithDomain.domain = "example.com";
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", hostWithDomain); });

    ruvia::CookieOptions securePrefixWithoutSecure;
    securePrefixWithoutSecure.prefix = ruvia::CookiePrefix::kSecure;
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", securePrefixWithoutSecure); });

    ruvia::CookieOptions partitionedWithoutSecure;
    partitionedWithoutSecure.partitioned = true;
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", partitionedWithoutSecure); });

    ruvia::CookieOptions maxAgeTooLong;
    maxAgeTooLong.maxAge = std::chrono::seconds(34560001);
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", maxAgeTooLong); });

    ruvia::CookieOptions negativeMaxAge;
    negativeMaxAge.maxAge = std::chrono::seconds(-1);
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", negativeMaxAge); });

    ruvia::CookieOptions expiresTooFar;
    expiresTooFar.expires = std::chrono::system_clock::now() + std::chrono::hours(24 * 401);
    checkThrowsInvalidArgument([&] { context.setCookie("n", "v", expiresTooFar); });

    checkThrowsInvalidArgument([&] { context.setSignedCookie("n", "v", ""); });
}

void exerciseSignedCookieRoundtrip(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    constexpr std::string_view kSecret = "guard-secret";
    auto writer = ruvia::detail::ContextAccess::make(memory, request);
    writer.setSignedCookie("sid", "hello", kSecret);
    auto response = writer.text("x");
    const std::string generated(
        response.header("Set-Cookie").value_or(std::string_view{}));
    const auto generatedView = std::string_view(generated);
    const auto cookiePair = generatedView.substr(0, generatedView.find(';'));

    std::string raw("GET / HTTP/1.1\r\nHost: guard\r\nCookie: ");
    raw.append(cookiePair);
    raw.append("\r\n\r\n");
    const auto parsed = parsePublicRequest(raw);
    auto reader = ruvia::detail::ContextAccess::make(memory, parsed);
    const auto verified = reader.req().signedCookie("sid", kSecret);
    check(verified.has_value() && *verified == "hello");
    check(!reader.req().signedCookie("sid", "other-secret").has_value());
    check(!reader.req().signedCookie("missing", kSecret).has_value());

    // Flip one signature character: the constant-time compare must reject it.
    std::string tampered("GET / HTTP/1.1\r\nHost: guard\r\nCookie: ");
    std::string tamperedPair(cookiePair);
    auto& lastChar = tamperedPair[tamperedPair.size() - 2];
    lastChar = lastChar == 'A' ? 'B' : 'A';
    tampered.append(tamperedPair);
    tampered.append("\r\n\r\n");
    const auto tamperedParsed = parsePublicRequest(tampered);
    auto tamperedReader = ruvia::detail::ContextAccess::make(memory, tamperedParsed);
    check(!tamperedReader.req().signedCookie("sid", kSecret).has_value());

    // A cookie without the fixed-width signature suffix is rejected, not parsed.
    std::string malformed("GET / HTTP/1.1\r\nHost: guard\r\nCookie: sid=no-signature\r\n\r\n");
    const auto malformedParsed = parsePublicRequest(malformed);
    auto malformedReader = ruvia::detail::ContextAccess::make(memory, malformedParsed);
    check(!malformedReader.req().signedCookie("sid", kSecret).has_value());
}

void exerciseDeleteCookieUsesRequestFacadeForPreviousValue(ruvia::RequestMemory& memory) {
    std::string raw("GET / HTTP/1.1\r\nHost: guard\r\nCookie: legacy=old\r\n\r\n");
    const auto parsed = parsePublicRequest(raw);
    auto context = ruvia::detail::ContextAccess::make(memory, parsed);
    const auto previous = context.req().cookie("legacy");
    check(previous.has_value() && *previous == "old");
    context.deleteCookie("legacy");
    auto response = context.text("x");
    const auto value = response.header("Set-Cookie");
    check(value.has_value());
    check(value->starts_with("legacy=;"));
    check(value->find("Max-Age=0") != std::string_view::npos);
}

void exerciseByteSpanBody(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    static constexpr std::array<std::byte, 3> bytes{
        std::byte{0x00},
        std::byte{0x41},
        std::byte{0xff}};
    context.status(ruvia::http_status::kPartialContent);
    context.header("X-Bin", "1");
    auto response = context.body(std::span<const std::byte>(bytes));
    check(response.status() == ruvia::http_status::kPartialContent);
    check(response.header("X-Bin") == "1");
    check(!response.header("Content-Type").has_value());
    const auto body = ruvia::detail::responseBody(response).bytes();
    check(body.size() == 3);
    check(body.size() == 3 && body[0] == '\0' && body[1] == 'A' &&
        static_cast<unsigned char>(body[2]) == 0xff);
}

}  // namespace

int main() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest();

    exerciseSetCookieSerialization(memory, request);
    exerciseSetCookieWritesResponseHeader(memory, request);
    exerciseCookieValidationThrows(memory, request);
    exerciseSignedCookieRoundtrip(memory, request);
    exerciseDeleteCookieUsesRequestFacadeForPreviousValue(memory);
    exerciseByteSpanBody(memory, request);

    return failures;
}
