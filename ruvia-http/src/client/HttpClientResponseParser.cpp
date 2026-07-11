#include "ruvia/http/Http1ClientResponseParser.h"

#include <array>
#include <charconv>
#include <system_error>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HttpContentLength.h"
#include "ruvia/http/detail/HttpTransferEncoding.h"
#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

struct Http1ClientResponsePlanAccess final {
    [[nodiscard]] static Http1ClientResponsePlan informational(
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientInformationalResponse()),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan withoutContent(
        Http1ClientResponsePersistence persistence,
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientResponseWithoutContent(persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan knownLength(
        std::size_t contentLength,
        Http1ClientResponsePersistence persistence,
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientKnownLengthResponse(contentLength, persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan chunked(
        HttpTransferCodings transferCodings,
        Http1ClientResponsePersistence persistence,
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientChunkedResponse(transferCodings, persistence)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan closeDelimited(
        HttpTransferCodings transferCodings,
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(
                Http1ClientCloseDelimitedResponse(transferCodings)),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan connectTunnel(
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientConnectTunnel()),
            requestContentSignal);
    }

    [[nodiscard]] static Http1ClientResponsePlan protocolUpgrade(
        Http1ClientRequestContentSignal requestContentSignal) noexcept {
        return Http1ClientResponsePlan(
            Http1ClientResponsePlan::State(Http1ClientProtocolUpgrade()),
            requestContentSignal);
    }
};

struct Http1ClientResponseParseResultAccess final {
    [[nodiscard]] static Http1ClientResponseParseResult needMore() noexcept {
        return Http1ClientResponseParseResult(Http1ClientResponseNeedMore());
    }

    [[nodiscard]] static Http1ClientResponseParseResult failure(
        Http1ClientResponseParseError error) noexcept {
        return Http1ClientResponseParseResult(
            Http1ClientResponseParseFailure(error));
    }

    [[nodiscard]] static Http1ClientResponseParseResult parsed(
        HttpClientResponse response,
        Http1ClientResponsePlan plan,
        std::size_t consumedBytes) noexcept {
        return Http1ClientResponseParseResult(
            Http1ParsedClientResponseHead(
                std::move(response), std::move(plan), consumedBytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

struct ParsedStatusLine final {
    std::uint16_t statusCode{0};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
};

struct ParsedResponseHead final {
    std::array<HttpHeaderView, kMaxHttpHeaderFields> headers;
    std::size_t headerCount{0};
    std::uint16_t statusCode{0};
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    bool contentLengthFieldPresent{false};
    bool sawTransferEncoding{false};
    detail::HttpConnectionOptions connectionOptions;
    detail::HttpUpgradeProtocols upgradeProtocols;
    detail::HttpContentLengthState contentLength;
    detail::HttpTransferEncodingState transferEncoding;
};

using ResponsePlanningResult = std::variant<
    Http1ClientResponsePlan,
    Http1ClientResponseParseError>;

[[nodiscard]] bool parseStatusLine(
    std::string_view statusLine,
    ParsedStatusLine& output,
    Http1ClientResponseParseError& error) noexcept {
    const auto separator = statusLine.find(' ');
    if (separator == std::string_view::npos) {
        error = Http1ClientResponseParseError::kInvalidStatusLine;
        return false;
    }

    const auto version = statusLine.substr(0, separator);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        error = Http1ClientResponseParseError::kUnsupportedHttpVersion;
        return false;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (statusLine.size() < separator + 5 ||
        statusLine[separator + 4] != ' ') {
        error = Http1ClientResponseParseError::kInvalidStatusCode;
        return false;
    }

    int statusCode = 0;
    const auto code = statusLine.substr(separator + 1, 3);
    const auto [end, ec] = std::from_chars(
        code.data(), code.data() + code.size(), statusCode);
    if (ec != std::errc{} || end != code.data() + code.size() ||
        statusCode < 100 || statusCode > 999) {
        error = Http1ClientResponseParseError::kInvalidStatusCode;
        return false;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    for (const auto ch : statusLine.substr(separator + 5)) {
        if (!detail::isHttpFieldValueChar(static_cast<unsigned char>(ch))) {
            error = Http1ClientResponseParseError::kInvalidReasonPhrase;
            return false;
        }
    }

    output = ParsedStatusLine{
        .statusCode = static_cast<std::uint16_t>(statusCode),
        .protocolVersion = version == "HTTP/1.1"
            ? HttpProtocolVersion::kHttp11
            : HttpProtocolVersion::kHttp10};
    return true;
}

[[nodiscard]] bool requestOffersProtocol(
    const detail::Http1ClientRequestContext& request,
    const detail::HttpUpgradeProtocol& selected) noexcept {
    bool offered = false;
    detail::HttpUpgradeProtocols protocols;
    for (const auto& header : request.headers()) {
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            continue;
        }
        if (protocols.parseField(
                header.value(),
                detail::HttpFieldListRole::kSender,
                [&selected, &offered](
                    const detail::HttpUpgradeProtocol& candidate) noexcept {
                    offered = offered ||
                        detail::httpUpgradeProtocolEquals(candidate, selected);
                    return true;
                }) != detail::HttpFieldListParseStatus::kOk) {
            return false;
        }
    }
    return protocols.hasProtocol() && offered;
}

[[nodiscard]] bool requestAllowsProtocolSwitch(
    const detail::Http1ClientRequestContext& request,
    const ParsedResponseHead& response,
    bool sawContinue,
    bool requestContentComplete) noexcept {
    if (request.closePolicy() ==
            Http1ClientRequestClosePolicy::kCloseAfterResponse ||
        (request.expectsContinue() && !sawContinue) ||
        !requestContentComplete) {
        return false;
    }
    if (!request.connectionOptions().upgrade() ||
        response.connectionOptions.close() ||
        !response.connectionOptions.upgrade() ||
        !response.upgradeProtocols.hasProtocol()) {
        return false;
    }

    detail::HttpUpgradeProtocols selectedProtocols;
    for (std::size_t i = 0; i < response.headerCount; ++i) {
        const auto& header = response.headers[i];
        if (!detail::httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            continue;
        }
        if (selectedProtocols.parseField(
                header.value(),
                detail::HttpFieldListRole::kRecipient,
                [&request](
                    const detail::HttpUpgradeProtocol& selected) noexcept {
                    return requestOffersProtocol(request, selected);
                }) != detail::HttpFieldListParseStatus::kOk) {
            return false;
        }
    }
    return selectedProtocols.hasProtocol();
}

[[nodiscard]] Http1ClientResponsePersistence responsePersistence(
    const detail::Http1ClientRequestContext& request,
    const ParsedResponseHead& response) noexcept {
    if (request.closePolicy() ==
            Http1ClientRequestClosePolicy::kCloseAfterResponse ||
        response.connectionOptions.close()) {
        return Http1ClientResponsePersistence::kClose;
    }
    if (response.protocolVersion == HttpProtocolVersion::kHttp11 ||
        response.connectionOptions.keepAlive()) {
        return Http1ClientResponsePersistence::kReuse;
    }
    return Http1ClientResponsePersistence::kClose;
}

[[nodiscard]] bool parseResponseHeadFields(
    std::string_view headSection,
    const detail::Http1ClientRequestContext& request,
    ParsedResponseHead& output,
    Http1ClientResponseParseError& error) noexcept {
    const auto firstLineEnd = headSection.find("\r\n");
    const auto firstLine = firstLineEnd == std::string_view::npos
        ? headSection
        : headSection.substr(0, firstLineEnd);
    ParsedStatusLine statusLine;
    if (!parseStatusLine(firstLine, statusLine, error)) {
        return false;
    }
    output.statusCode = statusLine.statusCode;
    output.protocolVersion = statusLine.protocolVersion;

    const bool protocolSwitch = output.statusCode == 101;
    const bool informational = output.statusCode < 200 && !protocolSwitch;
    const bool connectTunnel = request.method() == "CONNECT" &&
        output.statusCode >= 200 && output.statusCode < 300;
    const bool framingFieldsApply = !informational && !protocolSwitch &&
        request.method() != "HEAD" &&
        output.statusCode != 204 &&
        output.statusCode != 304 &&
        !connectTunnel;

    auto remaining = firstLineEnd == std::string_view::npos
        ? std::string_view{}
        : headSection.substr(firstLineEnd + 2);
    while (!remaining.empty()) {
        const auto lineEnd = remaining.find("\r\n");
        const auto line = lineEnd == std::string_view::npos
            ? remaining
            : remaining.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            error = Http1ClientResponseParseError::kInvalidHeader;
            return false;
        }

        const auto name = line.substr(0, colon);
        const auto value = detail::httpTrimOws(line.substr(colon + 1));
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
            error = Http1ClientResponseParseError::kInvalidHeader;
            return false;
        }
        if (output.headerCount == kMaxHttpHeaderFields) {
            error = Http1ClientResponseParseError::kTooManyHeaders;
            return false;
        }
        output.headers[output.headerCount++] = HttpHeaderView{name, value};

        if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
            output.contentLengthFieldPresent = true;
            // RFC 9112 section 6.3 applies method/status precedence before
            // Content-Length parsing. HEAD, non-101 informational, 204, 304,
            // and successful CONNECT therefore ignore this field for framing.
            // A 101 still records its forbidden presence for handshake checks.
            if (framingFieldsApply) {
                switch (output.contentLength.parseField(value)) {
                    case detail::HttpContentLengthParseStatus::kOk:
                        break;
                    case detail::HttpContentLengthParseStatus::kInvalid:
                        error = Http1ClientResponseParseError::kInvalidContentLength;
                        return false;
                    case detail::HttpContentLengthParseStatus::kConflicting:
                        error = Http1ClientResponseParseError::kConflictingContentLength;
                        return false;
                }
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
            if (output.connectionOptions.parseField(
                    value,
                    detail::HttpFieldListRole::kRecipient) !=
                detail::HttpFieldListParseStatus::kOk) {
                error = Http1ClientResponseParseError::kInvalidConnection;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
            output.sawTransferEncoding = true;
            // RFC 9112 method/status precedence decides whether this field can
            // frame this message. HEAD/1xx/204/304 and successful CONNECT ignore
            // it here; HEAD/304 may legitimately carry representation metadata.
            if (framingFieldsApply &&
                output.protocolVersion == HttpProtocolVersion::kHttp11) {
                switch (output.transferEncoding.parseField(value)) {
                    case detail::HttpTransferEncodingParseStatus::kOk:
                        break;
                    case detail::HttpTransferEncodingParseStatus::kMalformed:
                        error = Http1ClientResponseParseError::kInvalidTransferEncoding;
                        return false;
                    case detail::HttpTransferEncodingParseStatus::kUnsupported:
                        error = Http1ClientResponseParseError::kUnsupportedTransferEncoding;
                        return false;
                }
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
            if (output.upgradeProtocols.parseField(
                    value,
                    detail::HttpFieldListRole::kRecipient,
                    [](const detail::HttpUpgradeProtocol&) noexcept {
                        return true;
                    }) != detail::HttpFieldListParseStatus::kOk) {
                error = Http1ClientResponseParseError::kInvalidUpgrade;
                return false;
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(lineEnd + 2);
    }

    if (output.protocolVersion == HttpProtocolVersion::kHttp10 &&
        output.sawTransferEncoding) {
        error = Http1ClientResponseParseError::kTransferEncodingInHttp10;
        return false;
    }
    return true;
}

[[nodiscard]] ResponsePlanningResult planResponse(
    const detail::Http1ClientRequestContext& request,
    const ParsedResponseHead& response,
    bool sawContinue,
    bool requestContentComplete) noexcept {
    const bool protocolSwitch = response.statusCode == 101;
    const bool informational = response.statusCode < 200 && !protocolSwitch;
    const bool connectTunnel = request.method() == "CONNECT" &&
        response.statusCode >= 200 && response.statusCode < 300;
    const bool framingFieldsApply = !informational && !protocolSwitch &&
        request.method() != "HEAD" &&
        response.statusCode != 204 &&
        response.statusCode != 304 &&
        !connectTunnel;

    auto requestContentSignal = Http1ClientRequestContentSignal::kNone;
    if (request.expectsContinue()) {
        if (response.statusCode == 100) {
            requestContentSignal = Http1ClientRequestContentSignal::kContinue;
        } else if (protocolSwitch || response.statusCode >= 200) {
            requestContentSignal =
                Http1ClientRequestContentSignal::kExchangeComplete;
        }
    }

    if (protocolSwitch) {
        if (response.protocolVersion != HttpProtocolVersion::kHttp11 ||
            response.contentLengthFieldPresent ||
            response.sawTransferEncoding ||
            !requestAllowsProtocolSwitch(
                request,
                response,
                sawContinue,
                requestContentComplete)) {
            return Http1ClientResponseParseError::kInvalidProtocolSwitch;
        }
        return detail::Http1ClientResponsePlanAccess::protocolUpgrade(
            requestContentSignal);
    }
    if (informational) {
        return detail::Http1ClientResponsePlanAccess::informational(
            requestContentSignal);
    }
    if (connectTunnel) {
        return detail::Http1ClientResponsePlanAccess::connectTunnel(
            requestContentSignal);
    }

    const auto persistence = responsePersistence(request, response);
    if (!framingFieldsApply) {
        return detail::Http1ClientResponsePlanAccess::withoutContent(
            persistence,
            requestContentSignal);
    }

    if (response.sawTransferEncoding) {
        if (response.contentLength.present()) {
            return Http1ClientResponseParseError::
                kContentLengthAndTransferEncoding;
        }
        if (response.transferEncoding.finalChunked()) {
            return detail::Http1ClientResponsePlanAccess::chunked(
                response.transferEncoding.codings(),
                persistence,
                requestContentSignal);
        }
        return detail::Http1ClientResponsePlanAccess::closeDelimited(
            response.transferEncoding.codings(),
            requestContentSignal);
    }

    if (response.contentLength.present()) {
        return detail::Http1ClientResponsePlanAccess::knownLength(
            response.contentLength.value(),
            persistence,
            requestContentSignal);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    return detail::Http1ClientResponsePlanAccess::closeDelimited(
        {},
        requestContentSignal);
}

}  // namespace

std::string_view http1ClientResponseParseErrorMessage(
    Http1ClientResponseParseError error) noexcept {
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
        case Http1ClientResponseParseError::kExchangeComplete:
            return "HTTP/1 client exchange is already complete";
        case Http1ClientResponseParseError::kExchangeFailed:
            return "HTTP/1 client exchange has already failed";
    }
    return "invalid HTTP/1 response";
}

Http1ClientResponseParseResult Http1ClientResponseParser::parse(
    std::string_view buffer) {
    if (phase_ == Phase::kComplete) {
        return detail::Http1ClientResponseParseResultAccess::failure(
            Http1ClientResponseParseError::kExchangeComplete);
    }
    if (phase_ == Phase::kFailed) {
        return detail::Http1ClientResponseParseResultAccess::failure(
            Http1ClientResponseParseError::kExchangeFailed);
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
    ParsedResponseHead parsed;
    Http1ClientResponseParseError error =
        Http1ClientResponseParseError::kInvalidStatusLine;
    if (!parseResponseHeadFields(headSection, request_, parsed, error)) {
        return fail(error);
    }

    auto planning = planResponse(
        request_,
        parsed,
        sawContinue_,
        requestContentComplete_);
    if (const auto* planningError =
            std::get_if<Http1ClientResponseParseError>(&planning)) {
        return fail(*planningError);
    }
    auto plan = std::get<Http1ClientResponsePlan>(std::move(planning));

    // No owning response is observable until the entire head and its framing
    // plan have validated. Protocol failure therefore has no partially mutated
    // out-parameter and performs no PMR allocation.
    auto response = detail::HttpClientResponseAccess::make(
        parsed.protocolVersion, resource_);
    detail::HttpClientResponseAccess::setStatus(response, parsed.statusCode);
    auto& headers = detail::HttpClientResponseAccess::headers(response);
    if (parsed.headerCount != 0) {
        headers.reserve(parsed.headerCount);
    }
    for (std::size_t i = 0; i < parsed.headerCount; ++i) {
        const auto& header = parsed.headers[i];
        headers.emplace_back(detail::HttpClientResponseHeaderAccess::make(
            header.name(), header.value(), resource_));
    }

    auto result = detail::Http1ClientResponseParseResultAccess::parsed(
        std::move(response), std::move(plan), headerBytes);
    if (parsed.statusCode == 100) {
        sawContinue_ = true;
    }
    if (parsed.statusCode == 101 || parsed.statusCode >= 200) {
        phase_ = Phase::kComplete;
    }
    return result;
}

Http1ClientRequestContentCompletionStatus
Http1ClientResponseParser::completeRequestContent() noexcept {
    if (phase_ != Phase::kAwaitResponse) {
        return Http1ClientRequestContentCompletionStatus::kExchangeTerminal;
    }
    if (requestContentComplete_) {
        return Http1ClientRequestContentCompletionStatus::kAlreadyComplete;
    }
    requestContentComplete_ = true;
    return Http1ClientRequestContentCompletionStatus::kCompleted;
}

}  // namespace ruvia
