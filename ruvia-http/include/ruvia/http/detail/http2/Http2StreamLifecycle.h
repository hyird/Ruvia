#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class Http2StreamCloseSource : std::uint8_t {
    kNone,
    kLocal,
    kPeer,
    // The peer's GOAWAY proved this locally initiated request was never processed.
    kPeerGoaway
};

// The local HTTP message submission phase. This is intentionally separate from
// the peer/body state below: a server can be half-closed(remote) while its local
// response is still awaiting HEADERS, and a client can have ended its request
// while the response remains open.
enum class Http2LocalSendPhase : std::uint8_t {
    kAwaitingHead,
    // CONNECT HEADERS were sent without END_STREAM; DATA remains forbidden until
    // the peer accepts the tunnel with a final 2xx response.
    kAwaitingTunnelResponse,
    kBodyOpen,
    // The initial response head is open only for a terminal trailer HEADERS
    // section. DATA is rejected by phase, not by a second content-mode check.
    kTrailersOnly,
    // END_STREAM has been accepted by the core. It may already be serialized or
    // may still sit behind flow-control-blocked DATA/trailers.
    kEndStreamQueued,
    kReset
};

enum class Http2LocalMessageKind : std::uint8_t {
    kNone,
    kRequest,
    kResponse,
    kConnectTunnel
};

class Http2StreamLifecycle final {
public:
    [[nodiscard]] bool reset() const noexcept {
        return reset_;
    }

    [[nodiscard]] bool bodyEnded() const noexcept {
        return bodyEnded_;
    }

    [[nodiscard]] bool peerEndStream() const noexcept {
        return peerEndStream_;
    }

    [[nodiscard]] bool localEndStream() const noexcept {
        return localSendPhase_ == Http2LocalSendPhase::kEndStreamQueued ||
            localSendPhase_ == Http2LocalSendPhase::kReset;
    }

    [[nodiscard]] bool localEndStreamCommitted() const noexcept {
        return localEndStreamCommitted_;
    }

    [[nodiscard]] Http2LocalSendPhase localSendPhase() const noexcept {
        return localSendPhase_;
    }

    [[nodiscard]] Http2LocalMessageKind localMessageKind() const noexcept {
        return localMessageKind_;
    }

    [[nodiscard]] bool queued() const noexcept {
        return queued_;
    }

    [[nodiscard]] bool dispatchStarted() const noexcept {
        return dispatchStarted_;
    }

    [[nodiscard]] Http2StreamCloseSource closeSource() const noexcept {
        return closeSource_;
    }

    [[nodiscard]] bool holdPeerConcurrencySlot() noexcept {
        if (peerConcurrencySlotHeld_) {
            return false;
        }
        peerConcurrencySlotHeld_ = true;
        return true;
    }

    [[nodiscard]] bool releasePeerConcurrencySlot() noexcept {
        if (!peerConcurrencySlotHeld_) {
            return false;
        }
        peerConcurrencySlotHeld_ = false;
        return true;
    }

    void markReset(Http2StreamCloseSource source = Http2StreamCloseSource::kLocal) noexcept {
        reset_ = true;
        localSendPhase_ = Http2LocalSendPhase::kReset;
        if (source != Http2StreamCloseSource::kNone) {
            closeSource_ = source;
        }
    }

    void markClosed(Http2StreamCloseSource source) noexcept {
        reset_ = true;
        peerEndStream_ = true;
        bodyEnded_ = true;
        localSendPhase_ = Http2LocalSendPhase::kReset;
        closeSource_ = source;
    }

    void markPeerEndStream() noexcept {
        peerEndStream_ = true;
    }

    [[nodiscard]] bool canSubmitLocalHead() const noexcept {
        return !reset_ && localSendPhase_ == Http2LocalSendPhase::kAwaitingHead;
    }

    void markLocalHeadSubmitted(
        Http2LocalMessageKind kind,
        bool endStream) noexcept {
        localMessageKind_ = kind;
        localSendPhase_ = endStream
            ? Http2LocalSendPhase::kEndStreamQueued
            : Http2LocalSendPhase::kBodyOpen;
        localEndStreamCommitted_ = endStream;
    }

    void markLocalTrailersOnlyHeadSubmitted(
        Http2LocalMessageKind kind) noexcept {
        localMessageKind_ = kind;
        localSendPhase_ = Http2LocalSendPhase::kTrailersOnly;
        localEndStreamCommitted_ = false;
    }

    void markLocalConnectRequestSubmitted() noexcept {
        localMessageKind_ = Http2LocalMessageKind::kConnectTunnel;
        localSendPhase_ = Http2LocalSendPhase::kAwaitingTunnelResponse;
        localEndStreamCommitted_ = false;
    }

    [[nodiscard]] bool openLocalConnectTunnel() noexcept {
        if (reset_ ||
            localMessageKind_ != Http2LocalMessageKind::kConnectTunnel ||
            localSendPhase_ != Http2LocalSendPhase::kAwaitingTunnelResponse) {
            return false;
        }
        localSendPhase_ = Http2LocalSendPhase::kBodyOpen;
        return true;
    }

    [[nodiscard]] bool rejectLocalConnect() noexcept {
        if (reset_ ||
            localMessageKind_ != Http2LocalMessageKind::kConnectTunnel ||
            localSendPhase_ != Http2LocalSendPhase::kAwaitingTunnelResponse) {
            return false;
        }
        localSendPhase_ = Http2LocalSendPhase::kEndStreamQueued;
        localEndStreamCommitted_ = true;
        return true;
    }

    [[nodiscard]] bool localBodyOpen() const noexcept {
        return !reset_ && localSendPhase_ == Http2LocalSendPhase::kBodyOpen;
    }

    [[nodiscard]] bool localTrailersOnly() const noexcept {
        return !reset_ && localSendPhase_ == Http2LocalSendPhase::kTrailersOnly;
    }

    void markLocalEndStreamQueued() noexcept {
        if (!reset_) {
            localSendPhase_ = Http2LocalSendPhase::kEndStreamQueued;
        }
    }

    void markLocalEndStreamCommitted() noexcept {
        if (!reset_) {
            localSendPhase_ = Http2LocalSendPhase::kEndStreamQueued;
            localEndStreamCommitted_ = true;
        }
    }

    void markBodyEnded() noexcept {
        bodyEnded_ = true;
    }

    [[nodiscard]] bool tryMarkQueued() noexcept {
        if (queued_ || reset_) {
            return false;
        }
        queued_ = true;
        return true;
    }

    void clearQueued() noexcept {
        queued_ = false;
    }

    [[nodiscard]] bool tryStartDispatch() noexcept {
        queued_ = false;
        if (dispatchStarted_ || reset_) {
            return false;
        }
        dispatchStarted_ = true;
        return true;
    }

    void markDispatchStarted() noexcept {
        dispatchStarted_ = true;
    }

private:
    bool peerEndStream_ : 1 {false};
    bool localEndStreamCommitted_ : 1 {false};
    bool bodyEnded_ : 1 {false};
    bool queued_ : 1 {false};
    bool dispatchStarted_ : 1 {false};
    bool reset_ : 1 {false};
    bool peerConcurrencySlotHeld_ : 1 {false};
    Http2LocalSendPhase localSendPhase_{Http2LocalSendPhase::kAwaitingHead};
    Http2LocalMessageKind localMessageKind_{Http2LocalMessageKind::kNone};
    Http2StreamCloseSource closeSource_{Http2StreamCloseSource::kNone};
};

}  // namespace ruvia::detail
