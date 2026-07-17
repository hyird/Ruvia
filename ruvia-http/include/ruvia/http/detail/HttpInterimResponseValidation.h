#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/HttpContentCoding.h"
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

// Incremental form of the version-neutral interim-field contract. Senders use
// it while validating a complete HttpInterimResponseHead; receivers use the
// same state while HPACK progressively yields fields. Keeping the seen-known-
// field set here prevents protocol drivers from reimplementing the forbidden
// field and singleton rules with subtly different wire acceptance.
class HttpInterimResponseHeaderValidator final {
public:
    explicit HttpInterimResponseHeaderValidator(
        HttpFieldListRole role) noexcept
        : role_(role) {}

    [[nodiscard]] HttpInterimResponseHeaderValidationStatus validate(
        std::string_view name,
        std::string_view value) noexcept {
        if (!isValidHttpHeaderName(name) ||
            !isValidHttpHeaderValue(value)) {
            return HttpInterimResponseHeaderValidationStatus::kInvalidHeader;
        }

        const auto knownBit = classifyResponseHeaderName(name);
        if (knownBit == kResponseHeaderContentEncoding &&
            !isValidHttpContentEncodingFieldValue(value, role_)) {
            return HttpInterimResponseHeaderValidationStatus::kInvalidHeader;
        }
        if (knownBit == kResponseHeaderContentLength) {
            return HttpInterimResponseHeaderValidationStatus::
                kContentLengthForbidden;
        }
        if (knownBit == kResponseHeaderTransferEncoding) {
            return HttpInterimResponseHeaderValidationStatus::
                kTransferEncodingForbidden;
        }
        if (httpAsciiEqualsIgnoreCase(name, "Trailer")) {
            return HttpInterimResponseHeaderValidationStatus::
                kTrailerForbidden;
        }
        if (knownBit != 0 &&
            (knownBits_ & knownBit) != 0 &&
            responseHeaderAppendForbidden(knownBit)) {
            return HttpInterimResponseHeaderValidationStatus::
                kRepeatedSingleton;
        }
        knownBits_ |= knownBit;
        return HttpInterimResponseHeaderValidationStatus::kOk;
    }

private:
    HttpFieldListRole role_;
    std::uint32_t knownBits_{0};
};

// Version-neutral interim-message validation. RFC 9110 section 8.6 and RFC
// 9112 section 6 forbid Content-Length and Transfer-Encoding on every 1xx;
// an interim response also cannot have the trailer section advertised by
// Trailer. Version-specific connection fields are checked by the HTTP/1 and
// HTTP/2 writers after this common pass.
[[nodiscard]] inline HttpInterimResponseHeaderValidationStatus
validateHttpInterimResponseHeaders(
    const HttpInterimResponseHead& response) noexcept {
    HttpInterimResponseHeaderValidator validator(
        HttpFieldListRole::kSender);
    for (const auto& header : response.headers()) {
        const auto status = validator.validate(
            header.name(), header.value());
        if (status != HttpInterimResponseHeaderValidationStatus::kOk) {
            return status;
        }
    }
    return HttpInterimResponseHeaderValidationStatus::kOk;
}

}  // namespace ruvia::detail
