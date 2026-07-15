#include "test_harness.h"

#include <array>
#include <concepts>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpClientRedirect.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/client/HttpClientContentEncoding.h"
#include "ruvia/http/detail/client/HttpClientResponseLimits.h"

namespace {

using ruvia::HttpClientResponseHead;
using ruvia::Http1ClientRequestClosePolicy;
using ruvia::Http1ClientRequestContentCompletionStatus;
using ruvia::Http1ClientRequestContentSignal;
using ruvia::Http1ClientRequestWirePolicy;
using ruvia::Http1ClientResponsePersistence;
using ruvia::Http1ClientResponseParseError;
using ruvia::Http1ClientResponseParseResult;
using ruvia::Http1ClientResponseParser;
using ruvia::Http1ParsedClientResponseHead;
using ruvia::HttpProtocolVersion;
using ruvia::isValidHttpClientOriginTarget;

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(const T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ClientResponsePlanAccessor =
    requires(T&& plan) { std::move(plan).informational(); } ||
    requires(T&& plan) { std::move(plan).withoutContent(); } ||
    requires(T&& plan) { std::move(plan).knownLength(); } ||
    requires(T&& plan) { std::move(plan).chunked(); } ||
    requires(T&& plan) { std::move(plan).closeDelimited(); } ||
    requires(T&& plan) { std::move(plan).connectTunnel(); } ||
    requires(T&& plan) { std::move(plan).protocolUpgrade(); };

template <typename T>
concept HasAnyRvalueHttp1ParsedClientResponseBorrow =
    requires(T&& parsed) { std::move(parsed).head(); } ||
    requires(T&& parsed) { std::move(parsed).plan(); };

static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<
    Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasAnyRvalueHttp1ParsedClientResponseBorrow<
    Http1ParsedClientResponseHead>);

template <typename Access>
concept CanMutateHttpClientResponseHeadStatus = requires(
    HttpClientResponseHead& head) {
    Access::setStatus(head, std::uint16_t{200});
};

static_assert(!CanMutateHttpClientResponseHeadStatus<
    ruvia::detail::HttpClientResponseHeadAccess>);

Http1ClientResponseParseResult parseWire(
    std::string_view method,
    std::string_view wire,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::array<char, 2048> requestHead;
    const auto origin = ruvia::HttpOrigin::https("example.test");
    ruvia::HttpClientRequest request;
    request.method = method;
    request.headers = requestHeaders;
    const auto preparedResult = method == "CONNECT"
        ? ruvia::Http1ClientRequestWriter().prepareConnect(
              origin,
              requestHeaders,
              requestHead,
              Http1ClientRequestWirePolicy::withoutExpectation(closePolicy))
        : ruvia::Http1ClientRequestWriter().prepare(
              origin,
              request,
              requestHead,
              Http1ClientRequestWirePolicy::withoutExpectation(closePolicy));
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        throw std::runtime_error("test request could not be prepared");
    }
    auto parser = Http1ClientResponseParser(*prepared, resource);
    return parser.parse(wire);
}

Http1ClientResponseParseResult parseResult(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::string wire(headerSection);
    wire.append("\r\n\r\n");
    return parseWire(method, wire, closePolicy, requestHeaders, resource);
}

Http1ParsedClientResponseHead parseHead(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    auto result = parseResult(
        method, headerSection, closePolicy, requestHeaders);
    auto* parsed = result.parsed();
    if (parsed == nullptr) {
        throw std::runtime_error("test expected a parsed HTTP/1 response head");
    }
    return std::move(*parsed);
}

struct ParsedResponse final {
    HttpClientResponseHead head;
};

ParsedResponse parseResponse(
    std::string_view method,
    std::string_view headerSection) {
    auto head = parseHead(method, headerSection);
    return ParsedResponse{std::move(head).takeHead()};
}

bool parseFails(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    const auto result = parseResult(
        method, headerSection, closePolicy, requestHeaders);
    return result.failure() != nullptr;
}

const ruvia::Http1ClientKnownLengthResponse& requireKnownLength(
    const ruvia::Http1ClientResponsePlan& plan) {
    const auto* knownLength = plan.knownLength();
    if (knownLength == nullptr) {
        throw std::runtime_error("test expected exact-length response framing");
    }
    return *knownLength;
}

