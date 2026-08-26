#include "ruvia/http/detail/client/HttpClientResponseHead.h"

#include <optional>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool requestOffersProtocol(
    const Http1ClientExchangeState& exchangeState, const HttpUpgradeProtocol& selected) noexcept {
    const auto offeredProtocols =
        Http1ClientExchangeStateAccess::offeredUpgradeProtocols(exchangeState);
    if (offeredProtocols.empty()) {
        return false;
    }
    bool offered = false;
    HttpUpgradeProtocols protocols;
    if (protocols.parseField(offeredProtocols, HttpFieldListRole::kSender,
            [&selected, &offered](const HttpUpgradeProtocol& candidate) noexcept {
                offered = offered || httpUpgradeProtocolEquals(candidate, selected);
                return true;
            }) != HttpFieldListParseStatus::kOk) {
        return false;
    }
    return protocols.hasProtocol() && offered;
}

[[nodiscard]] bool requestAllowsProtocolSwitch(const Http1ClientExchangeState& exchangeState,
    const Http1ClientParsedResponseHead& response,
    Http1ClientRequestContentPhase requestContentPhase) noexcept {
    const bool requestContentAllowsSwitch =
        requestContentPhase == Http1ClientRequestContentPhase::kContentComplete ||
        requestContentPhase == Http1ClientRequestContentPhase::kContinueReceivedContentComplete;
    if (Http1ClientExchangeStateAccess::closePolicy(exchangeState) ==
            Http1ClosePolicy::kCloseAfterResponse ||
        !requestContentAllowsSwitch) {
        return false;
    }
    if (!Http1ClientExchangeStateAccess::connectionOptions(exchangeState).upgrade() ||
        response.connectionOptions.close() || !response.connectionOptions.upgrade() ||
        !response.upgradeProtocols.hasProtocol()) {
        return false;
    }

    HttpUpgradeProtocols selectedProtocols;
    for (std::size_t i = 0; i < response.headerCount; ++i) {
        const auto& header = response.headers[i];
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            continue;
        }
        if (selectedProtocols.parseField(header.value(), HttpFieldListRole::kRecipient,
                [&exchangeState](const HttpUpgradeProtocol& selected) noexcept {
                    return requestOffersProtocol(exchangeState, selected);
                }) != HttpFieldListParseStatus::kOk) {
            return false;
        }
    }
    return selectedProtocols.hasProtocol();
}

