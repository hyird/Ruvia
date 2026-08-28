#include "ruvia/http/Http1ClientResponseParser.h"

#include "ruvia/http/detail/client/HttpClientResponseHead.h"

#include <variant>

#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/client/HttpClientResponseLimits.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

struct Http1ClientResponseParseResultAccess final {
    [[nodiscard]] static Http1ClientResponseParseResult needMore() noexcept {
        return Http1ClientResponseParseResult(Http1ClientResponseNeedMore());
    }

    [[nodiscard]] static Http1ClientResponseParseResult failure(Http1ClientResponseParseError error) noexcept {
        return Http1ClientResponseParseResult(Http1ClientResponseParseFailure(error));
    }

    [[nodiscard]] static Http1ClientResponseParseResult terminal(bool completed) noexcept {
        return Http1ClientResponseParseResult(Http1ClientResponseParseTerminal(completed));
    }

    [[nodiscard]] static Http1ClientResponseParseResult parsed(HttpClientResponseHead head, Http1ClientResponsePlan plan, std::size_t consumedBytes) noexcept {
        return Http1ClientResponseParseResult(Http1ParsedClientResponseHead(std::move(head), std::move(plan), consumedBytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::Http1ClientRequestContentPhase receiveContinue(detail::Http1ClientRequestContentPhase phase) noexcept {
    switch (phase) {
        case detail::Http1ClientRequestContentPhase::kAwaitingContinue:
            return detail::Http1ClientRequestContentPhase::kContinueReceived;
        case detail::Http1ClientRequestContentPhase::kContentCompleteAwaitingContinue:
            return detail::Http1ClientRequestContentPhase::kContinueReceivedContentComplete;
        case detail::Http1ClientRequestContentPhase::kContentComplete:
        case detail::Http1ClientRequestContentPhase::kContentPending:
        case detail::Http1ClientRequestContentPhase::kContinueReceived:
        case detail::Http1ClientRequestContentPhase::kContinueReceivedContentComplete:
            return phase;
    }
    return phase;
}

}  // namespace
namespace {}  // namespace

std::string_view http1ClientResponseParseErrorMessage(Http1ClientResponseParseError error) noexcept {
    switch (error) {
        case Http1ClientResponseParseError::kHeaderTooLarge:
            return "response header is too large";
        case Http1ClientResponseParseError::kInvalidStatusLine:
            return "invalid response status line";
        case Http1ClientResponseParseError::kUnsupportedHttpVersion:
            return "unsupported response HTTP version";
        case Http1ClientResponseParseError::kInvalidStatusCode:
            return "invalid response status code";
        case Http1ClientResponseParseError::kInvalidReasonPhrase:
            return "invalid response reason phrase";
        case Http1ClientResponseParseError::kInvalidHeader:
            return "invalid response header";
        case Http1ClientResponseParseError::kInvalidConnection:
            return "invalid response Connection header";
        case Http1ClientResponseParseError::kInvalidUpgrade:
            return "invalid response Upgrade header";
        case Http1ClientResponseParseError::kTooManyHeaders:
            return "too many response headers";
        case Http1ClientResponseParseError::kInvalidContentLength:
            return "invalid response Content-Length";
        case Http1ClientResponseParseError::kConflictingContentLength:
            return "conflicting response Content-Length";
        case Http1ClientResponseParseError::kInvalidTransferEncoding:
            return "invalid response Transfer-Encoding";
        case Http1ClientResponseParseError::kUnsupportedTransferEncoding:
            return "unsupported response transfer coding";
        case Http1ClientResponseParseError::kTransferEncodingInHttp10:
            return "Transfer-Encoding in HTTP/1.0 response";
        case Http1ClientResponseParseError::kContentLengthAndTransferEncoding:
            return "response has both Content-Length and Transfer-Encoding";
        case Http1ClientResponseParseError::kInvalidProtocolSwitch:
            return "invalid Switching Protocols response";
        case Http1ClientResponseParseError::kTooManyInformationalResponses:
            return "too many informational responses";
    }
    return "invalid HTTP/1 response";
}

Http1ClientResponseParseResult Http1ClientResponseParser::parse(std::string_view buffer) {
    if (phase_ == Phase::kComplete) {
        return detail::Http1ClientResponseParseResultAccess::terminal(true);
    }
    if (phase_ == Phase::kFailed) {
        return detail::Http1ClientResponseParseResultAccess::terminal(false);
    }
    const auto fail = [this](Http1ClientResponseParseError error) noexcept {
        phase_ = Phase::kFailed;
        return detail::Http1ClientResponseParseResultAccess::failure(error);
    };

    const auto headerBytes = detail::findHttpHeaderEnd(buffer, 0);
    if (headerBytes == std::string_view::npos) {
        if (buffer.size() >= kMaxHttpHeaderBytes) {
            return fail(Http1ClientResponseParseError::kHeaderTooLarge);
        }
        return detail::Http1ClientResponseParseResultAccess::needMore();
    }
    if (headerBytes > kMaxHttpHeaderBytes) {
        return fail(Http1ClientResponseParseError::kHeaderTooLarge);
    }

    // Remove the terminal CRLF CRLF. The last field line then has the same
    // shape as every preceding line except that it has no trailing delimiter.
    const auto headSection = buffer.substr(0, headerBytes - 4);
    auto parsedHead = detail::parseHttp1ClientResponseHeadFields(headSection, exchangeState_);
    if (const auto* parseError = std::get_if<Http1ClientResponseParseError>(&parsedHead)) {
        return fail(*parseError);
    }
    const auto& parsed = std::get<detail::Http1ClientParsedResponseHead>(parsedHead);

    auto planning = detail::planHttp1ClientResponse(exchangeState_, parsed, requestContentPhase_);
    if (const auto* planningError = std::get_if<Http1ClientResponseParseError>(&planning)) {
        return fail(*planningError);
    }
    auto plan = std::get<Http1ClientResponsePlan>(std::move(planning));
    const auto* const informationalPlan = plan.informational();
    const bool informational = informationalPlan != nullptr;
    const bool closingInformational = informational && informationalPlan->persistence() == Http1ClosePolicy::kCloseAfterResponse;
    if (informational && informationalResponseCount_ >= detail::kMaxHttpClientInterimResponses) {
        return fail(Http1ClientResponseParseError::kTooManyInformationalResponses);
    }

    // No owning response head is observable until the entire head and framing
    // plan have validated. Protocol failure therefore has no partially mutated
    // out-parameter and performs no PMR allocation.
    auto head = detail::HttpClientResponseHeadAccess::make(parsed.statusCode, parsed.protocolVersion, resource_);
    auto& headers = detail::HttpClientResponseHeadAccess::headers(head);
    if (parsed.headerCount != 0) {
        headers.reserve(parsed.headerCount);
    }
    for (std::size_t i = 0; i < parsed.headerCount; ++i) {
        const auto& header = parsed.headers[i];
        headers.emplace_back(detail::HttpClientResponseHeaderAccess::make(header.name(), header.value(), resource_));
    }

    auto result = detail::Http1ClientResponseParseResultAccess::parsed(std::move(head), std::move(plan), headerBytes);
    if (informational) {
        ++informationalResponseCount_;
    }
    if (parsed.statusCode == http_status::kContinue && !closingInformational) {
        requestContentPhase_ = receiveContinue(requestContentPhase_);
    }
    if (closingInformational || parsed.statusCode == http_status::kSwitchingProtocols || parsed.statusCode.isFinal()) {
        phase_ = Phase::kComplete;
    }
    return result;
}

Http1ClientRequestContentCompletionStatus Http1ClientResponseParser::completeRequestContent() noexcept {
    if (phase_ != Phase::kAwaitResponse) {
        return Http1ClientRequestContentCompletionStatus::kExchangeTerminal;
    }
    switch (requestContentPhase_) {
        case detail::Http1ClientRequestContentPhase::kContentPending:
            requestContentPhase_ = detail::Http1ClientRequestContentPhase::kContentComplete;
            return Http1ClientRequestContentCompletionStatus::kCompleted;
        case detail::Http1ClientRequestContentPhase::kAwaitingContinue:
            requestContentPhase_ = detail::Http1ClientRequestContentPhase::kContentCompleteAwaitingContinue;
            return Http1ClientRequestContentCompletionStatus::kCompleted;
        case detail::Http1ClientRequestContentPhase::kContinueReceived:
            requestContentPhase_ = detail::Http1ClientRequestContentPhase::kContinueReceivedContentComplete;
            return Http1ClientRequestContentCompletionStatus::kCompleted;
        case detail::Http1ClientRequestContentPhase::kContentComplete:
        case detail::Http1ClientRequestContentPhase::kContentCompleteAwaitingContinue:
        case detail::Http1ClientRequestContentPhase::kContinueReceivedContentComplete:
            return Http1ClientRequestContentCompletionStatus::kAlreadyComplete;
    }
    return Http1ClientRequestContentCompletionStatus::kAlreadyComplete;
}

}  // namespace ruvia
