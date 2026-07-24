#pragma once

#include "ruvia/http/detail/http2/message/Http2LocalSendState.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveState.h"

namespace ruvia::detail {

class Http2StreamState;

class Http2StreamLifecycle final {
public:
    [[nodiscard]] bool aborted() const noexcept {
        return localSend_.aborted() != nullptr;
    }

    [[nodiscard]] const Http2LocalSendState& localSend() const& noexcept {
        return localSend_;
    }
    [[nodiscard]] const Http2LocalSendState& localSend() const&& = delete;

    [[nodiscard]] const Http2RemoteReceiveState& remoteReceive() const& noexcept {
        return remoteReceive_;
    }
    [[nodiscard]] const Http2RemoteReceiveState& remoteReceive() const&& = delete;

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
        if (remoteReceive_.aborted() != nullptr) {
            return false;
        }
        if (!localSend_.abort(source)) {
            return false;
        }
        if (!remoteReceive_.abort()) {
            return false;
        }
        queued_ = false;
        return true;
    }

    [[nodiscard]] bool recordRemoteHeadEndStream() noexcept {
        return remoteReceive_.recordHeadEndStream();
    }

    [[nodiscard]] bool finalizeRemoteContentHead() noexcept {
        return remoteReceive_.finalizeContentHead();
    }

    [[nodiscard]] bool finalizeRemoteConnectHead() noexcept {
        return remoteReceive_.finalizeConnectHead();
    }

    [[nodiscard]] bool canAcceptRemoteConnect() const noexcept {
        return remoteReceive_.canAcceptConnect();
    }

    [[nodiscard]] bool acceptRemoteConnect() noexcept {
        return remoteReceive_.acceptConnect();
    }

    [[nodiscard]] bool canRejectRemoteConnect() const noexcept {
        return remoteReceive_.canRejectConnect();
    }

    [[nodiscard]] bool rejectRemoteConnect() noexcept {
        return remoteReceive_.rejectConnect();
    }

    [[nodiscard]] bool finishRemoteContent() noexcept {
        return remoteReceive_.finishContent();
    }

    [[nodiscard]] bool finishRemotePendingConnect() noexcept {
        return remoteReceive_.finishPendingConnect();
    }

    [[nodiscard]] bool finishRemoteTunnel() noexcept {
        return remoteReceive_.finishTunnel();
    }

    [[nodiscard]] bool finishRemoteRejectedConnect() noexcept {
        return remoteReceive_.finishRejectedConnect();
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
    Http2RemoteReceiveState remoteReceive_;
    bool queued_ : 1 {false};
    bool dispatchStarted_ : 1 {false};
    bool peerConcurrencySlotHeld_ : 1 {false};
};

}  // namespace ruvia::detail
