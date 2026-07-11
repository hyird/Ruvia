#pragma once

#include <cstdint>

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/HttpResponseKnownHeaders.h"

namespace ruvia::detail {

enum class HttpInterimResponseHeaderValidationStatus : std::uint8_t {
    kOk,
    kInvalidHeader,
    kContentLengthForbidden,
    kTransferEncodingForbidden,
    kTrailerForbidden,
    kRepeatedSingleton,
};

// Version-neutral interim-message validation. RFC 9110 section 8.6 and RFC
// 9112 section 6 forbid Content-Length and Transfer-Encoding on every 1xx;
// an interim response also cannot have the trailer section advertised by
// Trailer. Version-specific connection fields are checked by the HTTP/1 and
// HTTP/2 writers after this common pass.
[[nodiscard]] inline HttpInterimResponseHeaderValidationStatus
validateHttpInterimResponseHeaders(
    const HttpInterimResponseHead& response) noexcept {
    std::uint32_t knownBits = 0;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        if (!isValidHttpHeaderName(name) ||
            !isValidHttpHeaderValue(header.value())) {
            return HttpInterimResponseHeaderValidationStatus::kInvalidHeader;
        }

        const auto knownBit = classifyResponseHeaderName(name);
        if (knownBit == kResponseHeaderContentLength) {
            return HttpInterimResponseHeaderValidationStatus::
                kContentLengthForbidden;
        }
        if (knownBit == kResponseHeaderTransferEncoding) {
            return HttpInterimResponseHeaderValidationStatus::
                kTransferEncodingForbidden;
        }
        if (httpAsciiEqualsIgnoreCase(name, "Trailer")) {
            return HttpInterimResponseHeaderValidationStatus::kTrailerForbidden;
        }
        if (knownBit != 0 &&
            (knownBits & knownBit) != 0 &&
            responseHeaderAppendForbidden(knownBit)) {
            return HttpInterimResponseHeaderValidationStatus::kRepeatedSingleton;
        }
        knownBits |= knownBit;
    }
    return HttpInterimResponseHeaderValidationStatus::kOk;
}

}  // namespace ruvia::detail
