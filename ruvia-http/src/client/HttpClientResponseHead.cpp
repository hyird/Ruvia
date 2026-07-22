#include "ruvia/http/detail/client/HttpClientResponseHead.h"

#include <charconv>
#include <system_error>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpContentCoding.h"
#include "ruvia/http/detail/HttpInterimResponseValidation.h"
#include "ruvia/http/detail/HttpMediaType.h"
#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/client/HttpClientResponseLimits.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] Http1ClientStatusLineParseResult parseStatusLine(
    std::string_view statusLine) noexcept {
    const auto separator = statusLine.find(' ');
    if (separator == std::string_view::npos) {
        return Http1ClientResponseParseError::kInvalidStatusLine;
    }

    const auto version = statusLine.substr(0, separator);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        return Http1ClientResponseParseError::kUnsupportedHttpVersion;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (statusLine.size() < separator + 5 ||
        statusLine[separator + 4] != ' ') {
        return Http1ClientResponseParseError::kInvalidStatusCode;
    }

    int statusCode = 0;
    const auto code = statusLine.substr(separator + 1, 3);
    const auto [end, ec] = std::from_chars(
        code.data(), code.data() + code.size(), statusCode);
    if (ec != std::errc{} || end != code.data() + code.size()) {
        return Http1ClientResponseParseError::kInvalidStatusCode;
    }
    const auto parsedStatus = HttpStatusCode::tryFromValue(
        static_cast<std::uint16_t>(statusCode));
    if (!parsedStatus) {
        return Http1ClientResponseParseError::kInvalidStatusCode;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    for (const auto ch : statusLine.substr(separator + 5)) {
        if (!isHttpFieldValueChar(static_cast<unsigned char>(ch))) {
            return Http1ClientResponseParseError::kInvalidReasonPhrase;
        }
    }

    return Http1ClientParsedStatusLine{
        .statusCode = *parsedStatus,
        .protocolVersion = version == "HTTP/1.1"
            ? HttpProtocolVersion::kHttp11
            : HttpProtocolVersion::kHttp10};
}

}  // namespace

Http1ClientResponseHeadParseResult parseHttp1ClientResponseHeadFields(
    std::string_view headSection,
    const Http1ClientRequestContext& request) noexcept {
    const auto firstLineEnd = headSection.find("\r\n");
    const auto firstLine = firstLineEnd == std::string_view::npos
        ? headSection
        : headSection.substr(0, firstLineEnd);
    auto statusLineResult = parseStatusLine(firstLine);
    const auto* statusLine = std::get_if<Http1ClientParsedStatusLine>(&statusLineResult);
    if (statusLine == nullptr) {
        return std::get<Http1ClientResponseParseError>(statusLineResult);
    }
    Http1ClientResponseHeadParseResult result(
        std::in_place_type<Http1ClientParsedResponseHead>, *statusLine);
    auto& output = std::get<Http1ClientParsedResponseHead>(result);

    const auto contentSemantics = httpResponseContentSemantics(
        request.method(), output.statusCode);
    HttpInterimResponseHeaderValidator interimHeaders(
        HttpFieldListRole::kRecipient);
    const bool framingFieldsApply =
        contentSemantics ==
        HttpResponseContentSemantics::kWithContent;
    const bool resetContentRequiresEmpty =
        output.statusCode == http_status::kResetContent &&
        contentSemantics !=
            HttpResponseContentSemantics::kConnectTunnel;

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
            return Http1ClientResponseParseError::kInvalidHeader;
        }

        const auto name = line.substr(0, colon);
        const auto value = httpTrimOws(line.substr(colon + 1));
        const bool fieldsValid = contentSemantics ==
                HttpResponseContentSemantics::kInformational
            ? interimHeaders.validate(name, value) ==
                HttpInterimResponseHeaderValidationStatus::kOk
            : isValidHttpHeaderName(name) &&
                isValidHttpHeaderValue(value);
        if (!fieldsValid) {
            return Http1ClientResponseParseError::kInvalidHeader;
        }
        if (output.headerCount == kMaxHttpHeaderFields) {
            return Http1ClientResponseParseError::kTooManyHeaders;
        }
        output.headers[output.headerCount++] = HttpHeaderView{name, value};

        if (httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
            output.contentLengthFieldPresent = true;
            // RFC 9112 section 6.3 applies method/status precedence before
            // Content-Length parsing. HEAD, non-101 informational, 204, 304,
            // and successful CONNECT therefore ignore this field for framing.
            // A 101 still records its forbidden presence for handshake checks.
            if (framingFieldsApply || resetContentRequiresEmpty) {
                switch (output.contentLength.parseField(value)) {
                    case HttpContentLengthParseStatus::kOk:
                        break;
                    case HttpContentLengthParseStatus::kInvalid:
                        return Http1ClientResponseParseError::kInvalidContentLength;
                    case HttpContentLengthParseStatus::kConflicting:
                        return Http1ClientResponseParseError::kConflictingContentLength;
                }
            }
        } else if (httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
            if (output.contentTypeFieldPresent ||
                !isValidHttpContentTypeFieldValue(value)) {
                return Http1ClientResponseParseError::kInvalidHeader;
            }
            output.contentTypeFieldPresent = true;
        } else if (httpAsciiEqualsIgnoreCase(
                       name, "Content-Encoding")) {
            if (!isValidHttpContentEncodingFieldValue(
                    value, HttpFieldListRole::kRecipient)) {
                return Http1ClientResponseParseError::kInvalidHeader;
            }
        } else if (httpAsciiEqualsIgnoreCase(name, "Connection")) {
            if (output.connectionOptions.parseField(
                    value,
                    HttpFieldListRole::kRecipient,
                    [](std::string_view option) noexcept {
                        return !httpConnectionOptionConflictsWithManagedField(
                            option);
                    }) !=
                HttpFieldListParseStatus::kOk) {
                return Http1ClientResponseParseError::kInvalidConnection;
            }
        } else if (httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
            output.sawTransferEncoding = true;
            // RFC 9112 method/status precedence decides whether this field can
            // frame this message. HEAD/1xx/204/304 and successful CONNECT ignore
            // it here; HEAD/304 may legitimately carry representation metadata.
            if (framingFieldsApply &&
                output.protocolVersion == HttpProtocolVersion::kHttp11) {
                switch (output.transferEncoding.parseField(value)) {
                    case HttpTransferEncodingParseStatus::kOk:
                        break;
                    case HttpTransferEncodingParseStatus::kMalformed:
                        return Http1ClientResponseParseError::kInvalidTransferEncoding;
                    case HttpTransferEncodingParseStatus::kUnsupported:
                        return Http1ClientResponseParseError::kUnsupportedTransferEncoding;
                }
            }
        } else if (httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
            if (output.upgradeProtocols.parseField(
                    value,
                    HttpFieldListRole::kRecipient,
                    [](const HttpUpgradeProtocol&) noexcept {
                        return true;
                    }) != HttpFieldListParseStatus::kOk) {
                return Http1ClientResponseParseError::kInvalidUpgrade;
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(lineEnd + 2);
    }

    if (output.protocolVersion == HttpProtocolVersion::kHttp10 &&
        output.sawTransferEncoding) {
        return Http1ClientResponseParseError::kTransferEncodingInHttp10;
    }
    return result;
}

}  // namespace ruvia::detail