const ruvia::Http1ClientChunkedResponse& requireChunked(
    const ruvia::Http1ClientResponsePlan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked response framing");
    }
    return *chunked;
}

const ruvia::Http1ClientCloseDelimitedResponse& requireCloseDelimited(
    const ruvia::Http1ClientResponsePlan& plan) {
    const auto* closeDelimited = plan.closeDelimited();
    if (closeDelimited == nullptr) {
        throw std::runtime_error("test expected close-delimited response framing");
    }
    return *closeDelimited;
}

const ruvia::Http1ClientResponseWithoutContent& requireWithoutContent(
    const ruvia::Http1ClientResponsePlan& plan) {
    const auto* withoutContent = plan.withoutContent();
    if (withoutContent == nullptr) {
        throw std::runtime_error("test expected a final response without content");
    }
    return *withoutContent;
}

std::size_t activePlanAlternativeCount(
    const ruvia::Http1ClientResponsePlan& plan) noexcept {
    return static_cast<std::size_t>(plan.informational() != nullptr) +
        static_cast<std::size_t>(plan.withoutContent() != nullptr) +
        static_cast<std::size_t>(plan.knownLength() != nullptr) +
        static_cast<std::size_t>(plan.chunked() != nullptr) +
        static_cast<std::size_t>(plan.closeDelimited() != nullptr) +
        static_cast<std::size_t>(plan.connectTunnel() != nullptr) +
        static_cast<std::size_t>(plan.protocolUpgrade() != nullptr);
}

Http1ClientResponseParseError parseFailureError(
    std::string_view method,
    std::string_view headerSection,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    const auto result = parseResult(
        method,
        headerSection,
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    const auto* failure = result.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected an HTTP/1 response parse failure");
    }
    return failure->error();
}

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    CountingMemoryResource() noexcept
        : upstream_(std::pmr::get_default_resource()) {}

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t allocationCount_{0};
};

template <typename T>
concept HasResponsePlanMode = requires(const T& value) {
    value.mode();
};

template <typename T>
concept HasResponseConnectionDisposition = requires(const T& value) {
    value.connectionDisposition();
};

template <typename T>
concept HasResponseContentLength = requires(const T& value) {
    value.contentLength();
};

template <typename T>
concept HasResponseTransferCodings = requires(const T& value) {
    { value.transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
} && requires(const T&& value) {
    { std::move(value).transferCodings() } ->
        std::same_as<ruvia::detail::HttpTransferCodings>;
};

template <typename T>
concept HasResponsePersistence = requires(const T& value) {
    value.persistence();
};

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView =
    requires(T&& value) { std::move(value).name(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).headers(); } ||
    requires(T&& value) { std::move(value).body(); };

template <typename T>
concept HasHttpClientResponseBody = requires(const T& head) {
    { head.body() } -> std::same_as<std::string_view>;
};

static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::HttpClientResponseHead>);
static_assert(!HasHttpClientResponseBody<ruvia::HttpClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<
    ruvia::Http1ClientCloseDelimitedResponse>);

static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponsePlan>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientInformationalResponse>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientResponseWithoutContent>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientChunkedResponse>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientConnectTunnel>);
static_assert(!std::is_default_constructible_v<
    ruvia::Http1ClientProtocolUpgrade>);
static_assert(!HasResponsePlanMode<ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseConnectionDisposition<
    ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseContentLength<ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseTransferCodings<ruvia::Http1ClientResponsePlan>);
static_assert(HasResponseContentLength<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!HasResponseContentLength<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasResponseContentLength<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(HasResponseTransferCodings<ruvia::Http1ClientChunkedResponse>);
static_assert(HasResponseTransferCodings<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!HasResponseTransferCodings<
    ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasResponsePersistence<
    ruvia::Http1ClientResponseWithoutContent>);
static_assert(HasResponsePersistence<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasResponsePersistence<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasResponsePersistence<
    ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().knownLength()),
    const ruvia::Http1ClientKnownLengthResponse*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().connectTunnel()),
    const ruvia::Http1ClientConnectTunnel*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().protocolUpgrade()),
    const ruvia::Http1ClientProtocolUpgrade*>);

}  // namespace

