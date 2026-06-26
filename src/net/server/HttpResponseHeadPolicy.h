#pragma once

#include <cstdint>

#include "../../http/HttpResponseHeaderBits.h"

namespace ruvia::detail {

class ResponseWritePolicy final {
public:
    [[nodiscard]] static constexpr ResponseWritePolicy normal() noexcept {
        return ResponseWritePolicy(true, true, true, true);
    }

    [[nodiscard]] static constexpr ResponseWritePolicy bodyForbidden() noexcept {
        return ResponseWritePolicy(false, false, false, false);
    }

    [[nodiscard]] static constexpr ResponseWritePolicy notModified() noexcept {
        return ResponseWritePolicy(false, false, true, false);
    }

    [[nodiscard]] constexpr bool bodyAllowed() const noexcept {
        return bodyAllowed_;
    }

    [[nodiscard]] constexpr bool autoContentLengthAllowed() const noexcept {
        return autoContentLengthAllowed_;
    }

    [[nodiscard]] constexpr bool explicitContentLengthAllowed() const noexcept {
        return explicitContentLengthAllowed_;
    }

    [[nodiscard]] constexpr bool transferEncodingAllowed() const noexcept {
        return transferEncodingAllowed_;
    }

private:
    constexpr ResponseWritePolicy(
        bool bodyAllowed,
        bool autoContentLengthAllowed,
        bool explicitContentLengthAllowed,
        bool transferEncodingAllowed) noexcept
        : bodyAllowed_(bodyAllowed),
          autoContentLengthAllowed_(autoContentLengthAllowed),
          explicitContentLengthAllowed_(explicitContentLengthAllowed),
          transferEncodingAllowed_(transferEncodingAllowed) {}

    bool bodyAllowed_{true};
    bool autoContentLengthAllowed_{true};
    bool explicitContentLengthAllowed_{true};
    bool transferEncodingAllowed_{true};
};

[[nodiscard]] inline ResponseWritePolicy responseWritePolicy(std::uint16_t statusCode) noexcept {
    if (statusCode >= 100 && statusCode < 200) {
        return ResponseWritePolicy::bodyForbidden();
    }
    if (statusCode == 204 || statusCode == 205) {
        return ResponseWritePolicy::bodyForbidden();
    }
    if (statusCode == 304) {
        return ResponseWritePolicy::notModified();
    }
    return ResponseWritePolicy::normal();
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
