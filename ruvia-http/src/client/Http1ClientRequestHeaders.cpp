#include "ruvia/http/detail/client/Http1ClientRequestHeaders.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
namespace ruvia {

namespace {

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
    // RFC 9110 section 12.4.2 defines weight with the exact `q=` literal.
    // BWS around '=' belongs to transfer-parameter syntax instead; treating
    // `q =` as a weight would emit an undefined gzip/deflate parameter.
    if (cursor + 2 > item.size() ||
        detail::httpAsciiToLower(static_cast<unsigned char>(item[cursor])) != 'q' ||
        item[cursor + 1] != '=') {
        return false;
    }
    cursor += 2;
    const auto quality = parseToken();
    if (quality.empty() || detail::httpParseQualityValue(quality) < 0) {
        return false;
    }
    skipOws();
    return cursor == item.size();
}

}  // namespace

bool addHeadBytes(
    std::size_t& total,
    std::size_t bytes) noexcept {
    if (bytes > kMaxHttpHeaderBytes - total) {
        return false;
    }
    total += bytes;
    return true;
}

bool isValidClientTeField(std::string_view value) noexcept {
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

bool analyzeHeaders(
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
            if (facts.connectionOptions.parseField(
                    value,
                    detail::HttpFieldListRole::kSender,
                    [](std::string_view option) noexcept {
                        return !detail::httpConnectionOptionConflictsWithManagedField(
                            option);
                    }) != detail::HttpFieldListParseStatus::kOk) {
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
            if (!detail::isValidHttpContentTypeFieldValue(value)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
            facts.hasContentType = true;
        } else if (detail::httpAsciiEqualsIgnoreCase(
                       name, "Content-Encoding")) {
            if (!detail::isValidHttpContentEncodingFieldValue(
                    value, detail::HttpFieldListRole::kSender)) {
                error = Http1ClientRequestPrepareError::kInvalidHeader;
                return false;
            }
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

}  // namespace ruvia