RUVIA_TEST(http_client_response_head_commits_status_and_version_at_construction) {
    auto head = ruvia::detail::HttpClientResponseHeadAccess::make(
        207,
        HttpProtocolVersion::kHttp10,
        std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(head.status(), std::uint16_t{207});
    RUVIA_CHECK(head.protocolVersion() == HttpProtocolVersion::kHttp10);
}

RUVIA_TEST(http_client_origin_target_validation) {
    RUVIA_CHECK(isValidHttpClientOriginTarget("/ok%2F?q=%7B%7D"));
    RUVIA_CHECK(isValidHttpClientOriginTarget("*"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget(""));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("relative"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad#fragment"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad\\path"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%zz"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%2"));
}

RUVIA_TEST(http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parseHead("GET", "HTTP/1.1 103 Early Hints");
    const auto withoutContent = parseHead("GET", "HTTP/1.1 204 No Content");
    const auto knownLength = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto closeDelimited = parseHead("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parseHead(
        "CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        upgradeHeaders);

    for (const auto* plan : {
             &informational.plan(),
             &withoutContent.plan(),
             &knownLength.plan(),
             &chunked.plan(),
             &closeDelimited.plan(),
             &tunnel.plan(),
             &upgrade.plan()}) {
        RUVIA_CHECK_EQ(activePlanAlternativeCount(*plan), std::size_t{1});
    }
}

RUVIA_TEST(http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header =
        "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parseHead("GET", header);
    const auto& knownLength = requireKnownLength(head.plan());
    RUVIA_CHECK_EQ(knownLength.contentLength(), std::size_t{5});
    RUVIA_CHECK(knownLength.requiresBodyConsumption());
    RUVIA_CHECK(
        knownLength.persistence() == Http1ClientResponsePersistence::kReuse);
    RUVIA_CHECK_EQ(head.consumedBytes(), header.size() + 4);

    const auto empty = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    RUVIA_CHECK(!requireKnownLength(empty.plan()).requiresBodyConsumption());
}

RUVIA_TEST(http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    RUVIA_CHECK_EQ(
        requireKnownLength(combined.plan()).contentLength(),
        std::size_t{5});

    const auto repeated = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK_EQ(
        requireKnownLength(repeated.plan()).contentLength(),
        std::size_t{5});

    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

RUVIA_TEST(http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = requireChunked(head.plan());
    RUVIA_CHECK(chunked.transferCodings().empty());
    RUVIA_CHECK(
        chunked.persistence() == Http1ClientResponsePersistence::kReuse);
}

RUVIA_TEST(http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combinedChunked = requireChunked(combined.plan());
    RUVIA_CHECK_EQ(
        combinedChunked.transferCodings().count,
        std::size_t{1});
    RUVIA_CHECK(
        combinedChunked.transferCodings().values[0] ==
        ruvia::detail::HttpTransferCoding::kGzip);
    RUVIA_CHECK(
        combinedChunked.persistence() == Http1ClientResponsePersistence::kReuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& splitChunked = requireChunked(split.plan());
    RUVIA_CHECK(
        splitChunked.transferCodings().values[0] ==
        ruvia::detail::HttpTransferCoding::kDeflate);
}

RUVIA_TEST(http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& closeDelimited = requireCloseDelimited(head.plan());
    RUVIA_CHECK_EQ(closeDelimited.transferCodings().count, std::size_t{1});
}

RUVIA_TEST(http_client_rejects_invalid_or_unsupported_transfer_coding) {
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , chunked"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked"));
}

RUVIA_TEST(http_client_content_length_and_transfer_encoding_rejected_for_body) {
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

RUVIA_TEST(http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parseHead(
        "HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& withoutContent = requireWithoutContent(head.plan());
    RUVIA_CHECK(
        withoutContent.persistence() == Http1ClientResponsePersistence::kReuse);

    const auto notModified = parseHead(
        "GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(notModified.plan().withoutContent() != nullptr);

    const auto noContent = parseHead(
        "GET",
        "HTTP/1.1 204 No Content\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(noContent.plan().withoutContent() != nullptr);
}

RUVIA_TEST(http_client_205_uses_normal_http1_message_framing) {
    const auto zeroLength = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    const auto& zeroLengthBody = requireKnownLength(zeroLength.plan());
    RUVIA_CHECK(!zeroLengthBody.requiresBodyConsumption());
    RUVIA_CHECK(
        zeroLengthBody.persistence() == Http1ClientResponsePersistence::kReuse);

    const auto nonzeroLength = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3");
    const auto& nonzeroLengthBody = requireKnownLength(nonzeroLength.plan());
    RUVIA_CHECK_EQ(nonzeroLengthBody.contentLength(), std::size_t{3});
    RUVIA_CHECK(nonzeroLengthBody.requiresBodyConsumption());

    const auto chunked = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(chunked.plan().chunked() != nullptr);

    const auto unframed = parseHead(
        "GET", "HTTP/1.1 205 Reset Content");
    RUVIA_CHECK(unframed.plan().closeDelimited() != nullptr);
}

RUVIA_TEST(http_client_informational_response_awaits_final_response) {
    for (const auto status : {
             std::string_view("HTTP/1.1 100 Continue"),
             std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parseHead("GET", status);
        RUVIA_CHECK(head.plan().informational() != nullptr);
    }

    const auto ignoredFraming = parseHead(
        "GET",
        "HTTP/1.1 103 Early Hints\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(ignoredFraming.plan().informational() != nullptr);
}

RUVIA_TEST(http_client_limits_informational_responses_per_exchange) {
    ruvia::HttpClientRequest request;
    request.method = "GET";
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        requestHead);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(*prepared.prepared());
    constexpr std::string_view earlyHints =
        "HTTP/1.1 103 Early Hints\r\n\r\n";
    for (std::size_t i = 0;
         i < ruvia::detail::kMaxHttpClientInterimResponses;
         ++i) {
        const auto interim = parser.parse(earlyHints);
        RUVIA_CHECK(interim.parsed() != nullptr);
        if (interim.parsed() != nullptr) {
            RUVIA_CHECK(interim.parsed()->plan().informational() != nullptr);
        }
    }

    const auto excessive = parser.parse(earlyHints);
    RUVIA_CHECK(excessive.failure() != nullptr);
    if (excessive.failure() != nullptr) {
        RUVIA_CHECK(
            excessive.failure()->error() ==
            Http1ClientResponseParseError::kTooManyInformationalResponses);
    }
    const auto afterFailure = parser.parse(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(afterFailure.failure() != nullptr);
    if (afterFailure.failure() != nullptr) {
        RUVIA_CHECK(
            afterFailure.failure()->error() ==
            Http1ClientResponseParseError::kExchangeFailed);
    }
}

RUVIA_TEST(http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        requestHead,
        Http1ClientRequestWirePolicy::expectContinue());
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->contentPlan().continueGated() != nullptr);

    Http1ClientResponseParser parser(*prepared);
    auto earlyHints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(earlyHints.parsed() != nullptr);
    if (earlyHints.parsed() != nullptr) {
        RUVIA_CHECK(
            !earlyHints.parsed()->plan().requestContentSignal());
        RUVIA_CHECK(
            earlyHints.parsed()->plan().informational() != nullptr);
    }

    auto continueResponse = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    if (continueResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            continueResponse.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kContinue);
    }
    const auto duplicateContinue = parser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(duplicateContinue.parsed() != nullptr);
    if (duplicateContinue.parsed() != nullptr) {
        RUVIA_CHECK(
            !duplicateContinue.parsed()->plan().requestContentSignal());
    }
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kAlreadyComplete);

    auto finalResponse = parser.parse(
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            !finalResponse.parsed()->plan().requestContentSignal());
    }

    const auto afterFinal = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(afterFinal.failure() != nullptr);
    if (afterFinal.failure() != nullptr) {
        RUVIA_CHECK(
            afterFinal.failure()->error() ==
            Http1ClientResponseParseError::kExchangeComplete);
    }
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kExchangeTerminal);
}

RUVIA_TEST(http_client_expect_final_cancels_only_pending_request_content) {
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        requestHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(*prepared.prepared());
    const auto finalResponse = parser.parse(
        "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            finalResponse.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kExchangeComplete);
    }

    Http1ClientResponseParser completedParser(*prepared.prepared());
    RUVIA_CHECK(
        completedParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto completedFinal = completedParser.parse(
        "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(completedFinal.parsed() != nullptr);
    if (completedFinal.parsed() != nullptr) {
        RUVIA_CHECK(
            !completedFinal.parsed()->plan().requestContentSignal());
    }
}

RUVIA_TEST(http_client_final_after_continue_does_not_cancel_released_content) {
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        requestHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(*prepared.prepared());
    const auto continueResponse = parser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    if (continueResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            continueResponse.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kContinue);
    }

    const auto finalResponse = parser.parse(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            !finalResponse.parsed()->plan().requestContentSignal());
        RUVIA_CHECK(
            requireKnownLength(finalResponse.parsed()->plan()).persistence() ==
            Http1ClientResponsePersistence::kReuse);
    }
}

RUVIA_TEST(http_client_upgrade_after_expect_requires_prior_continue) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> rejectedHead;
    const auto rejectedPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        rejectedHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(rejectedPrepared.prepared() != nullptr);
    if (rejectedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser rejectedParser(*rejectedPrepared.prepared());
    const auto rejected = rejectedParser.parse(switching);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        RUVIA_CHECK(
            rejected.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }
    const auto afterFailure = rejectedParser.parse(switching);
    RUVIA_CHECK(afterFailure.failure() != nullptr);
    if (afterFailure.failure() != nullptr) {
        RUVIA_CHECK(
            afterFailure.failure()->error() ==
            Http1ClientResponseParseError::kExchangeFailed);
    }

    // RFC 9110 section 7.8 still requires the server to acknowledge Expect
    // with 100 before 101, even when the client released and completed content
    // after its own finite wait expired.
    std::array<char, 512> completedWithoutContinueHead;
    const auto completedWithoutContinuePrepared =
        ruvia::Http1ClientRequestWriter().prepare(
            ruvia::HttpOrigin::https("example.test"),
            request,
            completedWithoutContinueHead,
            Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(completedWithoutContinuePrepared.prepared() != nullptr);
    if (completedWithoutContinuePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser completedWithoutContinueParser(
        *completedWithoutContinuePrepared.prepared());
    RUVIA_CHECK(
        completedWithoutContinueParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto completedWithoutContinue =
        completedWithoutContinueParser.parse(switching);
    RUVIA_CHECK(completedWithoutContinue.failure() != nullptr);
    if (completedWithoutContinue.failure() != nullptr) {
        RUVIA_CHECK(
            completedWithoutContinue.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> lateContinueHead;
    const auto lateContinuePrepared =
        ruvia::Http1ClientRequestWriter().prepare(
            ruvia::HttpOrigin::https("example.test"),
            request,
            lateContinueHead,
            Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(lateContinuePrepared.prepared() != nullptr);
    if (lateContinuePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser lateContinueParser(
        *lateContinuePrepared.prepared());
    RUVIA_CHECK(
        lateContinueParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto lateContinue = lateContinueParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(lateContinue.parsed() != nullptr);
    if (lateContinue.parsed() != nullptr) {
        RUVIA_CHECK(!lateContinue.parsed()->plan().requestContentSignal());
    }
    const auto acceptedAfterLateContinue = lateContinueParser.parse(switching);
    RUVIA_CHECK(acceptedAfterLateContinue.parsed() != nullptr);
    if (acceptedAfterLateContinue.parsed() != nullptr) {
        RUVIA_CHECK(
            acceptedAfterLateContinue.parsed()->plan().protocolUpgrade() !=
            nullptr);
    }

    std::array<char, 512> pendingHead;
    const auto pendingPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        pendingHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(pendingPrepared.prepared() != nullptr);
    if (pendingPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser pendingParser(*pendingPrepared.prepared());
    const auto pendingContinue = pendingParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(pendingContinue.parsed() != nullptr);
    const auto pendingUpgrade = pendingParser.parse(switching);
    RUVIA_CHECK(pendingUpgrade.failure() != nullptr);
    if (pendingUpgrade.failure() != nullptr) {
        RUVIA_CHECK(
            pendingUpgrade.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> acceptedHead;
    const auto acceptedPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        acceptedHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(acceptedPrepared.prepared() != nullptr);
    if (acceptedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser acceptedParser(*acceptedPrepared.prepared());
    const auto continueResponse = acceptedParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    RUVIA_CHECK(
        acceptedParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto accepted = acceptedParser.parse(switching);
    RUVIA_CHECK(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        RUVIA_CHECK(accepted.parsed()->plan().protocolUpgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_upgrade_requires_complete_request_content) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> incompleteHead;
    const auto incompletePrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, incompleteHead);
    RUVIA_CHECK(incompletePrepared.prepared() != nullptr);
    if (incompletePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser incompleteParser(*incompletePrepared.prepared());
    const auto incomplete = incompleteParser.parse(switching);
    RUVIA_CHECK(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        RUVIA_CHECK(
            incomplete.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> completeHead;
    const auto completePrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, completeHead);
    RUVIA_CHECK(completePrepared.prepared() != nullptr);
    if (completePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser completeParser(*completePrepared.prepared());
    RUVIA_CHECK(
        completeParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto complete = completeParser.parse(switching);
    RUVIA_CHECK(complete.parsed() != nullptr);
    if (complete.parsed() != nullptr) {
        RUVIA_CHECK(complete.parsed()->plan().protocolUpgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_switching_protocols_is_an_exclusive_upgrade_transition) {
    const ruvia::HttpHeaderView requestHeaders[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    RUVIA_CHECK(upgraded.head().status() == std::uint16_t{101});
    RUVIA_CHECK(upgraded.plan().protocolUpgrade() != nullptr);
    RUVIA_CHECK(upgraded.plan().connectTunnel() == nullptr);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    RUVIA_CHECK(versioned.plan().protocolUpgrade() != nullptr);
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders));
}

RUVIA_TEST(http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parseHead(
        "GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    RUVIA_CHECK(
        requireKnownLength(reusable.plan()).persistence() ==
        Http1ClientResponsePersistence::kReuse);
    RUVIA_CHECK(
        reusable.head().protocolVersion() ==
        HttpProtocolVersion::kHttp10);

    RUVIA_CHECK(
        parseFailureError(
            "GET",
            "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
            "Content-Length: 0") ==
        Http1ClientResponseParseError::kInvalidConnection);
    RUVIA_CHECK(
        parseFailureError(
            "GET",
            "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
            "Content-Length: 0") ==
        Http1ClientResponseParseError::kInvalidUpgrade);

    const ruvia::HttpHeaderView offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered);
    RUVIA_CHECK(upgraded.plan().protocolUpgrade() != nullptr);
}

RUVIA_TEST(http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parseHead(
        "GET", "HTTP/1.0 204 No Content");
    const auto http11 = parseHead(
        "GET", "HTTP/1.1 204 No Content");

    RUVIA_CHECK(
        http10.head().protocolVersion() ==
        HttpProtocolVersion::kHttp10);
    RUVIA_CHECK(
        http11.head().protocolVersion() ==
        HttpProtocolVersion::kHttp11);
}

RUVIA_TEST(http_client_switching_protocols_requires_wire_agreement) {
    const ruvia::HttpHeaderView offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const ruvia::HttpHeaderView closingOffer[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "websocket"},
    };
    constexpr std::string_view validResponse =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket";

    RUVIA_CHECK(parseFails("GET", validResponse));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nContent-Length: 0",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Transfer-Encoding: chunked",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        validResponse,
        Http1ClientRequestClosePolicy::kAllowReuse,
        closingOffer));
}

RUVIA_TEST(http_client_unframed_body_response_is_close_delimited) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().closeDelimited() != nullptr);
}

RUVIA_TEST(http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parseHead(
        "GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    const auto& http10Body = requireKnownLength(http10.plan());
    RUVIA_CHECK(
        http10Body.persistence() == Http1ClientResponsePersistence::kClose);

    const auto http10KeepAlive = parseHead(
        "GET",
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    RUVIA_CHECK(
        requireKnownLength(http10KeepAlive.plan()).persistence() ==
        Http1ClientResponsePersistence::kReuse);

    const auto responseClose = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    RUVIA_CHECK(
        requireKnownLength(responseClose.plan()).persistence() ==
        Http1ClientResponsePersistence::kClose);

    const auto requestClose = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 3",
        Http1ClientRequestClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(
        requireKnownLength(requestClose.plan()).persistence() ==
        Http1ClientResponsePersistence::kClose);
}

RUVIA_TEST(http_client_http10_transfer_encoding_is_faulty_framing) {
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails(
        "HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip"));
}

RUVIA_TEST(http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parseHead(
        "CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    RUVIA_CHECK(tunnel.plan().connectTunnel() != nullptr);

    const auto rejected = parseHead(
        "CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    RUVIA_CHECK(rejected.plan().knownLength() != nullptr);

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parseHead("connect", "HTTP/1.1 200 OK");
    RUVIA_CHECK(lowercase.plan().closeDelimited() != nullptr);
}

RUVIA_TEST(http_client_head_method_is_case_sensitive) {
    const auto head = parseHead("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parseHead("head", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().withoutContent() != nullptr);
    RUVIA_CHECK(lowercase.plan().closeDelimited() != nullptr);
}

RUVIA_TEST(http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::detail::HttpContentCoding;
    using ruvia::detail::httpClientResponseContentCoding;

    struct Case final {
        std::string_view headers;
        std::optional<HttpContentCoding> expected;
    };
    const Case cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
         HttpContentCoding::kBrotli},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
         HttpContentCoding::kZstd},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0",
         HttpContentCoding::kIdentity},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
         std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
         std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
         std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", HttpContentCoding::kIdentity},
    };

    for (const auto& test : cases) {
        auto parsed = parseResponse("GET", test.headers);
        RUVIA_CHECK_EQ(parsed.head.status(), std::uint16_t{200});
        const auto coding = httpClientResponseContentCoding(parsed.head);
        RUVIA_CHECK((coding.coding() != nullptr) == test.expected.has_value());
        RUVIA_CHECK((coding.unsupported() != nullptr) == !test.expected.has_value());
        if (coding.coding() != nullptr && test.expected.has_value()) {
            RUVIA_CHECK(*coding.coding() == *test.expected);
        }
    }
}

RUVIA_TEST(http_client_content_decode_reports_unsupported_wire_coding) {
    auto parsed = parseResponse(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n"
        "Content-Length: 7");
    const std::string_view encodedContent = "encoded";

    const auto decoded =
        ruvia::detail::decodeHttpClientResponseContentEncoding(
            parsed.head,
            encodedContent,
            1024,
            std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(
            decoded.failure()->error() ==
            ruvia::detail::HttpContentDecodeError::kUnsupportedCoding);
    }
}

RUVIA_TEST(http_client_content_decode_consumes_concatenated_gzip_members) {
    auto firstEncoding = ruvia::detail::encodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip,
        "first-",
        1024,
        std::pmr::get_default_resource());
    auto secondEncoding = ruvia::detail::encodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip,
        "second",
        1024,
        std::pmr::get_default_resource());
    RUVIA_CHECK(firstEncoding.encoded() != nullptr);
    RUVIA_CHECK(secondEncoding.encoded() != nullptr);
    if (firstEncoding.encoded() == nullptr ||
        secondEncoding.encoded() == nullptr) {
        return;
    }
    auto first = std::move(*firstEncoding.encoded()).takeBytes();
    auto second = std::move(*secondEncoding.encoded()).takeBytes();

    auto parsed = parseResponse(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    std::string encodedContent(first);
    encodedContent.append(second);
    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head,
        encodedContent,
        1024,
        std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view("first-second"));
    }
    // Decoding is a separate representation; the sans-I/O driver's encoded
    // content remains independent from the immutable parsed response head.
    RUVIA_CHECK(!encodedContent.empty());
    const auto coding =
        ruvia::detail::httpClientResponseContentCoding(parsed.head);
    RUVIA_CHECK(coding.coding() != nullptr);
    if (coding.coding() != nullptr) {
        RUVIA_CHECK(
            *coding.coding() == ruvia::detail::HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(http_client_content_decode_failure_preserves_encoded_body) {
    auto parsed = parseResponse(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    const std::string_view encodedContent = "not-gzip";
    const auto decoded =
        ruvia::detail::decodeHttpClientResponseContentEncoding(
            parsed.head,
            encodedContent,
            1024,
            std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    RUVIA_CHECK(
        decoded.failure()->error() ==
        ruvia::detail::HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(http_client_rejects_malformed_status_and_length_fields) {
    const auto upperBoundary = parseHead(
        "GET", "HTTP/1.1 599 Extension Status\r\nContent-Length: 0");
    RUVIA_CHECK_EQ(upperBoundary.head().status(), std::uint16_t{599});
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/2 200 OK") ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 99 Too Small") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 abc Bad") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    for (const std::string_view invalid : {
             "HTTP/1.1 600 Invalid",
             "HTTP/1.1 999 Invalid"}) {
        RUVIA_CHECK(
            parseFailureError("GET", invalid) ==
            Http1ClientResponseParseError::kInvalidStatusCode);
    }
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 200") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    std::string invalidReason("HTTP/1.1 200 ");
    invalidReason.push_back('\x01');
    RUVIA_CHECK(
        parseFailureError("GET", invalidReason) ==
        Http1ClientResponseParseError::kInvalidReasonPhrase);
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: notanumber"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5,"));
    RUVIA_CHECK(!ruvia::http1ClientResponseParseErrorMessage(
        Http1ClientResponseParseError::kInvalidStatusCode).empty());
}

RUVIA_TEST(http_client_response_parser_need_more_is_distinct) {
    const auto result = parseWire(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    RUVIA_CHECK(result.needMore() != nullptr);
    RUVIA_CHECK(result.parsed() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
}

RUVIA_TEST(http_client_response_parser_owns_exact_head_boundary) {
    std::string wire =
        "HTTP/1.1 200 OK\r\nX-Owner: response\r\nContent-Length: 4\r\n\r\n"
        "bodyHTTP/1.1 500 ignored\r\n\r\n";
    const auto expectedConsumed = wire.find("\r\n\r\n") + 4;
    auto result = parseWire("GET", wire);
    auto* parsed = result.parsed();
    RUVIA_CHECK(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->consumedBytes(), expectedConsumed);
    RUVIA_CHECK_EQ(parsed->head().status(), std::uint16_t{200});
    RUVIA_CHECK(
        parsed->head().protocolVersion() ==
        HttpProtocolVersion::kHttp11);
    RUVIA_CHECK_EQ(parsed->head().headers().size(), std::size_t{2});

    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    RUVIA_CHECK(headers[0].name() == "X-Owner");
    RUVIA_CHECK(headers[0].value() == "response");
    RUVIA_CHECK(headers[1].value() == "4");
}

RUVIA_TEST(http_client_response_parser_failure_is_typed_and_allocation_free) {
    CountingMemoryResource counting;
    ruvia::HttpClientRequest request;
    request.method = "GET";
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, requestHead);
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }

    auto failureParser = Http1ClientResponseParser(*prepared, &counting);
    const auto failure = failureParser.parse("HTTP/2 200 OK\r\n\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        failure.failure()->error() ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK_EQ(counting.allocationCount(), std::size_t{0});

    auto successParser = Http1ClientResponseParser(*prepared, &counting);
    const auto success = successParser.parse(
        "HTTP/1.1 200 OK\r\n"
        "X-Requires-Ownership: a-long-enough-value-to-require-storage\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(success.parsed() != nullptr);
    RUVIA_CHECK(counting.allocationCount() > 0);
}

RUVIA_TEST(http_client_response_parser_enforces_the_complete_head_limit) {
    std::string oversized(ruvia::kMaxHttpHeaderBytes, 'x');
    const auto result = parseWire("GET", oversized);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(
        result.failure()->error() ==
        Http1ClientResponseParseError::kHeaderTooLarge);
}
