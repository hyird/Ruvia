#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class Http2StreamCloseSource : std::uint8_t {
    kNone,
    kLocal,
    kPeer
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
        return localEndStream_;
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

    void markReset(Http2StreamCloseSource source = Http2StreamCloseSource::kLocal) noexcept {
        reset_ = true;
        if (source != Http2StreamCloseSource::kNone) {
            closeSource_ = source;
        }
    }

    void markClosed(Http2StreamCloseSource source) noexcept {
        reset_ = true;
        peerEndStream_ = true;
        localEndStream_ = true;
        bodyEnded_ = true;
        closeSource_ = source;
    }

    void markPeerEndStream() noexcept {
        peerEndStream_ = true;
    }

    void markLocalEndStream() noexcept {
        localEndStream_ = true;
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
    bool localEndStream_ : 1 {false};
    bool bodyEnded_ : 1 {false};
    bool queued_ : 1 {false};
    bool dispatchStarted_ : 1 {false};
    bool reset_ : 1 {false};
    Http2StreamCloseSource closeSource_{Http2StreamCloseSource::kNone};
};

}  // namespace ruvia::detail
