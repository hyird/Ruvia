#include "ruvia/http/HttpAcceptEncoding.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"

namespace ruvia {
namespace {

// Accept-Encoding uses `codings [ weight ]`, not an arbitrary parameter list.
[[nodiscard]] int encodingQualityParameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = value.substr(semicolon + 1);
    while (!weight.empty() && (weight.front() == ' ' || weight.front() == '\t')) {
        weight.remove_prefix(1);
    }
    if (weight.size() < 3 || detail::httpAsciiToLower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != detail::httpTrimOws(qvalue)) {
        return 0;
    }
    const auto parsed = detail::httpParseQualityValue(qvalue);
    return parsed < 0 ? 0 : parsed;
}

}  // namespace

void HttpAcceptedEncodingQuality::update(
    std::string_view acceptEncoding, std::string_view coding) noexcept {
    detail::httpVisitCommaSeparatedQuoted(acceptEncoding,
        [coding, this](std::string_view item) noexcept {
            const auto token = detail::httpHeaderTokenBeforeParameters(item);
            if (detail::httpAsciiEqualsIgnoreCase(token, coding)) {
                detail::httpAccumulateAcceptedQuality(
                    encodingQualityParameter(item), explicitQuality);
            } else if (token == "*") {
                detail::httpAccumulateAcceptedQuality(
                    encodingQualityParameter(item), wildcardQuality);
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
    return quality.accepts();
}

void HttpResponseCodingQualities::update(std::string_view acceptEncoding) noexcept {
    fieldPresent = true;
    detail::httpVisitCommaSeparatedQuoted(acceptEncoding, [this](std::string_view item) noexcept {
        const auto token = detail::httpHeaderTokenBeforeParameters(item);
        hasNonEmptyItem = true;
        if (detail::httpAsciiEqualsIgnoreCase(token, "gzip")) {
            detail::httpAccumulateAcceptedQuality(
                encodingQualityParameter(item), gzip.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "br")) {
            detail::httpAccumulateAcceptedQuality(
                encodingQualityParameter(item), brotli.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "zstd")) {
            detail::httpAccumulateAcceptedQuality(
                encodingQualityParameter(item), zstd.explicitQuality);
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "identity")) {
            detail::httpAccumulateAcceptedQuality(
                encodingQualityParameter(item), identity.explicitQuality);
        } else if (token == "*") {
            const auto wildcard = encodingQualityParameter(item);
            detail::httpAccumulateAcceptedQuality(wildcard, gzip.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, brotli.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, zstd.wildcardQuality);
            detail::httpAccumulateAcceptedQuality(wildcard, identity.wildcardQuality);
        }
        return true;
    });
}

}  // namespace ruvia
