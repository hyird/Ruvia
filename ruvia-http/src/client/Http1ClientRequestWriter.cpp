#include "ruvia/http/Http1ClientRequestWriter.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <system_error>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/client/Http1ClientRequestHeaders.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/client/HttpOriginView.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

struct Http1ClientRequestPrepareResultAccess final {
    [[nodiscard]] static Http1ClientExchangeState exchangeState(HttpKnownMethod method,
        HttpConnectionOptions connectionOptions, Http1ClosePolicy closePolicy,
        Http1ClientInitialContentState contentState, std::pmr::string offeredUpgradeProtocols) noexcept {
        return Http1ClientExchangeState(method, connectionOptions, closePolicy, contentState,
            std::move(offeredUpgradeProtocols));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult bufferTooSmall(std::size_t requiredHeadBytes) noexcept {
        return Http1ClientRequestPrepareResult(Http1ClientRequestBufferTooSmall(requiredHeadBytes));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult failure(Http1ClientRequestPrepareError error) noexcept {
        return Http1ClientRequestPrepareResult(Http1ClientRequestPrepareFailure(error));
    }

    [[nodiscard]] static Http1ClientRequestPrepareResult preparedWithoutContent(std::string_view head, Http1ClientExchangeState exchangeState) noexcept {
        return Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest(head,
            Http1ClientRequestContentPlan(Http1ClientRequestWithoutContent()), std::move(exchangeState)));
    }

    [[nodiscard]] static Http1ClientRequestPrepareResult preparedImmediateContent(std::string_view head,
        std::string_view contentBytes, Http1ClientExchangeState exchangeState) noexcept {
        return Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest(head,
            Http1ClientRequestContentPlan(Http1ClientImmediateRequestContent(contentBytes)), std::move(exchangeState)));
    }

    [[nodiscard]] static Http1ClientRequestPrepareResult preparedContinueGatedContent(std::string_view head,
        std::string_view contentBytes, Http1ClientExchangeState exchangeState) noexcept {
        return Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest(head,
            Http1ClientRequestContentPlan(Http1ClientContinueGatedRequestContent(contentBytes)), std::move(exchangeState)));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view kHttp11RequestLineSuffix = " HTTP/1.1\r\n";
constexpr std::string_view kHostPrefix = "Host: ";
constexpr std::string_view kContentLengthPrefix = "Content-Length: ";
constexpr std::string_view kExpectPrefix = "Expect: ";
constexpr std::string_view kConnectionClose = "Connection: close\r\n";

[[nodiscard]] std::size_t authorityLength(const HttpOriginView& origin, bool forcePort) noexcept {
    return origin.host().size() + ((forcePort || !detail::httpOriginUsesDefaultPort(origin)) ? 1 + decimalDigits(origin.port()) : 0);
}

void appendView(char*& cursor, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(cursor, value.data(), value.size());
        cursor += value.size();
    }
}

void appendUnsigned(char*& cursor, std::size_t value) noexcept {
    std::array<char, 32> digits;
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec == std::errc{}) {
        appendView(cursor, std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
    }
}

void appendAuthority(char*& cursor, const HttpOriginView& origin, bool forcePort) noexcept {
    appendView(cursor, origin.host());
    if (forcePort || !detail::httpOriginUsesDefaultPort(origin)) {
        *cursor++ = ':';
        appendUnsigned(cursor, origin.port());
    }
}

void appendHeaders(char*& cursor, std::span<const HttpHeaderView> headers) noexcept {
    for (const auto& header : headers) {
        appendView(cursor, header.name());
        appendView(cursor, ": ");
        appendView(cursor, header.value());
        appendView(cursor, kCrlf);
    }
}

[[nodiscard]] bool isValidHttp1ClosePolicy(Http1ClosePolicy policy) noexcept {
    switch (policy) {
        case Http1ClosePolicy::kAllowReuse:
        case Http1ClosePolicy::kCloseAfterResponse:
            return true;
    }
    return false;
}

[[nodiscard]] std::pmr::string ownOfferedUpgradeProtocols(std::span<const HttpHeaderView> headers,
    std::pmr::memory_resource* resource) {
    std::pmr::string result(resource);
    for (const auto& header : headers) {
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(header.value());
    }
    return result;
}

