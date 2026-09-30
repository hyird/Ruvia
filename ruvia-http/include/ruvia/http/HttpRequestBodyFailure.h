#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "ruvia/http/Http1ChunkDecodeError.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpTransferCodingDecodeError.h"
#include "ruvia/http/ProtocolByteLimit.h"

namespace ruvia {

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
class HttpRequestBodyFailure final {
public:
    [[nodiscard]] static constexpr HttpRequestBodyFailure tooLarge() noexcept {
        return HttpRequestBodyFailure(Kind::kTooLarge);
    }

    [[nodiscard]] static constexpr HttpRequestBodyFailure incomplete() noexcept {
        return HttpRequestBodyFailure(Kind::kIncomplete);
    }

    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        switch (kind_) {
            case Kind::kTooLarge:
                return HttpProtocolError(
                    http_status::kContentTooLarge, "request body is too large");
            case Kind::kIncomplete:
                return HttpProtocolError(http_status::kBadRequest, "incomplete request body");
        }
        return HttpProtocolError(http_status::kBadRequest, "invalid request body");
    }

private:
    enum class Kind : std::uint8_t { kTooLarge,
        kIncomplete };

    explicit constexpr HttpRequestBodyFailure(Kind kind) noexcept
        : kind_(kind) {}

    Kind kind_;
};

[[nodiscard]] inline HttpProtocolError httpRequestChunkDecodeError(
    Http1ChunkDecodeError error) noexcept {
    switch (error) {
        case Http1ChunkDecodeError::kInvalidFraming:
            return HttpProtocolError(http_status::kBadRequest, "invalid chunked request body");
        case Http1ChunkDecodeError::kBodyLimitExceeded:
            return HttpRequestBodyFailure::tooLarge().protocolError();
        case Http1ChunkDecodeError::kFramingLimitExceeded:
            return HttpProtocolError(
                http_status::kContentTooLarge, "request body framing is too large");
    }
    return HttpProtocolError(http_status::kBadRequest, "invalid chunked request body");
}

[[nodiscard]] inline HttpProtocolError httpRequestTransferCodingError(
    HttpTransferCodingDecodeError error) noexcept {
    switch (error) {
        case HttpTransferCodingDecodeError::kInvalidContent:
            return HttpProtocolError(http_status::kBadRequest, "invalid transfer-coding body");
        case HttpTransferCodingDecodeError::kDecodedSizeExceeded:
            return HttpRequestBodyFailure::tooLarge().protocolError();
    }
    return HttpProtocolError(http_status::kBadRequest, "invalid transfer-coding body");
}

[[nodiscard]] inline std::optional<HttpRequestBodyFailure> httpRequestBodySizeFailure(
    std::size_t size, ProtocolByteLimit limit) noexcept {
    return limit.exceeds(size)
               ? std::optional<HttpRequestBodyFailure>(HttpRequestBodyFailure::tooLarge())
               : std::nullopt;
}

[[nodiscard]] inline std::optional<HttpRequestBodyFailure> httpRequestBodyAdditionFailure(
    std::size_t currentSize, std::size_t additionalSize, ProtocolByteLimit limit) noexcept {
    return limit.additionExceeds(currentSize, additionalSize)
               ? std::optional<HttpRequestBodyFailure>(HttpRequestBodyFailure::tooLarge())
               : std::nullopt;
}

static_assert(std::is_trivially_copyable_v<HttpRequestBodyFailure>);
static_assert(sizeof(HttpRequestBodyFailure) <= 1);

}  // namespace ruvia
