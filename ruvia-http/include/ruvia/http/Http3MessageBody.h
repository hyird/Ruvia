#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

// Result of feeding the size of one HTTP/3 DATA payload and its stream FIN.
enum class Http3MessageBodyResult : std::uint8_t {
    kAccepted,
    kComplete,
    kPayloadNotAllowed,
    kContentLengthExceeded,
    kContentLengthMismatch,
    kLengthOverflow,
    kAlreadyComplete,
    kAlreadyFailed,
};

// Shared request/response DATA and FIN accounting state. contentLength is the
// expected representation length (when present), not a framing length.
class Http3MessageBody final {
public:
    enum class State : std::uint8_t {
        kReceiving,
        kComplete,
        kFailed,
    };

    Http3MessageBody(std::optional<std::uint64_t> contentLength, bool payloadAllowed) noexcept;

    // dataLength is only accounted; payload bytes remain owned by the caller.
    // Any protocol error is terminal. FIN succeeds only when Content-Length,
    // if present, exactly matches the total DATA payload length.
    [[nodiscard]] Http3MessageBodyResult feed(std::uint64_t dataLength, bool fin) noexcept;

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] std::uint64_t receivedLength() const noexcept;

private:
    std::optional<std::uint64_t> contentLength_{};
    std::uint64_t receivedLength_{0};
    bool payloadAllowed_{true};
    State state_{State::kReceiving};
};

}  // namespace ruvia
