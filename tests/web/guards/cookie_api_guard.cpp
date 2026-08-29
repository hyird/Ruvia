#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/cookie/SetCookiePlan.h"

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "context_services_fixture.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

int failures = 0;

template <typename Text>
concept CookiePathAccepts = requires(
    ruvia::CookieOptions& options, Text&& text) { options.path = std::forward<Text>(text); };

template <typename Text>
concept CookieDomainAccepts = requires(
    ruvia::CookieOptions& options, Text&& text) { options.domain = std::forward<Text>(text); };

template <typename Name, typename Value, typename Options>
concept CanConstructSetCookiePlan = requires(Name&& name, Value&& value, Options&& options) {
    ruvia::detail::SetCookiePlan(
        std::forward<Name>(name), std::forward<Value>(value), std::forward<Options>(options));
};

template <typename Context>
concept HasContextSetCookiePositional = requires(Context& context) {
    context.setCookie(std::string_view{}, std::string_view{});
} || requires(Context& context, const ruvia::CookieOptions& options) {
    context.setCookie(std::string_view{}, std::string_view{}, options);
};

template <typename Context>
concept HasContextSetSignedCookiePositional = requires(Context& context) {
    context.setSignedCookie(std::string_view{}, std::string_view{}, std::string_view{});
} || requires(Context& context, const ruvia::CookieOptions& options) {
    context.setSignedCookie(std::string_view{}, std::string_view{}, std::string_view{}, options);
};

template <typename Context>
concept HasContextDeleteCookiePositional = requires(Context& context) {
    context.deleteCookie(std::string_view{});
} || requires(Context& context, ruvia::CookieOptions options) {
    context.deleteCookie(std::string_view{}, options);
};

template <typename Request>
concept HasContextRequestSignedCookiePositional = requires(
    const Request& request) { request.signedCookie(std::string_view{}, std::string_view{}); };

template <typename String>
concept AcceptsAnyRvalueSetCookieOptionText = requires(String&& value) {
    ruvia::SetCookieOptions{.name = std::forward<String>(value), .value = "value"};
} || requires(String&& value) {
    ruvia::SetCookieOptions{.name = "name", .value = std::forward<String>(value)};
};

template <typename String>
concept AcceptsAnyRvalueSetSignedCookieOptionText = requires(String&& value) {
    ruvia::SetSignedCookieOptions{
        .name = std::forward<String>(value), .value = "value", .secret = "secret"};
} || requires(String&& value) {
    ruvia::SetSignedCookieOptions{
        .name = "name", .value = std::forward<String>(value), .secret = "secret"};
} || requires(String&& value) {
    ruvia::SetSignedCookieOptions{
        .name = "name", .value = "value", .secret = std::forward<String>(value)};
};

template <typename String>
concept AcceptsAnyRvalueDeleteCookieOptionText =
    requires(String&& value) { ruvia::DeleteCookieOptions{.name = std::forward<String>(value)}; };

template <typename String>
concept AcceptsAnyRvalueSignedCookieLookupOptionText = requires(String&& value) {
    ruvia::SignedCookieLookupOptions{.name = std::forward<String>(value), .secret = "secret"};
} || requires(String&& value) {
    ruvia::SignedCookieLookupOptions{.name = "name", .secret = std::forward<String>(value)};
};

template <typename T>
concept HasCookieHttpOnlyBoolean = requires(T& options) { options.httpOnly = true; };

template <typename T>
concept HasCookieSecureBoolean = requires(T& options) { options.secure = true; };

template <typename T>
concept HasCookiePartitionedBoolean = requires(T& options) { options.partitioned = true; };

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
static_assert(CanConstructSetCookiePlan<std::string&, const std::string&, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::string, std::string_view, ruvia::CookieOptions&>);
static_assert(
    !CanConstructSetCookiePlan<std::string_view, const std::string, ruvia::CookieOptions&>);
static_assert(
    !CanConstructSetCookiePlan<std::pmr::string, std::string_view, ruvia::CookieOptions&>);
static_assert(!CanConstructSetCookiePlan<std::string_view, std::string_view, ruvia::CookieOptions>);
static_assert(
    !CanConstructSetCookiePlan<std::string_view, std::string_view, const ruvia::CookieOptions>);
