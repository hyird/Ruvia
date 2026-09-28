#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/http3/Http3BufferedResponseWrite.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

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
    kMailboxStopped,
    kWireByteCountOverflow,
    kStopped,
};

// Worker-affine publisher for one buffered response. The caller owns and must
// retain response until this output completes or fails. WorkerMemory and the
// mailbox must also outlive it. The output owns only the cursor and copies its
// wire bytes into the mailbox. It does not drive QUIC:
// partial/WANT handling remains the responsibility of Http3ServerStreamOutput.
class Http3BufferedResponseOutput final {
public:
    using Error = Http3BufferedResponseOutputError;
    using MessageId = Http3StreamMessageId;
    using NextStep = Http3BufferedResponseWrite::NextStep;

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
        // Independent mailbox wakeup obligation. Honor it even when status is
        // kFailed: the data/control publication may already have succeeded.
        bool notifyPeer{};
    };

    // peerMaxFieldSectionSize is the peer's effective advisory limit, if known.
    // An over-limit response is rejected before any HEADERS bytes can be handed
    // to the mailbox. The plan should come from planBufferedHttpResponseWrite().
    [[nodiscard]] static std::expected<Http3BufferedResponseOutput, Error> create(
        const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan,
        WorkerMemory& worker, Http3StreamMailbox& mailbox, MessageId messageId,
        std::optional<std::uint64_t> peerMaxFieldSectionSize = std::nullopt) noexcept;

    Http3BufferedResponseOutput(const Http3BufferedResponseOutput&) = delete;
    Http3BufferedResponseOutput& operator=(const Http3BufferedResponseOutput&) = delete;
    // As with the cursor, moving is invalid while a published segment is awaiting
    // its acknowledgement. Normally owners construct output items in place.
    Http3BufferedResponseOutput(Http3BufferedResponseOutput&&) = default;
    Http3BufferedResponseOutput& operator=(Http3BufferedResponseOutput&&) = delete;

    // One call publishes at most one clipped DATA block or one FIN control.
    // kComplete means the FIN was accepted by the mailbox, not written to or
    // acknowledged by the QUIC peer. Backpressure never acknowledges the cursor.
    [[nodiscard]] Result publishStep() noexcept;

    // Terminally abandon unpublished cursor state. Previously returned
    // notifyPeer obligations and accepted mailbox bytes remain the owner's duty.
    void stop() noexcept;

    [[nodiscard]] NextStep nextStep() const noexcept;
    [[nodiscard]] std::size_t decodedFieldSectionSize() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Cumulative HTTP/3 wire bytes accepted by the mailbox, including frame bytes.
    [[nodiscard]] std::uint64_t publishedWireBytes() const noexcept;
    [[nodiscard]] const MessageId& messageId() const noexcept {
        return messageId_;
    }

private:
    enum class State : std::uint8_t { kPublishing,
        kComplete,
        kFailed };

    Http3BufferedResponseOutput(const HttpResponse& response,
        Http3StreamMailbox& mailbox, MessageId messageId,
        Http3BufferedResponseWrite cursor) noexcept;

    [[nodiscard]] static Error cursorError(Http3BufferedResponseWrite::Error error) noexcept;
    [[nodiscard]] Result fail(Error error, std::size_t bytesAccepted = 0,
        bool notifyPeer = false) noexcept;
    [[nodiscard]] Result result(Status status, BlockReason blockReason = BlockReason::kNone,
        Error error = Error::kNone, std::size_t bytesAccepted = 0,
        bool notifyPeer = false) const noexcept;

    const HttpResponse* response_{};
    Http3StreamMailbox& mailbox_;
    const MessageId messageId_;
    std::optional<Http3BufferedResponseWrite> cursor_;
    std::uint64_t publishedWireBytes_{};
    State state_{State::kPublishing};
    Error failure_{Error::kNone};
};

}  // namespace ruvia::detail