[[nodiscard]] Http1ClientRequestPrepareResult prepareRequest(const HttpOriginView& origin,
    std::string_view method, std::string_view target, bool connect,
    std::span<const HttpHeaderView> headers, HttpClientRequestContentView content,
    std::span<char> headBuffer, Http1ClientRequestWirePolicy policy,
    std::pmr::memory_resource* resource) {
    RequestHeaderFacts headerFacts;
    Http1ClientRequestPrepareError error = Http1ClientRequestPrepareError::kInvalidHeader;
    const auto* contentBytes = content.borrowedBytes();
    const bool explicitContent = contentBytes != nullptr;
    if (!analyzeHeaders(headers, headerFacts, error)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(error);
    }
    const bool expectContinue = policy.expectation == HttpClientRequestExpectation::kContinue;
    const auto contentIndication = explicitContent && !contentBytes->value().empty() ? HttpRequestContentIndication::kWillFollow : HttpRequestContentIndication::kNoContent;
    if (!httpClientExpectationIsValid(expectContinue, contentIndication)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }
    if (explicitContent) {
        const auto contentSemantics = detail::httpRequestContentSemantics(method);
        if (contentSemantics == detail::HttpRequestContentSemantics::kForbidden) {
            return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kContentForbiddenForMethod);
        }
        if (contentSemantics == detail::HttpRequestContentSemantics::kContentTypeRequired && !headerFacts.hasContentType) {
            return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kOptionsContentTypeRequired);
        }
    }

    const bool generateConnectionClose = policy.closePolicy == Http1ClosePolicy::kCloseAfterResponse && !headerFacts.connectionOptions.close();
    const auto effectiveClosePolicy = headerFacts.connectionOptions.close() || generateConnectionClose ? Http1ClosePolicy::kCloseAfterResponse : Http1ClosePolicy::kAllowReuse;
    const std::size_t generatedFields = 1 + (explicitContent ? 1 : 0) + (expectContinue ? 1 : 0) + (generateConnectionClose ? 1 : 0);
    if (headers.size() > kMaxHttpHeaderFields - generatedFields) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kTooManyHeaders);
    }

    const std::size_t targetBytes = connect ? authorityLength(origin, true) : target.size();
    std::size_t headBytes = 0;
    if (!addHeadBytes(headBytes, method.size()) || !addHeadBytes(headBytes, 1) || !addHeadBytes(headBytes, targetBytes) || !addHeadBytes(headBytes, kHttp11RequestLineSuffix.size()) || !addHeadBytes(headBytes, kHostPrefix.size()) || !addHeadBytes(headBytes, authorityLength(origin, false)) || !addHeadBytes(headBytes, kCrlf.size()) || !addHeadBytes(headBytes, headerFacts.wireBytes) || (explicitContent && (!addHeadBytes(headBytes, kContentLengthPrefix.size()) || !addHeadBytes(headBytes, decimalDigits(contentBytes->value().size())) || !addHeadBytes(headBytes, kCrlf.size()))) || (expectContinue && (!addHeadBytes(headBytes, kExpectPrefix.size()) || !addHeadBytes(headBytes, detail::kHttpContinueExpectationToken.size()) || !addHeadBytes(headBytes, kCrlf.size()))) || (generateConnectionClose && !addHeadBytes(headBytes, kConnectionClose.size())) || !addHeadBytes(headBytes, kCrlf.size())) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kHeaderTooLarge);
    }
    if (headBuffer.size() < headBytes) {
        return detail::Http1ClientRequestPrepareResultAccess::bufferTooSmall(headBytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offeredUpgradeProtocols = headerFacts.upgradeProtocols.hasProtocol()
        ? ownOfferedUpgradeProtocols(headers, resource)
        : std::pmr::string(resource);

    char* cursor = headBuffer.data();
    appendView(cursor, method);
    *cursor++ = ' ';
    if (connect) {
        appendAuthority(cursor, origin, true);
    } else {
        appendView(cursor, target);
    }
    appendView(cursor, kHttp11RequestLineSuffix);
    appendView(cursor, kHostPrefix);
    appendAuthority(cursor, origin, false);
    appendView(cursor, kCrlf);
    appendHeaders(cursor, headers);
    if (explicitContent) {
        appendView(cursor, kContentLengthPrefix);
        appendUnsigned(cursor, contentBytes->value().size());
        appendView(cursor, kCrlf);
    }
    if (expectContinue) {
        appendView(cursor, kExpectPrefix);
        appendView(cursor, detail::kHttpContinueExpectationToken);
        appendView(cursor, kCrlf);
    }
    if (generateConnectionClose) {
        appendView(cursor, kConnectionClose);
    }
    appendView(cursor, kCrlf);

    const auto contentState = expectContinue
        ? detail::Http1ClientInitialContentState::kAwaitingContinue
        : (explicitContent && !contentBytes->value().empty()
                  ? detail::Http1ClientInitialContentState::kPending
                  : detail::Http1ClientInitialContentState::kComplete);
    auto exchangeState = detail::Http1ClientRequestPrepareResultAccess::exchangeState(
        connect ? HttpKnownMethod::kConnect : classifyHttpMethod(method), headerFacts.connectionOptions,
        effectiveClosePolicy, contentState, std::move(offeredUpgradeProtocols));
    const auto head = std::string_view(headBuffer.data(), headBytes);
    if (!explicitContent) {
        return detail::Http1ClientRequestPrepareResultAccess::preparedWithoutContent(head, std::move(exchangeState));
    }
    if (expectContinue) {
        return detail::Http1ClientRequestPrepareResultAccess::preparedContinueGatedContent(head,
            contentBytes->value(), std::move(exchangeState));
    }
    return detail::Http1ClientRequestPrepareResultAccess::preparedImmediateContent(head,
        contentBytes->value(), std::move(exchangeState));
}

}  // namespace

