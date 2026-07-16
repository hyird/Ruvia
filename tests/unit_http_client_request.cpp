#include "test_harness.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::Http1ClientRequestClosePolicy;
using ruvia::Http1ClientRequestPrepareError;
using ruvia::Http1ClientRequestWirePolicy;
using ruvia::Http1ClientRequestWriter;
using ruvia::HttpClientRequest;
using ruvia::HttpClientRequestContent;
using ruvia::HttpOrigin;

static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&>()
        .connectionOptions()),
    ruvia::detail::HttpConnectionOptions>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&&>()
        .connectionOptions()),
    ruvia::detail::HttpConnectionOptions>);

template <typename T>
concept HasAnyRvalueHttpClientRequestContentAccessor =
    requires(T&& content) { std::move(content).withoutContent(); } ||
    requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor =
    requires(T&& policy) { std::move(policy).noExpectation(); } ||
    requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor =
    requires(T&& plan) { std::move(plan).withoutContent(); } ||
    requires(T&& plan) { std::move(plan).immediate(); } ||
    requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor =
    requires(T&& result) { std::move(result).bufferTooSmall(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasRvaluePreparedHttp1ClientRequestContentPlan =
    requires(T&& prepared) { std::move(prepared).contentPlan(); };

template <typename String>
concept AcceptsAnyTemporaryHttpClientRequestText =
    requires(String&& value) {
        HttpClientRequest{.method = std::forward<String>(value)};
    } ||
    requires(String&& value) {
        HttpClientRequest{.target = std::forward<String>(value)};
    } ||
    requires(HttpClientRequest& request, String&& value) {
        request.method = std::forward<String>(value);
    } ||
    requires(HttpClientRequest& request, String&& value) {
        request.target = std::forward<String>(value);
    };

template <typename String>
concept AcceptsLvalueHttpClientRequestText =
    requires(HttpClientRequest& request, String& value) {
        HttpClientRequest{.method = value, .target = value};
        request.method = value;
        request.target = value;
    };

static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<
    ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttpClientRequestContentAccessor<
    ruvia::HttpClientRequestContent>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<
    ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasRvaluePreparedHttp1ClientRequestContentPlan<
    ruvia::PreparedHttp1ClientRequest>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<const std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestText<std::pmr::string>);
static_assert(AcceptsLvalueHttpClientRequestText<std::string>);
constexpr HttpClientRequest kLiteralHttpClientRequest{
    .method = "POST",
    .target = "/items"};
static_assert(kLiteralHttpClientRequest.method.view() == "POST");
static_assert(kLiteralHttpClientRequest.target.view() == "/items");
static_assert(kLiteralHttpClientRequest.method == "POST");
static_assert("/items" == kLiteralHttpClientRequest.target);
static_assert(!std::is_constructible_v<
    HttpClientRequest::HeaderInit,
    std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::is_assignable_v<
    HttpClientRequest::HeaderInit&,
    std::array<ruvia::HttpHeaderView, 1>&&>);

template <typename T>
concept HasRequestContentMode = requires(const T& content) {
    content.mode();
};

template <typename T>
concept HasRequestContentValue = requires(const T& content) {
    { content.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasPreparedContentDisposition = requires(const T& plan) {
    plan.disposition();
};

template <typename T>
concept HasPreparedContentBytes = requires(const T& content) {
    { content.bytes() } -> std::same_as<std::string_view>;
};

static_assert(!HasRequestContentMode<ruvia::HttpClientRequestContent>);
static_assert(!HasRequestContentValue<ruvia::HttpClientRequestContent>);
static_assert(!HasRequestContentValue<ruvia::HttpClientRequestWithoutContent>);
static_assert(HasRequestContentValue<ruvia::HttpClientRequestBytes>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytes>);
static_assert(!HasPreparedContentDisposition<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasPreparedContentBytes<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasPreparedContentBytes<ruvia::Http1ClientRequestWithoutContent>);
static_assert(HasPreparedContentBytes<
    ruvia::Http1ClientImmediateRequestContent>);
static_assert(HasPreparedContentBytes<
    ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientRequestContentPlan>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<
    ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!std::default_initializable<Http1ClientRequestWirePolicy>);
constexpr auto kWithoutExpectation =
    Http1ClientRequestWirePolicy::withoutExpectation();
constexpr auto kExpectContinue =
    Http1ClientRequestWirePolicy::expectContinue();
static_assert(kWithoutExpectation.noExpectation() != nullptr);
static_assert(kWithoutExpectation.continueExpectation() == nullptr);
static_assert(kExpectContinue.noExpectation() == nullptr);
static_assert(kExpectContinue.continueExpectation() != nullptr);

template <std::size_t N = 2048>
struct PreparedFixture final {
    std::array<char, N> buffer{};
    ruvia::Http1ClientRequestPrepareResult result;

    PreparedFixture(
        const HttpOrigin& origin,
        const HttpClientRequest& request,
        Http1ClientRequestWirePolicy policy =
            Http1ClientRequestWirePolicy::withoutExpectation())
        : result(Http1ClientRequestWriter().prepare(
              origin, request, buffer, policy)) {}
};

[[nodiscard]] Http1ClientRequestPrepareError prepareError(
    const HttpClientRequest& request) {
    std::array<char, 512> buffer;
    const auto result = Http1ClientRequestWriter().prepare(
        HttpOrigin::https("example.test"), request, buffer);
    const auto* failure = result.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected request preparation to fail");
    }
    return failure->error();
}

}  // namespace

RUVIA_TEST(http1_client_request_writer_emits_one_canonical_scatter_gather_plan) {
    const ruvia::HttpHeaderView headers[] = {
        {"X-Test", "one"},
    };
    HttpClientRequest request;
    request.method = "POST";
    request.target = "/items?q=1";
    request.headers = headers;
    request.content = HttpClientRequestContent::bytes("payload");

    PreparedFixture fixture(
        HttpOrigin::https("example.test"),
        request,
        Http1ClientRequestWirePolicy::expectContinue());
    const auto* prepared = fixture.result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(
        prepared->head() ==
        "POST /items?q=1 HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "X-Test: one\r\n"
        "Content-Length: 7\r\n"
        "Expect: 100-continue\r\n\r\n");
    const auto* gated = prepared->contentPlan().continueGated();
    RUVIA_CHECK(gated != nullptr);
    RUVIA_CHECK(prepared->contentPlan().withoutContent() == nullptr);
    RUVIA_CHECK(prepared->contentPlan().immediate() == nullptr);
    if (gated != nullptr) {
        RUVIA_CHECK(gated->bytes() == "payload");
    }
}

RUVIA_TEST(http1_client_request_content_distinguishes_absent_from_explicit_empty) {
    HttpClientRequest absent;
    absent.method = "GET";
    RUVIA_CHECK(absent.content.withoutContent() != nullptr);
    RUVIA_CHECK(absent.content.borrowedBytes() == nullptr);
    PreparedFixture absentFixture(HttpOrigin::https("example.test"), absent);
    const auto* absentPrepared = absentFixture.result.prepared();
    RUVIA_CHECK(absentPrepared != nullptr);
    if (absentPrepared != nullptr) {
        RUVIA_CHECK(
            absentPrepared->contentPlan().withoutContent() != nullptr);
        RUVIA_CHECK(absentPrepared->contentPlan().immediate() == nullptr);
        RUVIA_CHECK(absentPrepared->contentPlan().continueGated() == nullptr);
        RUVIA_CHECK(absentPrepared->head().find("Content-Length") ==
                    std::string_view::npos);
    }

    HttpClientRequest empty;
    empty.method = "POST";
    empty.content = HttpClientRequestContent::bytes("");
    RUVIA_CHECK(empty.content.withoutContent() == nullptr);
    RUVIA_CHECK(empty.content.borrowedBytes() != nullptr);
    if (const auto* bytes = empty.content.borrowedBytes()) {
        RUVIA_CHECK(bytes->value().empty());
    }
    PreparedFixture emptyFixture(HttpOrigin::https("example.test"), empty);
    const auto* emptyPrepared = emptyFixture.result.prepared();
    RUVIA_CHECK(emptyPrepared != nullptr);
    if (emptyPrepared != nullptr) {
        const auto* immediate = emptyPrepared->contentPlan().immediate();
        RUVIA_CHECK(immediate != nullptr);
        RUVIA_CHECK(emptyPrepared->contentPlan().withoutContent() == nullptr);
        RUVIA_CHECK(emptyPrepared->contentPlan().continueGated() == nullptr);
        if (immediate != nullptr) {
            RUVIA_CHECK(immediate->bytes().empty());
        }
        RUVIA_CHECK(emptyPrepared->head().find("Content-Length: 0\r\n") !=
                    std::string_view::npos);
    }
}

RUVIA_TEST(http1_client_request_writer_owns_request_target_forms_and_host) {
    HttpClientRequest extension;
    extension.method = "PROPFIND";
    extension.target = "/dav";
    PreparedFixture extensionFixture(
        HttpOrigin::https("example.test", 8443), extension);
    const auto* extensionPrepared = extensionFixture.result.prepared();
    RUVIA_CHECK(extensionPrepared != nullptr);
    if (extensionPrepared != nullptr) {
        RUVIA_CHECK(extensionPrepared->head().starts_with(
            "PROPFIND /dav HTTP/1.1\r\nHost: example.test:8443\r\n"));
    }

    HttpClientRequest options;
    options.method = "OPTIONS";
    options.target = "*";
    PreparedFixture optionsFixture(HttpOrigin::http("example.test"), options);
    RUVIA_CHECK(optionsFixture.result.prepared() != nullptr);

    HttpClientRequest invalidAsterisk;
    invalidAsterisk.method = "GET";
    invalidAsterisk.target = "*";
    RUVIA_CHECK(
        prepareError(invalidAsterisk) ==
        Http1ClientRequestPrepareError::kInvalidTarget);

    HttpClientRequest absolute;
    absolute.target = "https://example.test/path";
    RUVIA_CHECK(
        prepareError(absolute) ==
        Http1ClientRequestPrepareError::kInvalidTarget);

    HttpClientRequest connect;
    connect.method = "CONNECT";
    connect.target = "example.test:443";
    RUVIA_CHECK(
        prepareError(connect) ==
        Http1ClientRequestPrepareError::kConnectRequiresDedicatedEntry);
}

RUVIA_TEST(http1_client_connect_entry_generates_authority_form_atomically) {
    std::array<char, 512> buffer;
    const auto result = Http1ClientRequestWriter().prepareConnect(
        HttpOrigin::https("example.test"), {}, buffer);
    const auto* prepared = result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK(
            prepared->head() ==
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test\r\n\r\n");
        RUVIA_CHECK(prepared->contentPlan().withoutContent() != nullptr);
    }

    std::array<char, 512> ipv6Buffer;
    const auto ipv6 = Http1ClientRequestWriter().prepareConnect(
        HttpOrigin::https("[::1]"), {}, ipv6Buffer);
    RUVIA_CHECK(ipv6.prepared() != nullptr);
    if (ipv6.prepared() != nullptr) {
        RUVIA_CHECK(ipv6.prepared()->head().starts_with(
            "CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]\r\n"));
    }

    std::array<char, 128> invalidBuffer;
    const auto invalid = Http1ClientRequestWriter().prepareConnect(
        HttpOrigin::https("example.test", 0), {}, invalidBuffer);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(
        invalid.failure()->error() ==
        Http1ClientRequestPrepareError::kInvalidConnectOrigin);
}

RUVIA_TEST(http1_client_request_writer_is_the_only_host_and_framing_owner) {
    struct Case final {
        std::string_view name;
        std::string_view value;
        Http1ClientRequestPrepareError error;
    };
    const Case cases[] = {
        {"Host", "other.test", Http1ClientRequestPrepareError::kHostHeaderManagedByWriter},
        {"Content-Length", "7", Http1ClientRequestPrepareError::kContentLengthManagedByWriter},
        {"Transfer-Encoding", "chunked", Http1ClientRequestPrepareError::kTransferEncodingUnsupported},
        {"Trailer", "Digest", Http1ClientRequestPrepareError::kTrailerSectionUnsupported},
        {"Expect", "100-continue", Http1ClientRequestPrepareError::kExpectHeaderManagedByWriter},
    };
    for (const auto& test : cases) {
        const ruvia::HttpHeaderView header(test.name, test.value);
        HttpClientRequest request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&header, 1);
        request.content = HttpClientRequestContent::bytes("payload");
        RUVIA_CHECK(prepareError(request) == test.error);
    }
}

