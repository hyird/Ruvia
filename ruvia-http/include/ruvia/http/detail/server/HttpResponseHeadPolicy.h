#pragma once

#include <cstdint>

#include "ruvia/http/detail/HttpResponseHeaderBits.h"

namespace ruvia::detail {

class ResponseWritePolicy final {
public:
    [[nodiscard]] static constexpr ResponseWritePolicy normal() noexcept {
        return ResponseWritePolicy(true, true, true, true);
    }

    [[nodiscard]] static constexpr ResponseWritePolicy bodyForbidden() noexcept {
        return ResponseWritePolicy(false, false, false, false);
    }

    [[nodiscard]] static constexpr ResponseWritePolicy zeroLengthContent() noexcept {
        return ResponseWritePolicy(false, true, false, false);
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
    if (statusCode == 204) {
        return ResponseWritePolicy::bodyForbidden();
    }
    if (statusCode == 205) {
        // RFC 9110 15.3.6 forbids content in a 205 response. Unlike 1xx/204/304,
        // HTTP/1.1 message framing does not make 205 self-delimiting from the
        // status alone, so the writer owns a canonical Content-Length: 0. A
        // caller-provided length and Transfer-Encoding are filtered instead of
        // creating a second, potentially contradictory framing declaration.
        return ResponseWritePolicy::zeroLengthContent();
    }
    if (statusCode == 304) {
        return ResponseWritePolicy::notModified();
    }
    return ResponseWritePolicy::normal();
}

}  // namespace ruvia::detail
