#include "ruvia/http/Http1ClientRequestWriter.h"

#include <array>
#include <charconv>
#include <cstring>
#include <system_error>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/client/HttpOrigin.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia::detail {

struct Http1ClientRequestPrepareResultAccess final {
    [[nodiscard]] static constexpr Http1ClientRequestContext context(
        std::string_view method,
        std::span<const HttpHeaderView> headers,
        HttpConnectionOptions connectionOptions,
        Http1ClientRequestClosePolicy closePolicy) noexcept {
        return Http1ClientRequestContext(
            method,
            headers,
            connectionOptions,
            closePolicy);
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult bufferTooSmall(
        std::size_t requiredHeadBytes) noexcept {
        return Http1ClientRequestPrepareResult(
            Http1ClientRequestBufferTooSmall(requiredHeadBytes));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult failure(
        Http1ClientRequestPrepareError error) noexcept {
        return Http1ClientRequestPrepareResult(
            Http1ClientRequestPrepareFailure(error));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult
    preparedWithoutContent(
        std::string_view head,
        Http1ClientRequestContext responseContext) noexcept {
        return Http1ClientRequestPrepareResult(
            PreparedHttp1ClientRequest(
                head,
                Http1ClientRequestContentPlan(
                    Http1ClientRequestWithoutContent()),
                responseContext));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult
    preparedImmediateContent(
        std::string_view head,
        std::string_view contentBytes,
        Http1ClientRequestContext responseContext) noexcept {
        return Http1ClientRequestPrepareResult(
            PreparedHttp1ClientRequest(
                head,
                Http1ClientRequestContentPlan(
                    Http1ClientImmediateRequestContent(contentBytes)),
                responseContext));
    }

    [[nodiscard]] static constexpr Http1ClientRequestPrepareResult
    preparedContinueGatedContent(
        std::string_view head,
        std::string_view contentBytes,
        Http1ClientRequestContext responseContext) noexcept {
        return Http1ClientRequestPrepareResult(
            PreparedHttp1ClientRequest(
                head,
                Http1ClientRequestContentPlan(
                    Http1ClientContinueGatedRequestContent(contentBytes)),
                responseContext));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view kHttp11RequestLineSuffix = " HTTP/1.1\r\n";
constexpr std::string_view kHostPrefix = "Host: ";
constexpr std::string_view kContentLengthPrefix = "Content-Length: ";
constexpr std::string_view kExpectContinue = "Expect: 100-continue\r\n";
constexpr std::string_view kConnectionClose = "Connection: close\r\n";
constexpr std::string_view kCrlf = "\r\n";

struct RequestHeaderFacts final {
    std::size_t wireBytes{0};
    detail::HttpConnectionOptions connectionOptions;
    detail::HttpUpgradeProtocols upgradeProtocols;
    bool hasContentType{false};
    bool hasTe{false};
};

[[nodiscard]] constexpr std::size_t decimalDigits(std::size_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

[[nodiscard]] bool addHeadBytes(
    std::size_t& total,
    std::size_t bytes) noexcept {
    if (bytes > kMaxHttpHeaderBytes - total) {
        return false;
    }
    total += bytes;
    return true;
}

[[nodiscard]] bool isValidClientTeItem(std::string_view item) noexcept {
    std::size_t cursor = 0;
    const auto skipOws = [&item, &cursor]() noexcept {
        while (cursor < item.size() &&
               (item[cursor] == ' ' || item[cursor] == '\t')) {
            ++cursor;
        }
    };
    const auto parseToken = [&item, &cursor]() noexcept {
        const auto begin = cursor;
        while (cursor < item.size() &&
               detail::isHttpTokenChar(
                   static_cast<unsigned char>(item[cursor]))) {
            ++cursor;
        }
        return item.substr(begin, cursor - begin);
    };

    const auto coding = parseToken();
    if (coding.empty()) {
        return false;
    }
    const bool trailers =
        detail::httpAsciiEqualsIgnoreCase(coding, "trailers");
    const bool supportedCoding =
        detail::httpAsciiEqualsIgnoreCase(coding, "gzip") ||
        detail::httpAsciiEqualsIgnoreCase(coding, "x-gzip") ||
        detail::httpAsciiEqualsIgnoreCase(coding, "deflate");
    if (!trailers && !supportedCoding) {
        // The paired response parser cannot represent any other transfer
        // coding. Advertising one here would make the client claim a decoding
        // capability it does not have. "chunked" is never listed in TE because
        // every HTTP/1.1 recipient already accepts it as message framing.
        return false;
    }

    skipOws();
    if (cursor == item.size()) {
        return true;
    }
    if (trailers || item[cursor] != ';') {
        return false;
    }
    ++cursor;
    skipOws();
    const auto parameter = parseToken();
    if (!detail::httpAsciiEqualsIgnoreCase(parameter, "q")) {
        return false;
    }
    skipOws();
    if (cursor == item.size() || item[cursor] != '=') {
        return false;
    }
    ++cursor;
    skipOws();
    const auto quality = parseToken();
    if (quality.empty() || detail::httpParseQualityValue(quality) < 0) {
        return false;
    }
    skipOws();
    return cursor == item.size();
}

[[nodiscard]] bool isValidClientTeField(std::string_view value) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (detail::httpTrimOws(value).empty()) {
        return true;
    }
    bool valid = true;
    bool sawItem = false;
    detail::httpVisitCommaSeparatedQuotedItems(
        value,
        [&valid, &sawItem](std::string_view item) noexcept {
            sawItem = true;
            if (!isValidClientTeItem(item)) {
                valid = false;
                return false;
            }
            return true;
        });
    return valid && sawItem;
}

[[nodiscard]] std::size_t authorityLength(
    const HttpOrigin& origin,
    bool forcePort) noexcept {
    return origin.host().size() +
        ((forcePort || !detail::httpOriginUsesDefaultPort(origin))
             ? 1 + decimalDigits(origin.port())
             : 0);
}

[[nodiscard]] bool analyzeHeaders(
    std::span<const HttpHeaderView> headers,
    RequestHeaderFacts& facts,
    Http1ClientRequestPrepareError& error) noexcept {
    for (const auto& header : headers) {
        const auto name = header.name();
        const auto value = header.value();
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
            error = Http1ClientRequestPrepareError::kInvalidHeader;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Host")) {
            error = Http1ClientRequestPrepareError::kHostHeaderManagedByWriter;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
            error = Http1ClientRequestPrepareError::kContentLengthManagedByWriter;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
            error = Http1ClientRequestPrepareError::kTransferEncodingUnsupported;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Trailer")) {
            error = Http1ClientRequestPrepareError::kTrailerSectionUnsupported;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Expect")) {
            error = Http1ClientRequestPrepareError::kExpectHeaderManagedByWriter;
            return false;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
            if (facts.connectionOptions.parseField(
                    value,
                    detail::HttpFieldListRole::kSender) !=
                detail::HttpFieldListParseStatus::kOk) {
                error = Http1ClientRequestPrepareError::kInvalidConnection;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
            if (facts.upgradeProtocols.parseField(
                    value,
                    detail::HttpFieldListRole::kSender,
                    [](const detail::HttpUpgradeProtocol&) noexcept {
                        return true;
                    }) != detail::HttpFieldListParseStatus::kOk) {
                error = Http1ClientRequestPrepareError::kInvalidUpgrade;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "TE")) {
            if (!isValidClientTeField(value)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
            facts.hasTe = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
            detail::HttpMediaTypeParts parts;
            if (facts.hasContentType ||
                !detail::httpParseMediaTypeParts(value, false, parts)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
            facts.hasContentType = true;
        }

        if (!addHeadBytes(facts.wireBytes, name.size()) ||
            !addHeadBytes(facts.wireBytes, 2) ||
            !addHeadBytes(facts.wireBytes, value.size()) ||
            !addHeadBytes(facts.wireBytes, kCrlf.size())) {
            error = Http1ClientRequestPrepareError::kHeaderTooLarge;
            return false;
        }
    }
    if (facts.upgradeProtocols.hasField() &&
        !facts.connectionOptions.upgrade()) {
        error =
            Http1ClientRequestPrepareError::kUpgradeConnectionOptionRequired;
        return false;
    }
    if (facts.hasTe && !facts.connectionOptions.te()) {
        error = Http1ClientRequestPrepareError::kTeConnectionOptionRequired;
        return false;
    }
    return true;
}

void appendView(char*& cursor, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(cursor, value.data(), value.size());
        cursor += value.size();
    }
}

void appendUnsigned(char*& cursor, std::size_t value) noexcept {
    std::array<char, 32> digits;
    const auto [end, ec] = std::to_chars(
        digits.data(), digits.data() + digits.size(), value);
    if (ec == std::errc{}) {
        appendView(
            cursor,
            std::string_view(
                digits.data(), static_cast<std::size_t>(end - digits.data())));
    }
}

void appendAuthority(
    char*& cursor,
    const HttpOrigin& origin,
    bool forcePort) noexcept {
    appendView(cursor, origin.host());
    if (forcePort || !detail::httpOriginUsesDefaultPort(origin)) {
        *cursor++ = ':';
        appendUnsigned(cursor, origin.port());
    }
}

void appendHeaders(
    char*& cursor,
    std::span<const HttpHeaderView> headers) noexcept {
    for (const auto& header : headers) {
        appendView(cursor, header.name());
        appendView(cursor, ": ");
        appendView(cursor, header.value());
        appendView(cursor, kCrlf);
    }
}

[[nodiscard]] Http1ClientRequestPrepareResult prepareRequest(
    const HttpOrigin& origin,
    std::string_view method,
    std::string_view target,
    bool connect,
    std::span<const HttpHeaderView> headers,
    HttpClientRequestContent content,
    std::span<char> headBuffer,
    Http1ClientRequestWirePolicy policy) noexcept {
    RequestHeaderFacts headerFacts;
    Http1ClientRequestPrepareError error =
        Http1ClientRequestPrepareError::kInvalidHeader;
    const auto* contentBytes = content.borrowedBytes();
    const bool explicitContent = contentBytes != nullptr;
    if (!analyzeHeaders(headers, headerFacts, error)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(error);
    }
    const bool expectContinue =
        policy.continueExpectation() != nullptr;
    if (expectContinue &&
        (!explicitContent || contentBytes->value().empty())) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kExpectationWithoutContent);
    }
    if (method == "TRACE" && explicitContent) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kContentForbiddenForMethod);
    }
    if (method == "OPTIONS" && explicitContent &&
        !contentBytes->value().empty() &&
        !headerFacts.hasContentType) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kOptionsContentTypeRequired);
    }

    const bool generateConnectionClose =
        policy.closePolicy() == Http1ClientRequestClosePolicy::kCloseAfterResponse &&
        !headerFacts.connectionOptions.close();
    const auto effectiveClosePolicy =
        headerFacts.connectionOptions.close() || generateConnectionClose
        ? Http1ClientRequestClosePolicy::kCloseAfterResponse
        : Http1ClientRequestClosePolicy::kAllowReuse;
    const std::size_t generatedFields =
        1 + (explicitContent ? 1 : 0) + (expectContinue ? 1 : 0) +
        (generateConnectionClose ? 1 : 0);
    if (headers.size() > kMaxHttpHeaderFields - generatedFields) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kTooManyHeaders);
    }

    const std::size_t targetBytes = connect
        ? authorityLength(origin, true)
        : target.size();
    std::size_t headBytes = 0;
    if (!addHeadBytes(headBytes, method.size()) ||
        !addHeadBytes(headBytes, 1) ||
        !addHeadBytes(headBytes, targetBytes) ||
        !addHeadBytes(headBytes, kHttp11RequestLineSuffix.size()) ||
        !addHeadBytes(headBytes, kHostPrefix.size()) ||
        !addHeadBytes(headBytes, authorityLength(origin, false)) ||
        !addHeadBytes(headBytes, kCrlf.size()) ||
        !addHeadBytes(headBytes, headerFacts.wireBytes) ||
        (explicitContent &&
         (!addHeadBytes(headBytes, kContentLengthPrefix.size()) ||
          !addHeadBytes(headBytes, decimalDigits(contentBytes->value().size())) ||
          !addHeadBytes(headBytes, kCrlf.size()))) ||
        (expectContinue && !addHeadBytes(headBytes, kExpectContinue.size())) ||
        (generateConnectionClose &&
         !addHeadBytes(headBytes, kConnectionClose.size())) ||
        !addHeadBytes(headBytes, kCrlf.size())) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kHeaderTooLarge);
    }
    if (headBuffer.size() < headBytes) {
        return detail::Http1ClientRequestPrepareResultAccess::bufferTooSmall(
            headBytes);
    }

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
        appendView(cursor, kExpectContinue);
    }
    if (generateConnectionClose) {
        appendView(cursor, kConnectionClose);
    }
    appendView(cursor, kCrlf);

    const auto responseContext =
        detail::Http1ClientRequestPrepareResultAccess::context(
            method,
            headers,
            headerFacts.connectionOptions,
            effectiveClosePolicy);
    const auto head = std::string_view(headBuffer.data(), headBytes);
    if (!explicitContent) {
        return detail::Http1ClientRequestPrepareResultAccess::preparedWithoutContent(
            head, responseContext);
    }
    if (expectContinue) {
        return detail::Http1ClientRequestPrepareResultAccess::preparedContinueGatedContent(
            head, contentBytes->value(), responseContext);
    }
    return detail::Http1ClientRequestPrepareResultAccess::preparedImmediateContent(
        head, contentBytes->value(), responseContext);
}

}  // namespace

