#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int failures = 0;

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
void exerciseGenerateCookieSerialization(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
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
    const auto cookie = context.generateCookie("chip", "value", options);
    check(std::string_view(cookie) ==
        "__Host-chip=value; Path=/; Max-Age=3600; Expires=Sun, 04 Jan 1970 00:00:00 GMT; "
        "HttpOnly; Secure; SameSite=None; Priority=High; Partitioned");
}

void exerciseSetCookieMatchesGenerate(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    ruvia::CookieOptions options;
    options.httpOnly = true;
    options.sameSite = ruvia::CookieSameSite::kLax;
    context.setCookie("session", "id", options);
    auto response = context.text("hi");
    check(response.header("Set-Cookie") == std::string_view(context.generateCookie("session", "id", options)));
}

void exerciseCookieValidationThrows(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("bad;name", "v"); });
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("name", "va;lue"); });

    ruvia::CookieOptions hostWithoutSecure;
    hostWithoutSecure.prefix = ruvia::CookiePrefix::kHost;
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", hostWithoutSecure); });

    ruvia::CookieOptions hostWithDomain;
    hostWithDomain.prefix = ruvia::CookiePrefix::kHost;
    hostWithDomain.secure = true;
    hostWithDomain.domain = "example.com";
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", hostWithDomain); });

    ruvia::CookieOptions securePrefixWithoutSecure;
    securePrefixWithoutSecure.prefix = ruvia::CookiePrefix::kSecure;
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", securePrefixWithoutSecure); });

    ruvia::CookieOptions partitionedWithoutSecure;
    partitionedWithoutSecure.partitioned = true;
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", partitionedWithoutSecure); });

    ruvia::CookieOptions maxAgeTooLong;
    maxAgeTooLong.maxAge = std::chrono::seconds(34560001);
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", maxAgeTooLong); });

    ruvia::CookieOptions negativeMaxAge;
    negativeMaxAge.maxAge = std::chrono::seconds(-1);
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", negativeMaxAge); });

    ruvia::CookieOptions expiresTooFar;
    expiresTooFar.expires = std::chrono::system_clock::now() + std::chrono::hours(24 * 401);
    checkThrowsInvalidArgument([&] { (void)context.generateCookie("n", "v", expiresTooFar); });

    checkThrowsInvalidArgument([&] { (void)context.generateSignedCookie("n", "v", ""); });
}

void exerciseSignedCookieRoundtrip(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    constexpr std::string_view kSecret = "guard-secret";
    auto writer = ruvia::detail::ContextAccess::make(memory, request);
    const auto generated = writer.generateSignedCookie("sid", "hello", kSecret);
    writer.setSignedCookie("sid", "hello", kSecret);
    auto response = writer.text("x");
    check(response.header("Set-Cookie") == std::string_view(generated));

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

void exerciseDeleteCookieReturnsRequestValue(ruvia::RequestMemory& memory) {
    std::string raw("GET / HTTP/1.1\r\nHost: guard\r\nCookie: legacy=old\r\n\r\n");
    const auto parsed = parsePublicRequest(raw);
    auto context = ruvia::detail::ContextAccess::make(memory, parsed);
    const auto deleted = context.deleteCookie("legacy");
    check(deleted.has_value() && *deleted == "old");
    auto response = context.text("x");
    const auto value = response.header("Set-Cookie");
    check(value.starts_with("legacy=;"));
    check(value.find("Max-Age=0") != std::string_view::npos);
}

void exerciseByteSpanBody(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    static constexpr std::array<std::byte, 3> bytes{
        std::byte{0x00},
        std::byte{0x41},
        std::byte{0xff}};
    constexpr ruvia::HttpHeaderView headers[] = {{"X-Bin", "1"}};
    auto response = context.body(
        std::span<const std::byte>(bytes),
        206,
        headers);
    check(response.status() == 206);
    check(response.header("X-Bin") == "1");
    check(response.header("Content-Type").empty());
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

    exerciseGenerateCookieSerialization(memory, request);
    exerciseSetCookieMatchesGenerate(memory, request);
    exerciseCookieValidationThrows(memory, request);
    exerciseSignedCookieRoundtrip(memory, request);
    exerciseDeleteCookieReturnsRequestValue(memory);
    exerciseByteSpanBody(memory, request);

    return failures;
}