RUVIA_TEST(http1_client_request_writer_owns_hop_by_hop_field_contracts) {
    struct FailureCase final {
        std::array<ruvia::HttpHeaderView, 2> headers;
        std::size_t count;
        Http1ClientRequestPrepareError error;
    };
    const FailureCase failures[] = {
        {.headers = {ruvia::HttpHeaderView("Connection", "close,"), {}},
         .count = 1,
         .error = Http1ClientRequestPrepareError::kInvalidConnection},
        {.headers = {
             ruvia::HttpHeaderView("Connection", "close;parameter"), {}},
         .count = 1,
         .error = Http1ClientRequestPrepareError::kInvalidConnection},
        {.headers = {ruvia::HttpHeaderView("Upgrade", "websocket"), {}},
         .count = 1,
         .error =
             Http1ClientRequestPrepareError::kUpgradeConnectionOptionRequired},
        {.headers = {
             ruvia::HttpHeaderView("Connection", "Upgrade"),
             ruvia::HttpHeaderView("Upgrade", "websocket/")},
         .count = 2,
         .error = Http1ClientRequestPrepareError::kInvalidUpgrade},
        {.headers = {ruvia::HttpHeaderView("TE", "trailers"), {}},
         .count = 1,
         .error = Http1ClientRequestPrepareError::kTeConnectionOptionRequired},
    };
    for (const auto& test : failures) {
        HttpClientRequest request;
        request.headers = std::span<const ruvia::HttpHeaderView>(
            test.headers.data(), test.count);
        RUVIA_CHECK(prepareError(request) == test.error);
    }

    for (const std::string_view connectionOptions : {
             "Host",
             "close, content-length",
             "EXPECT, keep-alive",
             "Cache-Control",
             "Trailer"}) {
        const ruvia::HttpHeaderView connection(
            "Connection", connectionOptions);
        HttpClientRequest request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&connection, 1);
        request.content = HttpClientRequestContent::bytes("payload");
        RUVIA_CHECK(
            prepareError(request) ==
            Http1ClientRequestPrepareError::kInvalidConnection);
    }

    const ruvia::HttpHeaderView validHeaders[] = {
        {"Connection", "keep-alive"},
        {"Connection", "Upgrade, TE, X-Hop"},
        {"Upgrade", "custom/1, websocket"},
        {"TE", "trailers"},
        {"X-Hop", "value"},
    };
    HttpClientRequest valid;
    valid.headers = validHeaders;
    PreparedFixture fixture(HttpOrigin::https("example.test"), valid);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_validates_te_capabilities_and_weights) {
    const auto prepareWithTe = [&ruvia_ctx](std::string_view value)
        -> std::optional<Http1ClientRequestPrepareError> {
        const std::array headers{
            ruvia::HttpHeaderView("Connection", "TE"),
            ruvia::HttpHeaderView("TE", value),
        };
        HttpClientRequest request;
        request.headers = headers;
        std::array<char, 512> buffer;
        const auto result = Http1ClientRequestWriter().prepare(
            HttpOrigin::https("example.test"), request, buffer);
        if (const auto* failure = result.failure()) {
            return failure->error();
        }
        RUVIA_CHECK(result.prepared() != nullptr);
        return std::nullopt;
    };

    for (const std::string_view valid : {
             "",
             "trailers",
             "gzip",
             "deflate;q=0.5",
             "x-gzip ; q=1.000",
             "gzip;q=0, trailers"}) {
        RUVIA_CHECK(!prepareWithTe(valid).has_value());
    }

    for (const std::string_view invalid : {
             ",trailers",
             "trailers,",
             "trailers,,gzip",
             "chunked",
             "br",
             "trailers;q=0.5",
             "gzip;q=1.001",
             "gzip;q=\"0.5\"",
             "gzip;q =0.5",
             "gzip;q= 0.5",
             "gzip; q = 0.5",
             "gzip;level=1",
             "gzip;q=0.5;level=1",
             "gzip; q",
             "gzip;q="}) {
        RUVIA_CHECK(
            prepareWithTe(invalid) ==
            Http1ClientRequestPrepareError::kInvalidHeader);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_expect_content_semantics) {
    const ruvia::HttpHeaderView expect("Expect", "100-Continue");
    HttpClientRequest rawExpectation;
    rawExpectation.method = "POST";
    rawExpectation.headers = std::span<const ruvia::HttpHeaderView>(&expect, 1);
    rawExpectation.content = HttpClientRequestContent::bytes("x");
    RUVIA_CHECK(
        prepareError(rawExpectation) ==
        Http1ClientRequestPrepareError::kExpectHeaderManagedByWriter);

    HttpClientRequest absent;
    absent.method = "POST";
    PreparedFixture absentFixture(
        HttpOrigin::https("example.test"),
        absent,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(absentFixture.result.failure() != nullptr);
    if (absentFixture.result.failure() != nullptr) {
        RUVIA_CHECK(
            absentFixture.result.failure()->error() ==
            Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }

    HttpClientRequest empty = absent;
    empty.content = HttpClientRequestContent::bytes("");
    PreparedFixture emptyFixture(
        HttpOrigin::https("example.test"),
        empty,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(emptyFixture.result.failure() != nullptr);
    if (emptyFixture.result.failure() != nullptr) {
        RUVIA_CHECK(
            emptyFixture.result.failure()->error() ==
            Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }

    HttpClientRequest nonempty;
    nonempty.method = "POST";
    nonempty.content = HttpClientRequestContent::bytes("x");
    PreparedFixture fixture(
        HttpOrigin::https("example.test"),
        nonempty,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
    if (fixture.result.prepared() != nullptr) {
        RUVIA_CHECK(
            fixture.result.prepared()->contentPlan().continueGated() != nullptr);
        RUVIA_CHECK(fixture.result.prepared()->head().find(
            "Expect: 100-continue\r\n") != std::string_view::npos);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_method_content_semantics) {
    HttpClientRequest trace;
    trace.method = "TRACE";
    trace.content = HttpClientRequestContent::bytes("trace body");
    RUVIA_CHECK(
        prepareError(trace) ==
        Http1ClientRequestPrepareError::kContentForbiddenForMethod);

    HttpClientRequest options;
    options.method = "OPTIONS";
    options.content = HttpClientRequestContent::bytes("options body");
    RUVIA_CHECK(
        prepareError(options) ==
        Http1ClientRequestPrepareError::kOptionsContentTypeRequired);

    // Content-Length signals request content even when its value is zero. An
    // explicitly empty OPTIONS representation therefore has the same mandatory
    // Content-Type contract as a non-empty one.
    options.content = HttpClientRequestContent::bytes("");
    RUVIA_CHECK(
        prepareError(options) ==
        Http1ClientRequestPrepareError::kOptionsContentTypeRequired);

    const ruvia::HttpHeaderView contentType("Content-Type", "application/json");
    options.headers = std::span<const ruvia::HttpHeaderView>(&contentType, 1);
    PreparedFixture fixture(HttpOrigin::https("example.test"), options);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);

    const ruvia::HttpHeaderView invalidContentType("Content-Type", "invalid");
    options.headers = std::span<const ruvia::HttpHeaderView>(&invalidContentType, 1);
    RUVIA_CHECK(
        prepareError(options) ==
        Http1ClientRequestPrepareError::kInvalidHeader);
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_content_type_parameters) {
    for (const std::string_view value : {
             "text/plain; charset",
             "text/plain; charset=",
             "text/plain; charset =utf-8",
             "text/plain; charset=utf-8; CHARSET=latin1",
             "text/plain; charset=\"unterminated"}) {
        const ruvia::HttpHeaderView contentType("Content-Type", value);
        HttpClientRequest request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&contentType, 1);
        request.content = HttpClientRequestContent::bytes("body");
        RUVIA_CHECK(
            prepareError(request) ==
            Http1ClientRequestPrepareError::kInvalidHeader);
    }

    const ruvia::HttpHeaderView validContentType(
        "Content-Type", "text/plain; charset=\"utf-8\"");
    HttpClientRequest valid;
    valid.method = "POST";
    valid.headers =
        std::span<const ruvia::HttpHeaderView>(&validContentType, 1);
    valid.content = HttpClientRequestContent::bytes("body");
    PreparedFixture fixture(HttpOrigin::https("example.test"), valid);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output) {
    HttpClientRequest request;
    request.method = "POST";
    request.target = "/upload";
    request.content = HttpClientRequestContent::bytes("body");

    std::array<char, 8> small;
    small.fill('z');
    const auto tooSmall = Http1ClientRequestWriter().prepare(
        HttpOrigin::https("example.test"), request, small);
    RUVIA_CHECK(tooSmall.bufferTooSmall() != nullptr);
    RUVIA_CHECK(std::ranges::all_of(small, [](char value) { return value == 'z'; }));

    std::array<char, 512> enough;
    const auto prepared = Http1ClientRequestWriter().prepare(
        HttpOrigin::https("example.test"), request, enough);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() != nullptr && tooSmall.bufferTooSmall() != nullptr) {
        RUVIA_CHECK_EQ(
            tooSmall.bufferTooSmall()->requiredHeadBytes(),
            prepared.prepared()->head().size());
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_header_count_and_size_transactionally) {
    std::array<ruvia::HttpHeaderView, ruvia::kMaxHttpHeaderFields> headers;
    headers.fill(ruvia::HttpHeaderView("X-Test", "x"));
    HttpClientRequest tooMany;
    tooMany.headers = headers;
    RUVIA_CHECK(
        prepareError(tooMany) ==
        Http1ClientRequestPrepareError::kTooManyHeaders);

    HttpClientRequest oversized;
    std::string target(ruvia::kMaxHttpHeaderBytes, 'a');
    target.front() = '/';
    oversized.target = target;
    RUVIA_CHECK(
        prepareError(oversized) ==
        Http1ClientRequestPrepareError::kHeaderTooLarge);

    HttpClientRequest invalidMethod;
    invalidMethod.method = "GET\r";
    std::array<char, 128> untouched;
    untouched.fill('q');
    const auto invalid = Http1ClientRequestWriter().prepare(
        HttpOrigin::https("example.test"), invalidMethod, untouched);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(
        invalid.failure()->error() ==
        Http1ClientRequestPrepareError::kInvalidMethod);
    RUVIA_CHECK(std::ranges::all_of(
        untouched, [](char value) { return value == 'q'; }));
    RUVIA_CHECK(!ruvia::http1ClientRequestPrepareErrorMessage(
        Http1ClientRequestPrepareError::kInvalidMethod).empty());
}

RUVIA_TEST(http1_client_request_context_binds_the_actual_close_signal) {
    const ruvia::HttpHeaderView closeHeader("Connection", "close");
    HttpClientRequest request;
    request.headers = std::span<const ruvia::HttpHeaderView>(&closeHeader, 1);
    PreparedFixture explicitClose(HttpOrigin::https("example.test"), request);
    const auto* explicitPrepared = explicitClose.result.prepared();
    RUVIA_CHECK(explicitPrepared != nullptr);
    if (explicitPrepared != nullptr) {
        auto parser = ruvia::Http1ClientResponseParser(*explicitPrepared);
        const auto response = parser.parse(
            "HTTP/1.1 204 No Content\r\n\r\n");
        RUVIA_CHECK(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto* withoutContent =
                response.parsed()->plan().withoutContent();
            RUVIA_CHECK(withoutContent != nullptr);
            if (withoutContent != nullptr) {
                RUVIA_CHECK(
                    withoutContent->persistence() ==
                    ruvia::Http1ClientResponsePersistence::kClose);
            }
        }
    }

    HttpClientRequest generatedRequest;
    PreparedFixture generatedClose(
        HttpOrigin::https("example.test"),
        generatedRequest,
        Http1ClientRequestWirePolicy::withoutExpectation(
            Http1ClientRequestClosePolicy::kCloseAfterResponse));
    RUVIA_CHECK(generatedClose.result.prepared() != nullptr);
    if (generatedClose.result.prepared() != nullptr) {
        RUVIA_CHECK(generatedClose.result.prepared()->head().find(
            "Connection: close\r\n") != std::string_view::npos);
    }
}
