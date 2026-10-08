#include "ruvia/http/HttpAcceptEncoding.h"

#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"

#include "field/HttpQualityValue.h"

namespace ruvia {

void HttpAcceptedEncodingQuality::update(
    std::string_view acceptEncoding, std::string_view coding) noexcept {
    const bool gzip_coding = http_is_gzip_coding_token(coding);
    detail::httpVisitCommaSeparatedQuoted(acceptEncoding,
        [coding, gzip_coding, this](std::string_view item) noexcept {
            const auto token = detail::httpHeaderTokenBeforeParameters(item);
            if (gzip_coding ? http_is_gzip_coding_token(token) : detail::httpAsciiEqualsIgnoreCase(token, coding)) {
                detail::httpAccumulateAcceptedQuality(
                    detail::http_weight_parameter(item), explicitQuality);
            } else if (token == "*") {
                detail::httpAccumulateAcceptedQuality(
                    detail::http_weight_parameter(item), wildcardQuality);
            }
            return true;
        });
}

bool httpAcceptsEncoding(std::string_view acceptEncoding, std::string_view coding) noexcept {
    if (acceptEncoding.empty()) {
        return detail::httpAsciiEqualsIgnoreCase(coding, "identity");
    }
    HttpAcceptedEncodingQuality quality;
    quality.update(acceptEncoding, coding);
    return quality.accepts(detail::httpAsciiEqualsIgnoreCase(coding, "identity"));
}

void HttpResponseCodingQualities::update(std::string_view acceptEncoding) noexcept {
    fieldPresent = true;
    detail::httpVisitCommaSeparatedQuoted(acceptEncoding, [this](std::string_view item) noexcept {
        const auto token = detail::httpHeaderTokenBeforeParameters(item);
        hasNonEmptyItem = true;
        if (http_is_gzip_coding_token(token)) {
            detail::httpAccumulateAcceptedQuality(
                detail::http_weight_parameter(item), gzip.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "deflate")) {
            detail::httpAccumulateAcceptedQuality(
                detail::http_weight_parameter(item), deflate.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "br")) {
            detail::httpAccumulateAcceptedQuality(
                detail::http_weight_parameter(item), brotli.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "zstd")) {
            detail::httpAccumulateAcceptedQuality(
                detail::http_weight_parameter(item), zstd.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "identity")) {
            detail::httpAccumulateAcceptedQuality(
                detail::http_weight_parameter(item), identity.explicitQuality);
        } else if (token == "*") {
            const auto wildcard = detail::http_weight_parameter(item);
            detail::httpAccumulateAcceptedQuality(wildcard, gzip.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, deflate.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, brotli.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, zstd.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, identity.wildcardQuality);
        }
        return true;
    });
}

}  // namespace ruvia