[[nodiscard]] std::optional<HttpClientRequestContentSignal> requestContentSignal(
    Http1ClientRequestContentPhase phase, HttpStatusCode statusCode,
    bool responseWillClose) noexcept {
    if (responseWillClose && (phase == Http1ClientRequestContentPhase::kAwaitingContinue ||
                                 phase == Http1ClientRequestContentPhase::kContentPending ||
                                 phase == Http1ClientRequestContentPhase::kContinueReceived)) {
        return HttpClientRequestContentSignal::kExchangeComplete;
    }
    if (statusCode == http_status::kContinue) {
        return phase == Http1ClientRequestContentPhase::kAwaitingContinue
                   ? std::optional<HttpClientRequestContentSignal>(
                         HttpClientRequestContentSignal::kContinue)
                   : std::nullopt;
    }
    if (statusCode.isFinal()) {
        // A final response cancels content only while Expect still gates it.
        // Once 100 Continue releases the writer, RFC 9110 section 7.5 says the
        // client should keep sending the request unless the server explicitly
        // indicates otherwise. RFC 9112 section 9.5 makes a closing final
        // response that explicit signal, including after Continue released the
        // body writer.
        if (phase == Http1ClientRequestContentPhase::kAwaitingContinue ||
            (responseWillClose &&
                (phase == Http1ClientRequestContentPhase::kContentPending ||
                    phase == Http1ClientRequestContentPhase::kContinueReceived))) {
            return HttpClientRequestContentSignal::kExchangeComplete;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Http1ClosePolicy responsePersistence(
    const Http1ClientParsedResponseHead& response) noexcept {
    if (response.connectionOptions.close()) {
        return Http1ClosePolicy::kCloseAfterResponse;
    }
    if (response.protocolVersion == HttpProtocolVersion::kHttp11 ||
        response.connectionOptions.keepAlive()) {
        return Http1ClosePolicy::kAllowReuse;
    }
    return Http1ClosePolicy::kCloseAfterResponse;
}

[[nodiscard]] Http1ClosePolicy finalResponsePersistence(
    const Http1ClientExchangeState& exchangeState,
    const Http1ClientParsedResponseHead& response) noexcept {
    if (Http1ClientExchangeStateAccess::closePolicy(exchangeState) ==
        Http1ClosePolicy::kCloseAfterResponse) {
        return Http1ClosePolicy::kCloseAfterResponse;
    }
    return responsePersistence(response);
}

}  // namespace

struct Http1ClientResponsePlanAccess final {
    using RequestContentSignal = std::optional<HttpClientRequestContentSignal>;

    [[nodiscard]] static Http1ClientResponsePlan informational(
        Http1ClosePolicy persistence, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientInformationalResponse(persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan withoutContent(
        Http1ClosePolicy persistence, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientResponseWithoutContent(persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan zeroContentKnownLength(
        Http1ClosePolicy persistence, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientResponseWithZeroContent(Http1ClientResponseWithZeroContent::Framing(
                    Http1ClientKnownLengthResponse(0, persistence)))),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan zeroContentChunked(
        HttpTransferCodings transferCodings, Http1ClosePolicy persistence,
        RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientResponseWithZeroContent(Http1ClientResponseWithZeroContent::Framing(
                    Http1ClientChunkedResponse(transferCodings, persistence)))),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan zeroContentCloseDelimited(
        HttpTransferCodings transferCodings, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientResponseWithZeroContent(Http1ClientResponseWithZeroContent::Framing(
                    Http1ClientCloseDelimitedResponse(transferCodings)))),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan knownLength(std::size_t contentLength,
        Http1ClosePolicy persistence, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientKnownLengthResponse(contentLength, persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan chunked(HttpTransferCodings transferCodings,
        Http1ClosePolicy persistence, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(Http1ClientResponsePlan::State(Http1ClientChunkedResponse(
                                           transferCodings, persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan closeDelimited(
        HttpTransferCodings transferCodings, RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientCloseDelimitedResponse(transferCodings)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan connectTunnel(
        RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientConnectTunnel()), requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan protocolUpgrade(
        RequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientProtocolUpgrade()), requestContentSignal);
    }
};

Http1ClientResponsePlanningResult planHttp1ClientResponse(
    const Http1ClientExchangeState& exchangeState, const Http1ClientParsedResponseHead& response,
    Http1ClientRequestContentPhase requestContentPhase) noexcept {
    const auto contentSemantics = httpResponseContentSemantics(
        Http1ClientExchangeStateAccess::method(exchangeState), response.statusCode);

    if (contentSemantics == HttpResponseContentSemantics::kProtocolSwitch) {
        if (response.protocolVersion != HttpProtocolVersion::kHttp11 ||
            response.contentLengthFieldPresent || response.sawTransferEncoding ||
            !requestAllowsProtocolSwitch(exchangeState, response, requestContentPhase)) {
            return Http1ClientResponseParseError::kInvalidProtocolSwitch;
        }
        return Http1ClientResponsePlanAccess::protocolUpgrade(std::nullopt);
    }
    if (contentSemantics == HttpResponseContentSemantics::kInformational) {
        const auto persistence = responsePersistence(response);
        return Http1ClientResponsePlanAccess::informational(
            persistence, requestContentSignal(requestContentPhase, response.statusCode,
                             persistence == Http1ClosePolicy::kCloseAfterResponse));
    }
    if (contentSemantics == HttpResponseContentSemantics::kConnectTunnel) {
        return Http1ClientResponsePlanAccess::connectTunnel(std::nullopt);
    }

    const bool resetContentRequiresEmpty = response.statusCode == http_status::kResetContent;
    const auto contentLength = response.contentLength.value();
    if (resetContentRequiresEmpty && contentLength.has_value() && *contentLength != 0) {
        return Http1ClientResponseParseError::kInvalidContentLength;
    }

    const auto persistence = finalResponsePersistence(exchangeState, response);
    const auto persistentContentSignal = requestContentSignal(requestContentPhase,
        response.statusCode, persistence == Http1ClosePolicy::kCloseAfterResponse);
    if (contentSemantics == HttpResponseContentSemantics::kWithoutContent) {
        return Http1ClientResponsePlanAccess::withoutContent(persistence, persistentContentSignal);
    }

    const auto transferEncoding = response.transferEncoding.value();
    if (response.sawTransferEncoding) {
        if (contentLength.has_value()) {
            return Http1ClientResponseParseError::kContentLengthAndTransferEncoding;
        }
        if (!transferEncoding.has_value()) {
            return Http1ClientResponseParseError::kInvalidTransferEncoding;
        }
        if (const auto* finalChunked = transferEncoding->finalChunked()) {
            if (resetContentRequiresEmpty) {
                return Http1ClientResponsePlanAccess::zeroContentChunked(
                    finalChunked->transferCodings(), persistence, persistentContentSignal);
            }
            return Http1ClientResponsePlanAccess::chunked(
                finalChunked->transferCodings(), persistence, persistentContentSignal);
        }
        if (resetContentRequiresEmpty) {
            return Http1ClientResponsePlanAccess::zeroContentCloseDelimited(
                transferEncoding->nonChunked()->transferCodings(),
                requestContentSignal(requestContentPhase, response.statusCode, true));
        }
        return Http1ClientResponsePlanAccess::closeDelimited(
            transferEncoding->nonChunked()->transferCodings(),
            requestContentSignal(requestContentPhase, response.statusCode, true));
    }

    if (contentLength.has_value()) {
        if (resetContentRequiresEmpty) {
            return Http1ClientResponsePlanAccess::zeroContentKnownLength(
                persistence, persistentContentSignal);
        }
        return Http1ClientResponsePlanAccess::knownLength(
            *contentLength, persistence, persistentContentSignal);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    if (resetContentRequiresEmpty) {
        return Http1ClientResponsePlanAccess::zeroContentCloseDelimited(
            {}, requestContentSignal(requestContentPhase, response.statusCode, true));
    }
    return Http1ClientResponsePlanAccess::closeDelimited(
        {}, requestContentSignal(requestContentPhase, response.statusCode, true));
}

}  // namespace ruvia::detail
