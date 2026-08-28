#pragma once

#include "ruvia/http/detail/coding/HttpTransferEncoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"
#include "ruvia/http/detail/util/AsciiCase.h"

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

enum class HttpTeFieldValidationMode : std::uint8_t { kRecipient, kClientCapability };

[[nodiscard]] inline bool httpIsClientSupportedTeTransferCoding(std::string_view coding) noexcept {
    return httpAsciiEqualsIgnoreCase(coding, "gzip") || httpAsciiEqualsIgnoreCase(coding, "x-gzip") || httpAsciiEqualsIgnoreCase(coding, "deflate");
}

[[nodiscard]] inline bool httpTeCodingAllowsOnlyQualityParameter(std::string_view coding) noexcept {
    return httpAsciiEqualsIgnoreCase(coding, "compress") || httpAsciiEqualsIgnoreCase(coding, "gzip") || httpAsciiEqualsIgnoreCase(coding, "x-gzip") || httpAsciiEqualsIgnoreCase(coding, "deflate");
}

[[nodiscard]] inline bool httpTeParametersAreValid(std::string_view item, HttpTeFieldValidationMode mode, bool onlyQualityParameter) noexcept {
    auto start = httpFindUnquotedDelimiter(item, 0, ';');
    if (start >= item.size()) {
        return true;
    }

    bool qualitySeen = false;
    ++start;
    while (start <= item.size()) {
        const auto end = httpFindUnquotedDelimiter(item, start, ';');
        const auto parameter = httpTrimOws(item.substr(start, end - start));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }

        const auto rawName = parameter.substr(0, equals);
        const auto rawValue = parameter.substr(equals + 1);
        const auto name = httpTrimOws(rawName);
        const auto value = httpTrimOws(rawValue);
        if (httpAsciiEqualsIgnoreCase(name, "q")) {
            if (qualitySeen || rawName != name || rawValue != value || httpParseQualityValue(value) < 0) {
                return false;
            }
            qualitySeen = true;
        } else if (mode == HttpTeFieldValidationMode::kClientCapability || onlyQualityParameter) {
            return false;
        }

        if (end >= item.size()) {
            return true;
        }
        start = end + 1;
    }

    return true;
}

[[nodiscard]] inline bool isValidHttpTeFieldItem(std::string_view item, HttpTeFieldValidationMode mode) noexcept {
    std::string_view coding;
    bool hasParameters = false;
    if (!httpParseTransferCodingSyntax(item, coding, hasParameters)) {
        return false;
    }
    if (httpAsciiEqualsIgnoreCase(coding, "trailers")) {
        return !hasParameters;
    }
    if (httpAsciiEqualsIgnoreCase(coding, "chunked")) {
        return false;
    }
    if (mode == HttpTeFieldValidationMode::kClientCapability && !httpIsClientSupportedTeTransferCoding(coding)) {
        return false;
    }
    return httpTeParametersAreValid(item, mode, httpTeCodingAllowsOnlyQualityParameter(coding));
}

[[nodiscard]] inline bool isValidHttpTeFieldValue(std::string_view value, HttpTeFieldValidationMode mode) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (httpTrimOws(value).empty()) {
        return true;
    }

    bool valid = true;
    bool sawItem = false;
    httpVisitCommaSeparatedQuotedItems(value, [&valid, &sawItem, mode](std::string_view item) noexcept {
        sawItem = true;
        if (!isValidHttpTeFieldItem(item, mode)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid && sawItem;
}

[[nodiscard]] inline bool isValidReceivedHttpTeFieldValue(std::string_view value) noexcept {
    return isValidHttpTeFieldValue(value, HttpTeFieldValidationMode::kRecipient);
}

[[nodiscard]] inline bool isValidClientHttpTeFieldValue(std::string_view value) noexcept {
    return isValidHttpTeFieldValue(value, HttpTeFieldValidationMode::kClientCapability);
}

}  // namespace ruvia::detail
