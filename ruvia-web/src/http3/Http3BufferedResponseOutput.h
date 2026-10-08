#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/http3_buffered_response_cursor.h"

#include "http3/http3_stream_buffer.h"

namespace ruvia::detail {

enum class Http3BufferedResponseOutputError : std::uint8_t {
    kNone,
    kInvalidResponsePlan,
    kFileBodyUnsupported,
    kResponseEncoding,
    kOutOfMemory,
    kInvalidCursorState,
    kCursorAcknowledgement,
    kCursorDataPlan,
    kPeerFieldSectionLimit,
    buffer_stopped,
    kWireByteCountOverflow,
    kStopped,
};

// Worker-affine publisher for one buffered response. The caller owns and must
// retain response until this output completes or fails. WorkerMemory and the
// buffer must also outlive it. The output owns only the cursor and copies its
// wire bytes into the buffer. It does not drive QUIC:
// partial/WANT handling remains the responsibility of Http3ServerStreamOutput.
class Http3BufferedResponseOutput final {
public:
    using Error = Http3BufferedResponseOutputError;
    using MessageId = http3_stream_id;
    using NextStep = ruvia::http3_buffered_response_cursor::step;

    enum class Status : std::uint8_t {
        kBytes,
        kFin,
        kBackpressured,
        kComplete,
        kFailed,
    };

    enum class BlockReason : std::uint8_t {
        kNone,
        kData,
        kControl,
    };

    struct Result final {
        Status status{Status::kFailed};
        BlockReason blockReason{BlockReason::kNone};
        Error error{Error::kNone};
        std::size_t bytesAccepted{};
        std::uint64_t publishedWireBytes{};
    };

    // peerMaxFieldSectionSize is the peer's effective advisory limit, if known.
    // An over-limit response is rejected before any HEADERS bytes can be handed
    // to the buffer. The plan should come from planBufferedHttpResponseWrite().
    [[nodiscard]] static std::expected<Http3BufferedResponseOutput, Error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan,
        WorkerMemory& worker, http3_stream_buffer& buffer, MessageId messageId,
        std::optional<std::uint64_t> peerMaxFieldSectionSize = std::nullopt, std::uint64_t initialPublishedWireBytes = 0) noexcept;

    [[nodiscard]] static std::expected<Http3BufferedResponseOutput, Error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan, Http3ResponseHead encodedHead,
        WorkerMemory& worker, http3_stream_buffer& buffer, MessageId messageId,
        std::optional<std::uint64_t> peerMaxFieldSectionSize = std::nullopt, std::uint64_t initialPublishedWireBytes = 0) noexcept;

    Http3BufferedResponseOutput(const Http3BufferedResponseOutput&) = delete;
    Http3BufferedResponseOutput& operator=(const Http3BufferedResponseOutput&) = delete;
    // As with the cursor, moving is invalid while a published segment is awaiting
    // its acknowledgement. Normally owners construct output items in place.
    Http3BufferedResponseOutput(Http3BufferedResponseOutput&&) = default;
    Http3BufferedResponseOutput& operator=(Http3BufferedResponseOutput&&) = delete;

    // One call publishes at most one clipped DATA block or one FIN control.
    // kComplete means the FIN was accepted by the buffer, not written to or
    // acknowledged by the QUIC peer. Backpressure never acknowledges the cursor.
    [[nodiscard]] Result publishStep() noexcept;

    // Terminally abandon unpublished cursor state. Already accepted buffer
    // bytes remain owned by the buffer until the consumer releases them.
    void stop() noexcept;

    [[nodiscard]] NextStep nextStep() const noexcept;
    [[nodiscard]] std::size_t decodedFieldSectionSize() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Cumulative HTTP/3 wire bytes accepted by the buffer, including frame bytes.
    [[nodiscard]] std::uint64_t publishedWireBytes() const noexcept;
    [[nodiscard]] const MessageId& messageId() const noexcept {
        return messageId_;
    }

private:
    enum class State : std::uint8_t { kPublishing,
        kComplete,
        kFailed };

    Http3BufferedResponseOutput(const HttpResponse& response,
        http3_stream_buffer& buffer, MessageId messageId,
        ruvia::http3_buffered_response_cursor cursor, std::uint64_t initialPublishedWireBytes) noexcept;

    [[nodiscard]] static Error cursorError(ruvia::http3_buffered_response_cursor::error error) noexcept;
    [[nodiscard]] Result fail(Error error, std::size_t bytesAccepted = 0) noexcept;
    [[nodiscard]] Result result(Status status, BlockReason blockReason = BlockReason::kNone,
        Error error = Error::kNone, std::size_t bytesAccepted = 0) const noexcept;

    const HttpResponse* response_{};
    http3_stream_buffer& buffer_;
    const MessageId messageId_;
    std::optional<ruvia::http3_buffered_response_cursor> cursor_;
    std::uint64_t publishedWireBytes_{};
    State state_{State::kPublishing};
    Error failure_{Error::kNone};
};

}  // namespace ruvia::detail
