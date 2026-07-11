#pragma once

#include "ruvia/http/detail/http2/Http2LocalSendState.h"

namespace ruvia::detail {

class Http2StreamState;

class Http2StreamLifecycle final {
public:
    [[nodiscard]] bool aborted() const noexcept {
        return localSend_.aborted() != nullptr;
    }

    [[nodiscard]] bool bodyEnded() const noexcept {
        return bodyEnded_;
    }

    [[nodiscard]] bool peerEndStream() const noexcept {
        return peerEndStream_;
    }

    [[nodiscard]] const Http2LocalSendState& localSend() const noexcept {
        return localSend_;
    }

    [[nodiscard]] bool queued() const noexcept {
        return queued_;
    }

    [[nodiscard]] bool dispatchStarted() const noexcept {
        return dispatchStarted_;
    }

private:
    friend class Http2StreamState;

    constexpr Http2StreamLifecycle() noexcept = default;

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

    [[nodiscard]] bool abort(Http2StreamCloseSource source) noexcept {
        if (!localSend_.abort(source)) {
            return false;
        }
        peerEndStream_ = true;
        bodyEnded_ = true;
        queued_ = false;
        return true;
    }

    void markPeerEndStream() noexcept {
        peerEndStream_ = true;
    }

    [[nodiscard]] bool beginLocalRequestContent() noexcept {
        return localSend_.beginRequestContent();
    }

    [[nodiscard]] bool beginLocalResponseContent() noexcept {
        return localSend_.beginResponseContent();
    }

    [[nodiscard]] bool beginLocalResponseTrailersOnly() noexcept {
        return localSend_.beginResponseTrailersOnly();
    }

    [[nodiscard]] bool commitLocalHeadEndStream() noexcept {
        return localSend_.commitHeadEndStream();
    }

    [[nodiscard]] bool beginLocalConnectRequest() noexcept {
        return localSend_.beginConnectRequest();
    }

    [[nodiscard]] bool openLocalConnectTunnel() noexcept {
        return localSend_.openTunnel();
    }

    [[nodiscard]] bool rejectLocalConnect() noexcept {
        return localSend_.rejectConnect();
    }

    [[nodiscard]] bool queueLocalEndStream() noexcept {
        return localSend_.queueEndStream();
    }

    [[nodiscard]] bool commitLocalEndStream() noexcept {
        return localSend_.commitEndStream();
    }

    void markBodyEnded() noexcept {
        bodyEnded_ = true;
    }

    [[nodiscard]] bool tryMarkQueued() noexcept {
        if (queued_ || aborted()) {
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
        if (dispatchStarted_ || aborted()) {
            return false;
        }
        dispatchStarted_ = true;
        return true;
    }

private:
    Http2LocalSendState localSend_;
    bool peerEndStream_ : 1 {false};
    bool bodyEnded_ : 1 {false};
    bool queued_ : 1 {false};
    bool dispatchStarted_ : 1 {false};
    bool peerConcurrencySlotHeld_ : 1 {false};
};

}  // namespace ruvia::detail
