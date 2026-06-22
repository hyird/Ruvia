#pragma once

#include <cstdint>

#include "../../http/HttpResponseHeaderBits.h"

namespace ruvia::detail {

struct ResponseWritePolicy {
    bool bodyAllowed{true};
    bool autoContentLengthAllowed{true};
    bool explicitContentLengthAllowed{true};
    bool transferEncodingAllowed{true};
};

[[nodiscard]] inline ResponseWritePolicy responseWritePolicy(std::uint16_t statusCode) noexcept {
    if (statusCode >= 100 && statusCode < 200) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = false,
            .transferEncodingAllowed = false};
    }
    if (statusCode == 204 || statusCode == 205) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = false,
            .transferEncodingAllowed = false};
    }
    if (statusCode == 304) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = true,
            .transferEncodingAllowed = false};
    }
    return {};
}

[[nodiscard]] inline bool responseBodyFramingHeaderForbidden(
    std::uint32_t knownBit,
    bool explicitContentLengthAllowed,
    bool transferEncodingAllowed) noexcept {
    return (!explicitContentLengthAllowed && knownBit == kResponseHeaderContentLength) ||
        (!transferEncodingAllowed && knownBit == kResponseHeaderTransferEncoding);
}

[[nodiscard]] inline bool responseHasForbiddenBodyFramingHeader(
    std::uint32_t knownBits,
    bool explicitContentLengthAllowed,
    bool transferEncodingAllowed) noexcept {
    return ((knownBits & kResponseHeaderContentLength) != 0 &&
               responseBodyFramingHeaderForbidden(
                   kResponseHeaderContentLength,
                   explicitContentLengthAllowed,
                   transferEncodingAllowed)) ||
        ((knownBits & kResponseHeaderTransferEncoding) != 0 &&
            responseBodyFramingHeaderForbidden(
                kResponseHeaderTransferEncoding,
                explicitContentLengthAllowed,
                transferEncodingAllowed));
}

}  // namespace ruvia::detail