static_assert(std::is_aggregate_v<ruvia::SetCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::SetSignedCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::DeleteCookieOptions>);
static_assert(std::is_aggregate_v<ruvia::SignedCookieLookupOptions>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.value), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetCookieOptions{}.attributes), ruvia::CookieOptions>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.value), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SetSignedCookieOptions{}.secret), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::DeleteCookieOptions{}.name), ruvia::BorrowedText>);
static_assert(std::same_as<decltype(ruvia::SignedCookieLookupOptions{}.name), ruvia::BorrowedText>);
static_assert(
    std::same_as<decltype(ruvia::SignedCookieLookupOptions{}.secret), ruvia::BorrowedText>);
static_assert(!HasContextSetCookiePositional<ruvia::Context>);
static_assert(!HasContextSetSignedCookiePositional<ruvia::Context>);
static_assert(!HasContextDeleteCookiePositional<ruvia::Context>);
static_assert(!HasContextRequestSignedCookiePositional<ruvia::ContextRequest>);
static_assert(!AcceptsAnyRvalueSetCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSetCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueSetSignedCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSetSignedCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueDeleteCookieOptionText<std::string>);
static_assert(!AcceptsAnyRvalueDeleteCookieOptionText<std::pmr::string>);
static_assert(!AcceptsAnyRvalueSignedCookieLookupOptionText<std::string>);
static_assert(!AcceptsAnyRvalueSignedCookieLookupOptionText<std::pmr::string>);
static_assert(
    std::same_as<decltype(ruvia::CookieOptions{}.httpOnly), ruvia::CookieAttributePolicy>);
static_assert(std::same_as<decltype(ruvia::CookieOptions{}.secure), ruvia::CookieAttributePolicy>);
static_assert(
    std::same_as<decltype(ruvia::CookieOptions{}.partitioned), ruvia::CookieAttributePolicy>);
static_assert(!HasCookieHttpOnlyBoolean<ruvia::CookieOptions>);
static_assert(!HasCookieSecureBoolean<ruvia::CookieOptions>);
static_assert(!HasCookiePartitionedBoolean<ruvia::CookieOptions>);

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
void exerciseSetCookieSerialization(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    const ruvia::CookieOptions options{
        .sameSite = ruvia::CookieSameSite::kNone,
        .priority = ruvia::CookiePriority::kHigh,
        .expires = std::chrono::system_clock::from_time_t(259200),
        .maxAge = std::chrono::seconds(3600),
        .prefix = ruvia::CookiePrefix::kHost,
        .httpOnly = ruvia::CookieAttributePolicy::kEmit,
        .secure = ruvia::CookieAttributePolicy::kEmit,
        .partitioned = ruvia::CookieAttributePolicy::kEmit,
    };
    context.setCookie({.name = "chip", .value = "value", .attributes = options});
    const auto response = context.text("ok");
    check(response.header("Set-Cookie") ==
          "__Host-chip=value; Path=/; Max-Age=3600; Expires=Sun, 04 Jan 1970 00:00:00 GMT; "
          "HttpOnly; Secure; SameSite=None; Priority=High; Partitioned");
}

void exerciseSetCookieWritesResponseHeader(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    const ruvia::CookieOptions options{
        .sameSite = ruvia::CookieSameSite::kLax,
        .httpOnly = ruvia::CookieAttributePolicy::kEmit,
    };
    context.setCookie({.name = "session", .value = "id", .attributes = options});
    auto response = context.text("hi");
    check(response.header("Set-Cookie") == "session=id; Path=/; HttpOnly; SameSite=Lax");
}

void exerciseCookieValidationThrows(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    checkThrowsInvalidArgument([&] { context.setCookie({.name = "bad;name", .value = "v"}); });
    checkThrowsInvalidArgument([&] { context.setCookie({.name = "name", .value = "va;lue"}); });

    const ruvia::CookieOptions hostWithoutSecure{
        .prefix = ruvia::CookiePrefix::kHost,
    };
    checkThrowsInvalidArgument(
        [&] { context.setCookie({.name = "n", .value = "v", .attributes = hostWithoutSecure}); });

    ruvia::CookieOptions hostWithDomain;
    hostWithDomain.prefix = ruvia::CookiePrefix::kHost;
    hostWithDomain.secure = ruvia::CookieAttributePolicy::kEmit;
    hostWithDomain.domain = "example.com";
    checkThrowsInvalidArgument(
        [&] { context.setCookie({.name = "n", .value = "v", .attributes = hostWithDomain}); });

    const ruvia::CookieOptions securePrefixWithoutSecure{
        .prefix = ruvia::CookiePrefix::kSecure,
    };
    checkThrowsInvalidArgument([&] {
        context.setCookie({.name = "n", .value = "v", .attributes = securePrefixWithoutSecure});
    });

    const ruvia::CookieOptions partitionedWithoutSecure{
        .partitioned = ruvia::CookieAttributePolicy::kEmit,
    };
    checkThrowsInvalidArgument([&] {
        context.setCookie({.name = "n", .value = "v", .attributes = partitionedWithoutSecure});
    });

    ruvia::CookieOptions maxAgeTooLong;
    maxAgeTooLong.maxAge = std::chrono::seconds(34560001);
    checkThrowsInvalidArgument(
        [&] { context.setCookie({.name = "n", .value = "v", .attributes = maxAgeTooLong}); });

    ruvia::CookieOptions negativeMaxAge;
    negativeMaxAge.maxAge = std::chrono::seconds(-1);
    checkThrowsInvalidArgument(
        [&] { context.setCookie({.name = "n", .value = "v", .attributes = negativeMaxAge}); });

    ruvia::CookieOptions expiresTooFar;
    expiresTooFar.expires = std::chrono::system_clock::now() + std::chrono::hours(24 * 401);
    checkThrowsInvalidArgument(
        [&] { context.setCookie({.name = "n", .value = "v", .attributes = expiresTooFar}); });

    checkThrowsInvalidArgument(
        [&] { context.setSignedCookie({.name = "n", .value = "v", .secret = ""}); });
}

