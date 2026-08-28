#pragma once

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

namespace http_client_response_test {

using ruvia::Http1ClientRequestContentCompletionStatus;
using ruvia::Http1ClientRequestWirePolicy;
using ruvia::Http1ClientResponseParseError;
using ruvia::Http1ClientResponseParser;
using ruvia::Http1ClientResponseParseResult;
using ruvia::Http1ClosePolicy;
using ruvia::Http1ParsedClientResponseHead;
using ruvia::HttpClientRequestContentSignal;
using ruvia::HttpClientResponseHead;
using ruvia::HttpProtocolVersion;

template <typename T>
concept HasAnyRvalueHttp1ClientResponseParseAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).parsed(); } || requires(const T&& result) { std::move(result).parsed(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ClientResponsePlanAccessor = requires(T&& plan) { std::move(plan).informational(); } || requires(T&& plan) { std::move(plan).withoutContent(); } || requires(T&& plan) { std::move(plan).zeroContent(); } || requires(T&& plan) { std::move(plan).knownLength(); } || requires(T&& plan) { std::move(plan).chunked(); } || requires(T&& plan) { std::move(plan).closeDelimited(); } || requires(T&& plan) { std::move(plan).connectTunnel(); } || requires(T&& plan) { std::move(plan).protocolUpgrade(); };

template <typename T>
concept HasAnyRvalueHttp1ParsedClientResponseBorrow = requires(T&& parsed) { std::move(parsed).head(); } || requires(T&& parsed) { std::move(parsed).plan(); };

static_assert(!HasAnyRvalueHttp1ClientResponseParseAccessor<Http1ClientResponseParseResult>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<ruvia::Http1ClientResponsePlan>);
static_assert(!HasAnyRvalueHttp1ClientResponsePlanAccessor<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!HasAnyRvalueHttp1ParsedClientResponseBorrow<Http1ParsedClientResponseHead>);

template <typename Access>
concept CanMutateHttpClientResponseHeadStatus = requires(HttpClientResponseHead& head) { Access::setStatus(head, std::uint16_t{200}); };

static_assert(!CanMutateHttpClientResponseHeadStatus<ruvia::detail::HttpClientResponseHeadAccess>);

inline Http1ClientResponseParseResult parseWire(std::string_view method, std::string_view wire, Http1ClosePolicy closePolicy = Http1ClosePolicy::kAllowReuse, std::span<const ruvia::HttpHeaderView> requestHeaders = {}, std::pmr::memory_resource* resource = nullptr) {
    std::array<char, 2048> requestHead;
    const auto origin = ruvia::HttpOriginView::https({.host = "example.test"});
    ruvia::HttpClientRequestView request;
    request.method = method;
    request.headers = requestHeaders;
    const auto preparedResult = method == "CONNECT" ? ruvia::Http1ClientRequestWriter().prepareConnect(origin, requestHeaders, requestHead, Http1ClientRequestWirePolicy{.closePolicy = closePolicy}) : ruvia::Http1ClientRequestWriter().prepare(origin, request, requestHead, Http1ClientRequestWirePolicy{.closePolicy = closePolicy});
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        throw std::runtime_error("test request could not be prepared");
    }
    auto parser = Http1ClientResponseParser(prepared->exchangeState(), {.resource = resource});
    return parser.parse(wire);
}

inline Http1ClientResponseParseResult parseResult(std::string_view method, std::string_view headerSection, Http1ClosePolicy closePolicy = Http1ClosePolicy::kAllowReuse, std::span<const ruvia::HttpHeaderView> requestHeaders = {}, std::pmr::memory_resource* resource = nullptr) {
    std::string wire(headerSection);
    wire.append("\r\n\r\n");
    return parseWire(method, wire, closePolicy, requestHeaders, resource);
}

inline Http1ParsedClientResponseHead parseHead(std::string_view method, std::string_view headerSection, Http1ClosePolicy closePolicy = Http1ClosePolicy::kAllowReuse, std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    auto result = parseResult(method, headerSection, closePolicy, requestHeaders);
    auto* parsed = result.parsed();
    if (parsed == nullptr) {
        throw std::runtime_error("test expected a parsed HTTP/1 response head");
    }
    return std::move(*parsed);
}

struct ParsedResponse final {
    HttpClientResponseHead head;
};

inline ParsedResponse parseResponse(std::string_view method, std::string_view headerSection) {
    auto head = parseHead(method, headerSection);
    return ParsedResponse{std::move(head).takeHead()};
}

inline bool parseFails(std::string_view method, std::string_view headerSection, Http1ClosePolicy closePolicy = Http1ClosePolicy::kAllowReuse, std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    const auto result = parseResult(method, headerSection, closePolicy, requestHeaders);
    return result.failure() != nullptr;
}

inline const ruvia::Http1ClientKnownLengthResponse& requireKnownLength(const ruvia::Http1ClientResponsePlan& plan) {
    const auto* knownLength = plan.knownLength();
    if (knownLength == nullptr) {
        throw std::runtime_error("test expected exact-length response framing");
    }
    return *knownLength;
}

inline const ruvia::Http1ClientChunkedResponse& requireChunked(const ruvia::Http1ClientResponsePlan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked response framing");
    }
    return *chunked;
}