std::string_view http1ClientRequestPrepareErrorMessage(
    Http1ClientRequestPrepareError error) noexcept {
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
            return "100-continue requires non-empty request content";
        case Http1ClientRequestPrepareError::kContentForbiddenForMethod:
            return "request method forbids content";
        case Http1ClientRequestPrepareError::kOptionsContentTypeRequired:
            return "OPTIONS content requires Content-Type";
        case Http1ClientRequestPrepareError::kHeaderTooLarge:
            return "HTTP/1 client request header is too large";
    }
    return "invalid HTTP/1 client request";
}

Http1ClientRequestPrepareResult Http1ClientRequestWriter::prepare(
    const HttpOrigin& origin,
    const HttpClientRequest& request,
    std::span<char> headBuffer,
    Http1ClientRequestWirePolicy policy) const noexcept {
    if (!isValidHttpMethodToken(request.method)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kInvalidMethod);
    }
    if (request.method == "CONNECT") {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kConnectRequiresDedicatedEntry);
    }
    if ((request.target == "*" && request.method != "OPTIONS") ||
        !detail::isValidOriginFormTarget(request.target)) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kInvalidTarget);
    }
    return prepareRequest(
        origin,
        request.method,
        request.target,
        false,
        static_cast<std::span<const HttpHeaderView>>(request.headers),
        request.content,
        headBuffer,
        policy);
}

Http1ClientRequestPrepareResult Http1ClientRequestWriter::prepareConnect(
    const HttpOrigin& tunnelOrigin,
    std::span<const HttpHeaderView> headers,
    std::span<char> headBuffer,
    Http1ClientRequestWirePolicy policy) const noexcept {
    if (tunnelOrigin.port() == 0) {
        return detail::Http1ClientRequestPrepareResultAccess::failure(
            Http1ClientRequestPrepareError::kInvalidConnectOrigin);
    }
    return prepareRequest(
        tunnelOrigin,
        "CONNECT",
        {},
        true,
        headers,
        HttpClientRequestContent::none(),
        headBuffer,
        policy);
}

}  // namespace ruvia
