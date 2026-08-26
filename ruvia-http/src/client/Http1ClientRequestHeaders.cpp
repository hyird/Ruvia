#include "ruvia/http/detail/client/Http1ClientRequestHeaders.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpTeFields.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
namespace ruvia {

bool addHeadBytes(std::size_t& total, std::size_t bytes) noexcept {
    if (bytes > kMaxHttpHeaderBytes - total) {
        return false;
    }
    total += bytes;
    return true;
}

bool isValidClientTeField(std::string_view value) noexcept {
    return detail::isValidClientHttpTeFieldValue(value);
}

bool analyzeHeaders(std::span<const HttpHeaderView> headers, RequestHeaderFacts& facts,
    Http1ClientRequestPrepareError& error) noexcept {
    for (const auto& header : headers) {
        const auto name = header.name();
        const auto value = header.value();
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
            error = Http1ClientRequestPrepareError::kInvalidHeader;
            return false;
        }
        const auto kind = detail::classifyRequestHeader(name);
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
        if ((kind == detail::RequestHeaderKind::kOrigin &&
                !detail::isValidHttpOriginFieldValue(value)) ||
            (kind == detail::RequestHeaderKind::kAccessControlRequestMethod &&
                !detail::isValidHttpCorsRequestMethod(value)) ||
            (kind == detail::RequestHeaderKind::kAccessControlRequestHeaders &&
                !detail::isValidHttpCorsRequestHeaderNames(value))) {
            error = Http1ClientRequestPrepareError::kInvalidHeader;
            return false;
        }
        if (const auto bit = detail::singletonRequestHeaderBit(kind); bit != 0) {
            if ((facts.singletonHeaders & bit) != 0) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
            facts.singletonHeaders |= bit;
        }
        if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
            if (facts.connectionOptions.parseField(value, detail::HttpFieldListRole::kSender,
                    [](std::string_view option) noexcept {
                        return !detail::httpConnectionOptionConflictsWithManagedField(option);
                    }) != detail::HttpFieldListParseStatus::kOk) {
                error = Http1ClientRequestPrepareError::kInvalidConnection;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
            if (facts.upgradeProtocols.parseField(value, detail::HttpFieldListRole::kSender,
                    [](const detail::HttpUpgradeProtocol&) noexcept { return true; }) !=
                detail::HttpFieldListParseStatus::kOk) {
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
            if (!detail::isValidHttpContentTypeFieldValue(value)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
            facts.hasContentType = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) {
            if (!detail::isValidHttpContentEncodingFieldValue(
                    value, detail::HttpFieldListRole::kSender)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
        }

        if (!addHeadBytes(facts.wireBytes, name.size()) || !addHeadBytes(facts.wireBytes, 2) ||
            !addHeadBytes(facts.wireBytes, value.size()) ||
            !addHeadBytes(facts.wireBytes, kCrlf.size())) {
            error = Http1ClientRequestPrepareError::kHeaderTooLarge;
            return false;
        }
    }
    if (facts.upgradeProtocols.hasField() && !facts.connectionOptions.upgrade()) {
        error = Http1ClientRequestPrepareError::kUpgradeConnectionOptionRequired;
        return false;
    }
    if (facts.hasTe && !facts.connectionOptions.te()) {
        error = Http1ClientRequestPrepareError::kTeConnectionOptionRequired;
        return false;
    }
    return true;
}

}  // namespace ruvia
