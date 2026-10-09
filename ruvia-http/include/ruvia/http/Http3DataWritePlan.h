#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class Http3DataWriteError : std::uint8_t {
    kFrameHeaderEncoding,
    kVarIntOutOfRange,
    kContentLengthOverflow,
    kContentLengthMismatch,
    kContentLengthForbidden,
    kBodyNotAllowed,
    kWriteAlreadyPending,
    kNoWritePending,
    kCommitDoesNotMatchPlan,
    kAlreadyFinished,
};

// Pure protocol state for planning DATA frame writes for responses and outbound
// requests. It owns no payload and performs no allocation; all state is stored
// inline and needs no PMR resource. Destroying a plan never commits pending
// bytes or implicitly finishes the stream.
class Http3DataWritePlan final {
public:
    struct Chunk final {
        std::array<char, kHttp3FrameHeaderMaxBytes> frameHeader{};
        std::size_t frameHeaderSize{0};
        std::span<const char> payload{};  // Borrowed from the caller.
        bool emitsData{false};
        bool finishing{false};
    };

    Http3DataWritePlan(HttpResponseBodyPlan bodyPlan,
        std::optional<std::uint64_t> declaredContentLength) noexcept;
    // Request bodies are not subject to response status/method body rules.
    explicit Http3DataWritePlan(Http3ClientRequestBodyPlan bodyPlan) noexcept;

    [[nodiscard]] bool bodyAllowed() const noexcept;
    // Whether the stream may be concluded now without sending more DATA.
    [[nodiscard]] bool finAllowed() const noexcept;
    [[nodiscard]] std::uint64_t committedPayloadBytes() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

    // The returned payload view borrows the caller's chunk. The runtime must
    // retain that chunk until every partial SSL_write_ex write has completed.
    // To end a headers-only body, plan an empty finishing chunk and commit it;
    // no zero-length DATA frame is emitted.
    [[nodiscard]] std::variant<Chunk, Http3DataWriteError> planChunk(
        std::span<const char> payload, bool finishing) noexcept;

    // Call only after the planned frame header and all payload bytes have been
    // fully written. The runtime must signal FIN with SSL_stream_conclude or
    // SSL_write_ex2; on transport failure do not commit and RESET_STREAM instead.
    [[nodiscard]] std::variant<std::monostate, Http3DataWriteError> commitPayload(
        std::uint64_t bytes, bool finishing) noexcept;

private:
    std::optional<HttpResponseBodyPlan> responseBodyPlan_;
    std::optional<std::uint64_t> declaredContentLength_;
    bool requestBody_{false};
    std::uint64_t committedPayloadBytes_{0};
    std::uint64_t pendingPayloadBytes_{0};
    bool pendingFinishing_{false};
    bool writePending_{false};
    bool finished_{false};
};

}  // namespace ruvia