std::string_view http1ClientRequestPrepareErrorMessage(Http1ClientRequestPrepareError error) noexcept {
    switch (error) {
        case Http1ClientRequestPrepareError::kInvalidMethod:
            return "invalid HTTP/1 client request method";
        case Http1ClientRequestPrepareError::kInvalidTarget:
            return "invalid HTTP/1 client request target";
        case Http1ClientRequestPrepareError::kConnectRequiresDedicatedEntry:
            return "CONNECT requires the dedicated HTTP/1 client entry";
        case Http1ClientRequestPrepareError::kInvalidConnectOrigin:
            return "invalid HTTP/1 CONNECT origin";
        case Http1ClientRequestPrepareError::kInvalidHeader:
            return "invalid HTTP/1 client request header";
        case Http1ClientRequestPrepareError::kTooManyHeaders:
            return "too many HTTP/1 client request headers";
        case Http1ClientRequestPrepareError::kHostHeaderManagedByWriter:
            return "HTTP/1 client Host is managed by the writer";
        case Http1ClientRequestPrepareError::kContentLengthManagedByWriter:
            return "HTTP/1 client Content-Length is managed by the writer";
        case Http1ClientRequestPrepareError::kTransferEncodingUnsupported:
            return "HTTP/1 client Transfer-Encoding is unsupported";
        case Http1ClientRequestPrepareError::kTrailerSectionUnsupported:
            return "HTTP/1 client trailer sections are unsupported";
        case Http1ClientRequestPrepareError::kExpectHeaderManagedByWriter:
            return "HTTP/1 client Expect is managed by the writer";
        case Http1ClientRequestPrepareError::kInvalidConnection:
            return "invalid HTTP/1 client Connection header";
        case Http1ClientRequestPrepareError::kInvalidUpgrade:
            return "invalid HTTP/1 client Upgrade header";
        case Http1ClientRequestPrepareError::kUpgradeConnectionOptionRequired:
            return "HTTP/1 Upgrade requires Connection: Upgrade";
        case Http1ClientRequestPrepareError::kTeConnectionOptionRequired:
            return "HTTP/1 TE requires Connection: TE";
        case Http1ClientRequestPrepareError::kExpectationWithoutContent:
            return "Continue expectation requires non-empty request content";
        case Http1ClientRequestPrepareError::kContentForbiddenForMethod:
            return "request method forbids content";
        case Http1ClientRequestPrepareError::kOptionsContentTypeRequired:
            return "OPTIONS content requires Content-Type";
        case Http1ClientRequestPrepareError::kHeaderTooLarge:
            return "HTTP/1 client request header is too large";
        case Http1ClientRequestPrepareError::kInvalidClosePolicy:
            return "invalid HTTP/1 client close policy";
    }
    return "invalid HTTP/1 client request";
}

Http1ClientRequestWriter::Http1ClientRequestWriter() noexcept
    : Http1ClientRequestWriter(Options{}) {}

Http1ClientRequestWriter::Http1ClientRequestWriter(Options options) noexcept
    : resource_(detail::httpPmrResourceOrDefault(options.resource)) {}

Http1ClientRequestPrepareResult Http1ClientRequestWriter::prepare(const HttpOriginView& origin,
    const HttpClientRequestView& request, std::span<char> headBuffer,
    Http1ClientRequestWirePolicy policy) const {
    if (!isValidHttp1ClosePolicy(policy.closePolicy)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kInvalidClosePolicy);
    }
    if (!isValidHttpMethodToken(request.method)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kInvalidMethod);
    }
    if (request.method == "CONNECT") {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kConnectRequiresDedicatedEntry);
    }
    if (!detail::isValidOriginOrAsteriskFormTarget(classifyHttpMethod(request.method), request.target)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kInvalidTarget);
    }
    return prepareRequest(origin, request.method, request.target, false,
        static_cast<std::span<const HttpHeaderView>>(request.headers), request.content, headBuffer, policy,
        resource_);
}

Http1ClientRequestPrepareResult Http1ClientRequestWriter::prepareConnect(const HttpOriginView& tunnelOrigin,
    std::span<const HttpHeaderView> headers, std::span<char> headBuffer,
    Http1ClientRequestWirePolicy policy) const {
    if (!isValidHttp1ClosePolicy(policy.closePolicy)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kInvalidClosePolicy);
    }
    if (tunnelOrigin.port() == 0) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(Http1ClientRequestPrepareError::kInvalidConnectOrigin);
    }
    return prepareRequest(tunnelOrigin, "CONNECT", {}, true, headers, HttpClientRequestContentView::none(),
        headBuffer, policy, resource_);
}

}  // namespace ruvia