void exerciseSignedCookieRoundtrip(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    constexpr std::string_view kSecret = "guard-secret";
    auto writer =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    writer.setSignedCookie({.name = "sid", .value = "hello", .secret = kSecret});
    auto response = writer.text("x");
    const std::string generated(response.header("Set-Cookie").value_or(std::string_view{}));
    const auto generatedView = std::string_view(generated);
    const auto cookiePair = generatedView.substr(0, generatedView.find(';'));

    std::string raw("GET / HTTP/1.1\r\nHost: guard\r\nCookie: ");
    raw.append(cookiePair);
    raw.append("\r\n\r\n");
    const auto parsed = parsePublicRequest(raw);
    auto reader =
        ruvia::detail::ContextAccess::make(memory, parsed, ruvia::test::testContextServices());
    const auto verified = reader.req().signedCookie({.name = "sid", .secret = kSecret});
    check(verified.has_value() && *verified == "hello");
    check(!reader.req().signedCookie({.name = "sid", .secret = "other-secret"}).has_value());
    check(!reader.req().signedCookie({.name = "missing", .secret = kSecret}).has_value());

    // Flip one signature character: the constant-time compare must reject it.
    std::string tampered("GET / HTTP/1.1\r\nHost: guard\r\nCookie: ");
    std::string tamperedPair(cookiePair);
    auto& lastChar = tamperedPair[tamperedPair.size() - 2];
    lastChar = lastChar == 'A' ? 'B' : 'A';
    tampered.append(tamperedPair);
    tampered.append("\r\n\r\n");
    const auto tamperedParsed = parsePublicRequest(tampered);
    auto tamperedReader = ruvia::detail::ContextAccess::make(
        memory, tamperedParsed, ruvia::test::testContextServices());
    check(!tamperedReader.req().signedCookie({.name = "sid", .secret = kSecret}).has_value());

    // A cookie without the fixed-width signature suffix is rejected, not parsed.
    std::string malformed("GET / HTTP/1.1\r\nHost: guard\r\nCookie: sid=no-signature\r\n\r\n");
    const auto malformedParsed = parsePublicRequest(malformed);
    auto malformedReader = ruvia::detail::ContextAccess::make(
        memory, malformedParsed, ruvia::test::testContextServices());
    check(!malformedReader.req().signedCookie({.name = "sid", .secret = kSecret}).has_value());
}

void exerciseDeleteCookieUsesRequestFacadeForPreviousValue(ruvia::RequestMemory& memory) {
    std::string raw("GET / HTTP/1.1\r\nHost: guard\r\nCookie: legacy=old\r\n\r\n");
    const auto parsed = parsePublicRequest(raw);
    auto context =
        ruvia::detail::ContextAccess::make(memory, parsed, ruvia::test::testContextServices());
    const auto previous = context.req().cookie("legacy");
    check(previous.has_value() && *previous == "old");
    context.deleteCookie({.name = "legacy"});
    auto response = context.text("x");
    const auto value = response.header("Set-Cookie");
    check(value.has_value());
    check(value->starts_with("legacy=;"));
    check(value->find("Max-Age=0") != std::string_view::npos);
}

void exerciseByteSpanBody(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    static constexpr std::array<std::byte, 3> bytes{
        std::byte{0x00}, std::byte{0x41}, std::byte{0xff}};
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
