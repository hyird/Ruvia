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
#include <vector>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::Http1ClosePolicy;
using ruvia::Http1ClientRequestPrepareError;
using ruvia::Http1ClientRequestWirePolicy;
using ruvia::Http1ClientRequestWriter;
using ruvia::HttpClientRequestView;
using ruvia::HttpClientRequestContentView;
using ruvia::HttpOriginView;

static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&>().connectionOptions()), ruvia::detail::HttpConnectionOptions>);
static_assert(std::same_as<decltype(std::declval<const ruvia::detail::Http1ClientRequestContext&&>().connectionOptions()), ruvia::detail::HttpConnectionOptions>);

template <typename T>
concept HasAnyRvalueHttpClientRequestContentViewAccessor = requires(T&& content) { std::move(content).withoutContent(); } || requires(T&& content) { std::move(content).borrowedBytes(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestWirePolicyAccessor = requires(T&& policy) { std::move(policy).noExpectation(); } || requires(T&& policy) { std::move(policy).continueExpectation(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestContentPlanAccessor = requires(T&& plan) { std::move(plan).withoutContent(); } || requires(T&& plan) { std::move(plan).immediate(); } || requires(T&& plan) { std::move(plan).continueGated(); };

template <typename T>
concept HasAnyRvalueHttp1ClientRequestPrepareAccessor = requires(T&& result) { std::move(result).bufferTooSmall(); } || requires(T&& result) { std::move(result).prepared(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasRvaluePreparedHttp1ClientRequestContentPlan = requires(T&& prepared) { std::move(prepared).contentPlan(); };

template <typename String>
concept AcceptsAnyTemporaryHttpClientRequestViewText = requires(String&& value) { HttpClientRequestView{.method = std::forward<String>(value)}; } || requires(String&& value) { HttpClientRequestView{.target = std::forward<String>(value)}; } || requires(HttpClientRequestView& request, String&& value) { request.method = std::forward<String>(value); } || requires(HttpClientRequestView& request, String&& value) { request.target = std::forward<String>(value); };

template <typename String>
concept AcceptsLvalueHttpClientRequestViewText = requires(HttpClientRequestView& request, String& value) {
    HttpClientRequestView{.method = value, .target = value};
    request.method = value;
    request.target = value;
};

template <typename Headers>
concept AcceptsHttp1ConnectHeaders = requires(Http1ClientRequestWriter& writer, const HttpOriginView& origin, std::array<char, 512>& buffer, Headers&& headers) { writer.prepareConnect(origin, std::forward<Headers>(headers), buffer); };

static_assert(!HasAnyRvalueHttp1ClientRequestPrepareAccessor<ruvia::Http1ClientRequestPrepareResult>);
static_assert(!HasAnyRvalueHttpClientRequestContentViewAccessor<ruvia::HttpClientRequestContentView>);
static_assert(!HasAnyRvalueHttp1ClientRequestWirePolicyAccessor<ruvia::Http1ClientRequestWirePolicy>);
static_assert(!HasAnyRvalueHttp1ClientRequestContentPlanAccessor<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasRvaluePreparedHttp1ClientRequestContentPlan<ruvia::PreparedHttp1ClientRequest>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<const std::string>);
static_assert(!AcceptsAnyTemporaryHttpClientRequestViewText<std::pmr::string>);
static_assert(AcceptsLvalueHttpClientRequestViewText<std::string>);
constexpr HttpClientRequestView kLiteralHttpClientRequestView{.method = "POST", .target = "/items"};
static_assert(kLiteralHttpClientRequestView.method.view() == "POST");
static_assert(kLiteralHttpClientRequestView.target.view() == "/items");
static_assert(kLiteralHttpClientRequestView.method == "POST");
static_assert("/items" == kLiteralHttpClientRequestView.target);
static_assert(!std::is_constructible_v<HttpClientRequestView::HeaderInit, std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::is_assignable_v<HttpClientRequestView::HeaderInit&, std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>&>);
static_assert(AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>&>);
static_assert(!AcceptsHttp1ConnectHeaders<std::vector<ruvia::HttpHeaderView>>);
static_assert(!AcceptsHttp1ConnectHeaders<const std::vector<ruvia::HttpHeaderView>>);
static_assert(!AcceptsHttp1ConnectHeaders<std::array<ruvia::HttpHeaderView, 1>>);
static_assert(!AcceptsHttp1ConnectHeaders<const std::array<ruvia::HttpHeaderView, 1>>);

template <typename T>
concept HasRequestContentMode = requires(const T& content) { content.mode(); };

template <typename T>
concept HasRequestContentValue = requires(const T& content) {
    { content.value() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasPreparedContentDisposition = requires(const T& plan) { plan.disposition(); };

template <typename T>
concept HasPreparedContentBytes = requires(const T& content) {
    { content.bytes() } -> std::same_as<std::string_view>;
};

static_assert(!HasRequestContentMode<ruvia::HttpClientRequestContentView>);
static_assert(!HasRequestContentValue<ruvia::HttpClientRequestContentView>);
static_assert(!HasRequestContentValue<ruvia::HttpClientRequestWithoutContent>);
static_assert(HasRequestContentValue<ruvia::HttpClientRequestBytesView>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestContentView>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::HttpClientRequestBytesView>);
static_assert(!HasPreparedContentDisposition<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasPreparedContentBytes<ruvia::Http1ClientRequestContentPlan>);
static_assert(!HasPreparedContentBytes<ruvia::Http1ClientRequestWithoutContent>);
static_assert(HasPreparedContentBytes<ruvia::Http1ClientImmediateRequestContent>);
static_assert(HasPreparedContentBytes<ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientRequestContentPlan>);
static_assert(!std::default_initializable<ruvia::Http1ClientRequestWithoutContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientImmediateRequestContent>);
static_assert(!std::default_initializable<ruvia::Http1ClientContinueGatedRequestContent>);
static_assert(std::default_initializable<Http1ClientRequestWirePolicy>);
constexpr auto kWithoutExpectation = Http1ClientRequestWirePolicy();
constexpr auto kExpectContinue = Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue);
static_assert(kWithoutExpectation.expectation() == ruvia::HttpClientRequestExpectation::kNone);
static_assert(kExpectContinue.expectation() == ruvia::HttpClientRequestExpectation::kContinue);

template <std::size_t N = 2048>
struct PreparedFixture final {
    std::array<char, N> buffer{};
    ruvia::Http1ClientRequestPrepareResult result;

    PreparedFixture(const HttpOriginView& origin, const HttpClientRequestView& request, Http1ClientRequestWirePolicy policy = Http1ClientRequestWirePolicy())
        : result(Http1ClientRequestWriter().prepare(origin, request, buffer, policy)) {}
};

[[nodiscard]] Http1ClientRequestPrepareError prepareError(const HttpClientRequestView& request, Http1ClientRequestWirePolicy policy = Http1ClientRequestWirePolicy()) {
    std::array<char, 512> buffer;
    const auto result = Http1ClientRequestWriter().prepare(HttpOriginView::https("example.test"), request, buffer, policy);
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
    HttpClientRequestView request;
    request.method = "POST";
    request.target = "/items?q=1";
    request.headers = headers;
    request.content = HttpClientRequestContentView::bytes("payload");

    PreparedFixture fixture(HttpOriginView::https("example.test"), request, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    const auto* prepared = fixture.result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->head() ==
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
    HttpClientRequestView absent;
    absent.method = "GET";
    RUVIA_CHECK(absent.content.withoutContent() != nullptr);
    RUVIA_CHECK(absent.content.borrowedBytes() == nullptr);
    PreparedFixture absentFixture(HttpOriginView::https("example.test"), absent);
    const auto* absentPrepared = absentFixture.result.prepared();
    RUVIA_CHECK(absentPrepared != nullptr);
    if (absentPrepared != nullptr) {
        RUVIA_CHECK(absentPrepared->contentPlan().withoutContent() != nullptr);
        RUVIA_CHECK(absentPrepared->contentPlan().immediate() == nullptr);
        RUVIA_CHECK(absentPrepared->contentPlan().continueGated() == nullptr);
        RUVIA_CHECK(absentPrepared->head().find("Content-Length") == std::string_view::npos);
    }

    HttpClientRequestView empty;
    empty.method = "POST";
    empty.content = HttpClientRequestContentView::bytes("");
    RUVIA_CHECK(empty.content.withoutContent() == nullptr);
    RUVIA_CHECK(empty.content.borrowedBytes() != nullptr);
    if (const auto* bytes = empty.content.borrowedBytes()) {
        RUVIA_CHECK(bytes->value().empty());
    }
    PreparedFixture emptyFixture(HttpOriginView::https("example.test"), empty);
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
        RUVIA_CHECK(emptyPrepared->head().find("Content-Length: 0\r\n") != std::string_view::npos);
    }
}

RUVIA_TEST(http1_client_request_writer_owns_request_target_forms_and_host) {
    HttpClientRequestView extension;
    extension.method = "PROPFIND";
    extension.target = "/dav";
    PreparedFixture extensionFixture(HttpOriginView::https("example.test", 8443), extension);
    const auto* extensionPrepared = extensionFixture.result.prepared();
    RUVIA_CHECK(extensionPrepared != nullptr);
    if (extensionPrepared != nullptr) {
        RUVIA_CHECK(extensionPrepared->head().starts_with("PROPFIND /dav HTTP/1.1\r\nHost: example.test:8443\r\n"));
    }

    HttpClientRequestView options;
    options.method = "OPTIONS";
    options.target = "*";
    PreparedFixture optionsFixture(HttpOriginView::http("example.test"), options);
    RUVIA_CHECK(optionsFixture.result.prepared() != nullptr);

    HttpClientRequestView invalidAsterisk;
    invalidAsterisk.method = "GET";
    invalidAsterisk.target = "*";
    RUVIA_CHECK(prepareError(invalidAsterisk) == Http1ClientRequestPrepareError::kInvalidTarget);

    HttpClientRequestView absolute;
    absolute.target = "https://example.test/path";
    RUVIA_CHECK(prepareError(absolute) == Http1ClientRequestPrepareError::kInvalidTarget);

    HttpClientRequestView connect;
    connect.method = "CONNECT";
    connect.target = "example.test:443";
    RUVIA_CHECK(prepareError(connect) == Http1ClientRequestPrepareError::kConnectRequiresDedicatedEntry);
}

RUVIA_TEST(http1_client_connect_entry_generates_authority_form_atomically) {
    std::array<char, 512> buffer;
    const auto result = Http1ClientRequestWriter().prepareConnect(HttpOriginView::https("example.test"), {}, buffer);
    const auto* prepared = result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK(prepared->head() ==
                    "CONNECT example.test:443 HTTP/1.1\r\n"
                    "Host: example.test\r\n\r\n");
        RUVIA_CHECK(prepared->contentPlan().withoutContent() != nullptr);
    }

    std::array<char, 512> ipv6Buffer;
    const auto ipv6 = Http1ClientRequestWriter().prepareConnect(HttpOriginView::https("[::1]"), {}, ipv6Buffer);
    RUVIA_CHECK(ipv6.prepared() != nullptr);
    if (ipv6.prepared() != nullptr) {
        RUVIA_CHECK(ipv6.prepared()->head().starts_with("CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]\r\n"));
    }

    std::array<char, 128> invalidBuffer;
    const auto invalid = Http1ClientRequestWriter().prepareConnect(HttpOriginView::https("example.test", 0), {}, invalidBuffer);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == Http1ClientRequestPrepareError::kInvalidConnectOrigin);
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
        HttpClientRequestView request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&header, 1);
        request.content = HttpClientRequestContentView::bytes("payload");
        RUVIA_CHECK(prepareError(request) == test.error);
    }
}

RUVIA_TEST(http1_client_request_writer_rejects_repeated_singleton_fields) {
    struct Case final {
        std::string_view name;
        std::string_view first;
        std::string_view second;
    };
    const Case cases[] = {
        {"Access-Control-Request-Method", "GET", "POST"},
        {"Authorization", "Bearer first", "Bearer second"},
        {"Content-Type", "text/plain", "application/json"},
        {"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT", "Mon, 07 Nov 1994 08:49:37 GMT"},
        {"If-Range", "\"first\"", "\"second\""},
        {"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:37 GMT", "Mon, 07 Nov 1994 08:49:37 GMT"},
        {"Origin", "https://first.test", "https://second.test"},
        {"Range", "bytes=0-1", "bytes=2-3"},
        {"Sec-WebSocket-Key", "first", "second"},
        {"Sec-WebSocket-Version", "13", "12"},
        {"User-Agent", "first/1", "second/2"},
    };
    for (const auto& test : cases) {
        const ruvia::HttpHeaderView headers[] = {
            {test.name, test.first},
            {test.name, test.second},
        };
        HttpClientRequestView request;
        request.headers = headers;

        RUVIA_CHECK(prepareError(request) == Http1ClientRequestPrepareError::kInvalidHeader);
    }
}

RUVIA_TEST(http1_client_request_writer_validates_cors_fields) {
    const ruvia::HttpHeaderView invalidHeaders[] = {
        {"Origin", "https://example.test/path"},
        {"Access-Control-Request-Method", "GET, POST"},
        {"Access-Control-Request-Headers", "X-Good, Bad Header"},
    };
    for (const auto& header : invalidHeaders) {
        HttpClientRequestView request;
        request.headers = std::span<const ruvia::HttpHeaderView>(&header, 1);
        RUVIA_CHECK(prepareError(request) == Http1ClientRequestPrepareError::kInvalidHeader);
    }

    const ruvia::HttpHeaderView validHeaders[] = {
        {"Origin", "https://first.test https://second.test"},
        {"Access-Control-Request-Method", "GET"},
        {"Access-Control-Request-Headers", "X-First"},
        {"Access-Control-Request-Headers", "X-Second, X-Third"},
    };
    HttpClientRequestView valid;
    valid.headers = validHeaders;
    PreparedFixture fixture(HttpOriginView::https("example.test"), valid);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_owns_hop_by_hop_field_contracts) {
    struct FailureCase final {
        std::array<ruvia::HttpHeaderView, 2> headers;
        std::size_t count;
        Http1ClientRequestPrepareError error;
    };
    const FailureCase failures[] = {
        {.headers = {ruvia::HttpHeaderView("Connection", "close,"), {}}, .count = 1, .error = Http1ClientRequestPrepareError::kInvalidConnection},
        {.headers = {ruvia::HttpHeaderView("Connection", "close;parameter"), {}}, .count = 1, .error = Http1ClientRequestPrepareError::kInvalidConnection},
        {.headers = {ruvia::HttpHeaderView("Upgrade", "websocket"), {}}, .count = 1, .error = Http1ClientRequestPrepareError::kUpgradeConnectionOptionRequired},
        {.headers = {ruvia::HttpHeaderView("Connection", "Upgrade"), ruvia::HttpHeaderView("Upgrade", "websocket/")}, .count = 2, .error = Http1ClientRequestPrepareError::kInvalidUpgrade},
        {.headers = {ruvia::HttpHeaderView("TE", "trailers"), {}}, .count = 1, .error = Http1ClientRequestPrepareError::kTeConnectionOptionRequired},
    };
    for (const auto& test : failures) {
        HttpClientRequestView request;
        request.headers = std::span<const ruvia::HttpHeaderView>(test.headers.data(), test.count);
        RUVIA_CHECK(prepareError(request) == test.error);
    }

    for (const std::string_view connectionOptions : {"Host", "close, content-length", "EXPECT, keep-alive", "Cache-Control", "Trailer", "Authorization", "Cookie", "Range"}) {
        const ruvia::HttpHeaderView connection("Connection", connectionOptions);
        HttpClientRequestView request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&connection, 1);
        request.content = HttpClientRequestContentView::bytes("payload");
        RUVIA_CHECK(prepareError(request) == Http1ClientRequestPrepareError::kInvalidConnection);
    }

    const ruvia::HttpHeaderView validHeaders[] = {
        {"Connection", "keep-alive"},
        {"Connection", "Upgrade, TE, X-Hop"},
        {"Upgrade", "custom/1, websocket"},
        {"TE", "trailers"},
        {"X-Hop", "value"},
    };
    HttpClientRequestView valid;
    valid.headers = validHeaders;
    PreparedFixture fixture(HttpOriginView::https("example.test"), valid);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_validates_te_capabilities_and_weights) {
    const auto prepareWithTe = [&ruvia_ctx](std::string_view value) -> std::optional<Http1ClientRequestPrepareError> {
        const std::array headers{
            ruvia::HttpHeaderView("Connection", "TE"),
            ruvia::HttpHeaderView("TE", value),
        };
        HttpClientRequestView request;
        request.headers = headers;
        std::array<char, 512> buffer;
        const auto result = Http1ClientRequestWriter().prepare(HttpOriginView::https("example.test"), request, buffer);
        if (const auto* failure = result.failure()) {
            return failure->error();
        }
        RUVIA_CHECK(result.prepared() != nullptr);
        return std::nullopt;
    };

    for (const std::string_view valid : {"", "trailers", "gzip", "deflate;q=0.5", "deflate;Q=0.5", "x-gzip ; q=1.000", "gzip;q=0, trailers"}) {
        RUVIA_CHECK(!prepareWithTe(valid).has_value());
    }

    for (const std::string_view invalid : {",trailers", "trailers,", "trailers,,gzip", "chunked", "br", "trailers;q=0.5", "gzip;q=1.001", "gzip;q=\"0.5\"", "gzip;q =0.5", "gzip;q= 0.5", "gzip; q = 0.5", "gzip;level=1", "gzip;q=0.5;level=1", "gzip; q", "gzip;q="}) {
        RUVIA_CHECK(prepareWithTe(invalid) == Http1ClientRequestPrepareError::kInvalidHeader);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_expect_content_semantics) {
    const ruvia::HttpHeaderView expect("Expect", "100-Continue");
    HttpClientRequestView rawExpectation;
    rawExpectation.method = "POST";
    rawExpectation.headers = std::span<const ruvia::HttpHeaderView>(&expect, 1);
    rawExpectation.content = HttpClientRequestContentView::bytes("x");
    RUVIA_CHECK(prepareError(rawExpectation) == Http1ClientRequestPrepareError::kExpectHeaderManagedByWriter);

    HttpClientRequestView absent;
    absent.method = "POST";
    PreparedFixture absentFixture(HttpOriginView::https("example.test"), absent, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    RUVIA_CHECK(absentFixture.result.failure() != nullptr);
    if (absentFixture.result.failure() != nullptr) {
        RUVIA_CHECK(absentFixture.result.failure()->error() == Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }

    HttpClientRequestView empty = absent;
    empty.content = HttpClientRequestContentView::bytes("");
    PreparedFixture emptyFixture(HttpOriginView::https("example.test"), empty, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    RUVIA_CHECK(emptyFixture.result.failure() != nullptr);
    if (emptyFixture.result.failure() != nullptr) {
        RUVIA_CHECK(emptyFixture.result.failure()->error() == Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }

    HttpClientRequestView nonempty;
    nonempty.method = "POST";
    nonempty.content = HttpClientRequestContentView::bytes("x");
    PreparedFixture fixture(HttpOriginView::https("example.test"), nonempty, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
    if (fixture.result.prepared() != nullptr) {
        RUVIA_CHECK(fixture.result.prepared()->contentPlan().continueGated() != nullptr);
        RUVIA_CHECK(fixture.result.prepared()->head().find("Expect: 100-continue\r\n") != std::string_view::npos);
    }
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_close_policy) {
    HttpClientRequestView request;
    const auto invalidWithoutExpectation = Http1ClientRequestWirePolicy(static_cast<Http1ClosePolicy>(0xFF));
    const auto invalidExpectContinue = Http1ClientRequestWirePolicy(static_cast<Http1ClosePolicy>(0xFF), ruvia::HttpClientRequestExpectation::kContinue);
    RUVIA_CHECK(prepareError(request, invalidWithoutExpectation) == Http1ClientRequestPrepareError::kInvalidClosePolicy);
    RUVIA_CHECK(prepareError(request, invalidExpectContinue) == Http1ClientRequestPrepareError::kInvalidClosePolicy);

    std::array<char, 512> buffer{};
    const auto connect = Http1ClientRequestWriter().prepareConnect(
        HttpOriginView::https("example.test"),
        std::span<const ruvia::HttpHeaderView>{},
        buffer,
        invalidWithoutExpectation);
    RUVIA_CHECK(connect.failure() != nullptr);
    if (connect.failure() != nullptr) {
        RUVIA_CHECK(connect.failure()->error() == Http1ClientRequestPrepareError::kInvalidClosePolicy);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_method_content_semantics) {
    HttpClientRequestView trace;
    trace.method = "TRACE";
    trace.content = HttpClientRequestContentView::bytes("trace body");
    RUVIA_CHECK(prepareError(trace) == Http1ClientRequestPrepareError::kContentForbiddenForMethod);

    HttpClientRequestView options;
    options.method = "OPTIONS";
    options.content = HttpClientRequestContentView::bytes("options body");
    RUVIA_CHECK(prepareError(options) == Http1ClientRequestPrepareError::kOptionsContentTypeRequired);

    // Content-Length signals request content even when its value is zero. An
    // explicitly empty OPTIONS representation therefore has the same mandatory
    // Content-Type contract as a non-empty one.
    options.content = HttpClientRequestContentView::bytes("");
    RUVIA_CHECK(prepareError(options) == Http1ClientRequestPrepareError::kOptionsContentTypeRequired);

    const ruvia::HttpHeaderView contentType("Content-Type", "application/json");
    options.headers = std::span<const ruvia::HttpHeaderView>(&contentType, 1);
    PreparedFixture fixture(HttpOriginView::https("example.test"), options);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);

    const ruvia::HttpHeaderView invalidContentType("Content-Type", "invalid");
    options.headers = std::span<const ruvia::HttpHeaderView>(&invalidContentType, 1);
    RUVIA_CHECK(prepareError(options) == Http1ClientRequestPrepareError::kInvalidHeader);
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_content_type_parameters) {
    for (const std::string_view value : {"text/plain; charset", "text/plain; charset=", "text/plain; charset =utf-8", "text/plain; charset=utf-8; CHARSET=latin1", "text/plain; charset=\"unterminated"}) {
        const ruvia::HttpHeaderView contentType("Content-Type", value);
        HttpClientRequestView request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&contentType, 1);
        request.content = HttpClientRequestContentView::bytes("body");
        RUVIA_CHECK(prepareError(request) == Http1ClientRequestPrepareError::kInvalidHeader);
    }

    const ruvia::HttpHeaderView validContentType("Content-Type", "text/plain; charset=\"utf-8\"");
    HttpClientRequestView valid;
    valid.method = "POST";
    valid.headers = std::span<const ruvia::HttpHeaderView>(&validContentType, 1);
    valid.content = HttpClientRequestContentView::bytes("body");
    PreparedFixture fixture(HttpOriginView::https("example.test"), valid);
    RUVIA_CHECK(fixture.result.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "", ",gzip", "gzip,"}) {
        const ruvia::HttpHeaderView contentEncoding("Content-Encoding", value);
        HttpClientRequestView request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&contentEncoding, 1);
        request.content = HttpClientRequestContentView::bytes("body");
        RUVIA_CHECK(prepareError(request) == Http1ClientRequestPrepareError::kInvalidHeader);
    }

    for (const std::string_view value : {"deflate", "gzip, br"}) {
        const ruvia::HttpHeaderView contentEncoding("Content-Encoding", value);
        HttpClientRequestView request;
        request.method = "POST";
        request.headers = std::span<const ruvia::HttpHeaderView>(&contentEncoding, 1);
        request.content = HttpClientRequestContentView::bytes("body");
        PreparedFixture fixture(HttpOriginView::https("example.test"), request);
        RUVIA_CHECK(fixture.result.prepared() != nullptr);
    }
}

RUVIA_TEST(http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output) {
    HttpClientRequestView request;
    request.method = "POST";
    request.target = "/upload";
    request.content = HttpClientRequestContentView::bytes("body");

    std::array<char, 8> small;
    small.fill('z');
    const auto tooSmall = Http1ClientRequestWriter().prepare(HttpOriginView::https("example.test"), request, small);
    RUVIA_CHECK(tooSmall.bufferTooSmall() != nullptr);
    RUVIA_CHECK(std::ranges::all_of(small, [](char value) { return value == 'z'; }));

    std::array<char, 512> enough;
    const auto prepared = Http1ClientRequestWriter().prepare(HttpOriginView::https("example.test"), request, enough);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() != nullptr && tooSmall.bufferTooSmall() != nullptr) {
        RUVIA_CHECK_EQ(tooSmall.bufferTooSmall()->requiredHeadBytes(), prepared.prepared()->head().size());
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_header_count_and_size_transactionally) {
    std::array<ruvia::HttpHeaderView, ruvia::kMaxHttpHeaderFields> headers;
    headers.fill(ruvia::HttpHeaderView("X-Test", "x"));
    HttpClientRequestView tooMany;
    tooMany.headers = headers;
    RUVIA_CHECK(prepareError(tooMany) == Http1ClientRequestPrepareError::kTooManyHeaders);

    HttpClientRequestView oversized;
    std::string target(ruvia::kMaxHttpHeaderBytes, 'a');
    target.front() = '/';
    oversized.target = target;
    RUVIA_CHECK(prepareError(oversized) == Http1ClientRequestPrepareError::kHeaderTooLarge);

    HttpClientRequestView invalidMethod;
    invalidMethod.method = "GET\r";
    std::array<char, 128> untouched;
    untouched.fill('q');
    const auto invalid = Http1ClientRequestWriter().prepare(HttpOriginView::https("example.test"), invalidMethod, untouched);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == Http1ClientRequestPrepareError::kInvalidMethod);
    RUVIA_CHECK(std::ranges::all_of(untouched, [](char value) { return value == 'q'; }));
    RUVIA_CHECK(!ruvia::http1ClientRequestPrepareErrorMessage(Http1ClientRequestPrepareError::kInvalidMethod).empty());
}

RUVIA_TEST(http1_client_request_context_binds_the_actual_close_signal) {
    const ruvia::HttpHeaderView closeHeader("Connection", "close");
    HttpClientRequestView request;
    request.headers = std::span<const ruvia::HttpHeaderView>(&closeHeader, 1);
    PreparedFixture explicitClose(HttpOriginView::https("example.test"), request);
    const auto* explicitPrepared = explicitClose.result.prepared();
    RUVIA_CHECK(explicitPrepared != nullptr);
    if (explicitPrepared != nullptr) {
        auto parser = ruvia::Http1ClientResponseParser(*explicitPrepared);
        const auto response = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
        RUVIA_CHECK(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto* withoutContent = response.parsed()->plan().withoutContent();
            RUVIA_CHECK(withoutContent != nullptr);
            if (withoutContent != nullptr) {
                RUVIA_CHECK(withoutContent->persistence() == ruvia::Http1ClosePolicy::kCloseAfterResponse);
            }
        }
    }

    HttpClientRequestView generatedRequest;
    PreparedFixture generatedClose(HttpOriginView::https("example.test"), generatedRequest, Http1ClientRequestWirePolicy(Http1ClosePolicy::kCloseAfterResponse));
    RUVIA_CHECK(generatedClose.result.prepared() != nullptr);
    if (generatedClose.result.prepared() != nullptr) {
        RUVIA_CHECK(generatedClose.result.prepared()->head().find("Connection: close\r\n") != std::string_view::npos);
    }
}