inline const ruvia::Http1ClientCloseDelimitedResponse& requireCloseDelimited(const ruvia::Http1ClientResponsePlan& plan) {
    const auto* closeDelimited = plan.closeDelimited();
    if (closeDelimited == nullptr) {
        throw std::runtime_error("test expected close-delimited response framing");
    }
    return *closeDelimited;
}

inline const ruvia::Http1ClientResponseWithoutContent& requireWithoutContent(const ruvia::Http1ClientResponsePlan& plan) {
    const auto* withoutContent = plan.withoutContent();
    if (withoutContent == nullptr) {
        throw std::runtime_error("test expected a final response without content");
    }
    return *withoutContent;
}

inline const ruvia::Http1ClientResponseWithZeroContent& requireZeroContent(const ruvia::Http1ClientResponsePlan& plan) {
    const auto* zeroContent = plan.zeroContent();
    if (zeroContent == nullptr) {
        throw std::runtime_error("test expected a response with framed zero content");
    }
    return *zeroContent;
}

inline std::size_t activePlanAlternativeCount(const ruvia::Http1ClientResponsePlan& plan) noexcept {
    return static_cast<std::size_t>(plan.informational() != nullptr) + static_cast<std::size_t>(plan.withoutContent() != nullptr) + static_cast<std::size_t>(plan.zeroContent() != nullptr) + static_cast<std::size_t>(plan.knownLength() != nullptr) + static_cast<std::size_t>(plan.chunked() != nullptr) + static_cast<std::size_t>(plan.closeDelimited() != nullptr) + static_cast<std::size_t>(plan.connectTunnel() != nullptr) + static_cast<std::size_t>(plan.protocolUpgrade() != nullptr);
}

inline Http1ClientResponseParseError parseFailureError(std::string_view method, std::string_view headerSection, std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    const auto result = parseResult(method, headerSection, Http1ClosePolicy::kAllowReuse, requestHeaders);
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

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t allocationCount_{0};
};

template <typename T>
concept HasResponsePlanMode = requires(const T& value) { value.mode(); };

template <typename T>
concept HasResponseConnectionDisposition = requires(const T& value) { value.connectionDisposition(); };

template <typename T>
concept HasResponseContentLength = requires(const T& value) { value.contentLength(); };

template <typename T>
concept HasResponseTransferCodings = requires(const T& value) {
    { value.transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
} && requires(const T&& value) {
    { std::move(value).transferCodings() } -> std::same_as<ruvia::HttpTransferCodings>;
};

template <typename T>
concept HasResponsePersistence = requires(const T& value) { value.persistence(); };

template <typename T>
concept ExposesAnyRvalueHttpClientOwnedView = requires(T&& value) { std::move(value).name(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).headers(); } || requires(T&& value) { std::move(value).body(); };

template <typename T>
concept HasHttpClientResponseBody = requires(const T& head) {
    { head.body() } -> std::same_as<std::string_view>;
};

static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHeader>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::HttpClientResponseHead>);
static_assert(!HasHttpClientResponseBody<ruvia::HttpClientResponseHead>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientChunkedResponse>);
static_assert(!ExposesAnyRvalueHttpClientOwnedView<ruvia::Http1ClientCloseDelimitedResponse>);

static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponsePlan>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientInformationalResponse>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponseWithoutContent>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientResponseWithZeroContent>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientChunkedResponse>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientConnectTunnel>);
static_assert(!std::is_default_constructible_v<ruvia::Http1ClientProtocolUpgrade>);
static_assert(!HasResponsePlanMode<ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseConnectionDisposition<ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseContentLength<ruvia::Http1ClientResponsePlan>);
static_assert(!HasResponseTransferCodings<ruvia::Http1ClientResponsePlan>);
static_assert(HasResponseContentLength<ruvia::Http1ClientKnownLengthResponse>);
static_assert(!HasResponseContentLength<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasResponseContentLength<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(HasResponseTransferCodings<ruvia::Http1ClientChunkedResponse>);
static_assert(HasResponseTransferCodings<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(!HasResponseTransferCodings<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasResponsePersistence<ruvia::Http1ClientResponseWithoutContent>);
static_assert(HasResponsePersistence<ruvia::Http1ClientKnownLengthResponse>);
static_assert(HasResponsePersistence<ruvia::Http1ClientChunkedResponse>);
static_assert(!HasResponsePersistence<ruvia::Http1ClientCloseDelimitedResponse>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().zeroContent()), const ruvia::Http1ClientResponseWithZeroContent*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().knownLength()), const ruvia::Http1ClientKnownLengthResponse*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().connectTunnel()), const ruvia::Http1ClientConnectTunnel*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Http1ClientResponsePlan&>().protocolUpgrade()), const ruvia::Http1ClientProtocolUpgrade*>);

}  // namespace http_client_response_test

using namespace http_client_response_test;  // NOLINT(google-build-using-namespace)
