#pragma once

#include <variant>

namespace ruvia::detail {

class Http2RemoteReceiveState;
class Http2StreamLifecycle;

// The initial/final field block has not selected the remote message semantics yet.
// Client-role 1xx responses deliberately return to this same alternative.
class Http2RemoteHeadPending final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteHeadPending() noexcept = default;
};

// END_STREAM arrived on the initial/final HEADERS, but the complete field block must
// still be decoded before content, CONNECT, or tunnel semantics can be selected.
class Http2RemoteHeadEndStreamPending final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteHeadEndStreamPending() noexcept = default;
};

class Http2RemoteContentOpen final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteContentOpen() noexcept = default;
};

// A server decoded CONNECT without END_STREAM and has not accepted or rejected it.
// DATA is forbidden until that decision selects tunnel or rejection semantics.
class Http2RemoteConnectPending final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteConnectPending() noexcept = default;
};

// A server decoded CONNECT and then observed END_STREAM on its HEADERS or on an empty
// DATA frame. The peer send half is closed while the application still owns the
// accept/reject decision.
class Http2RemoteConnectPendingEndStream final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteConnectPendingEndStream() noexcept = default;
};

// The server rejected CONNECT before the peer closed its request half. CONNECT has no
// HTTP request content, so only empty DATA is legal; END_STREAM finishes normally.
class Http2RemoteConnectRejectedAwaitingEndStream final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteConnectRejectedAwaitingEndStream() noexcept = default;
};

class Http2RemoteTunnelOpen final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteTunnelOpen() noexcept = default;
};

// A peer HEADERS or DATA carrying END_STREAM has closed the remote send half.
class Http2RemoteEndStream final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteEndStream() noexcept = default;
};

// Whole-stream abnormal termination is mirrored here so remote frame permission can
// never remain apparently open after local/peer RST_STREAM or GOAWAY exclusion.
class Http2RemoteAborted final {
private:
    friend class Http2RemoteReceiveState;

    constexpr Http2RemoteAborted() noexcept = default;
};

// Remote frame permission and peer-half lifecycle are one exclusive protocol state.
// In particular, HTTP CONNECT content completion is not conflated with END_STREAM:
// an accepted tunnel can keep receiving DATA and replenishing its stream window, while
// a pending/rejected CONNECT accepts empty framing DATA until the peer sends END_STREAM.
class Http2RemoteReceiveState final {
private:
    friend class Http2StreamLifecycle;

    constexpr Http2RemoteReceiveState() noexcept
        : state_(Http2RemoteHeadPending()) {}

    [[nodiscard]] bool recordHeadEndStream() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2RemoteHeadEndStreamPending());
        return true;
    }

    [[nodiscard]] bool finalizeContentHead() noexcept {
        if (headPending() != nullptr) {
            state_ = State(Http2RemoteContentOpen());
            return true;
        }
        if (headEndStreamPending() != nullptr) {
            state_ = State(Http2RemoteEndStream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finalizeConnectHead() noexcept {
        if (headPending() != nullptr) {
            state_ = State(Http2RemoteConnectPending());
            return true;
        }
        if (headEndStreamPending() != nullptr) {
            state_ = State(Http2RemoteConnectPendingEndStream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool canAcceptConnect() const noexcept {
        return headPending() != nullptr || headEndStreamPending() != nullptr || connectPending() != nullptr || connectPendingEndStream() != nullptr;
    }

    [[nodiscard]] bool acceptConnect() noexcept {
        if (headPending() != nullptr || connectPending() != nullptr) {
            state_ = State(Http2RemoteTunnelOpen());
            return true;
        }
        if (headEndStreamPending() != nullptr || connectPendingEndStream() != nullptr) {
            state_ = State(Http2RemoteEndStream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool canRejectConnect() const noexcept {
        return canAcceptConnect();
    }

    [[nodiscard]] bool rejectConnect() noexcept {
        // Client role: a non-2xx CONNECT response resumes ordinary response-content
        // semantics. Server role: a rejected CONNECT request has no content, but its
        // peer send half can remain open until an empty DATA(END_STREAM) arrives.
        if (headPending() != nullptr) {
            state_ = State(Http2RemoteContentOpen());
            return true;
        }
        if (headEndStreamPending() != nullptr || connectPendingEndStream() != nullptr) {
            state_ = State(Http2RemoteEndStream());
            return true;
        }
        if (connectPending() != nullptr) {
            state_ = State(Http2RemoteConnectRejectedAwaitingEndStream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finishContent() noexcept {
        if (contentOpen() == nullptr) {
            return false;
        }
        state_ = State(Http2RemoteEndStream());
        return true;
    }

    [[nodiscard]] bool finishPendingConnect() noexcept {
        if (connectPending() == nullptr) {
            return false;
        }
        state_ = State(Http2RemoteConnectPendingEndStream());
        return true;
    }

    [[nodiscard]] bool finishTunnel() noexcept {
        if (tunnelOpen() == nullptr) {
            return false;
        }
        state_ = State(Http2RemoteEndStream());
        return true;
    }

    [[nodiscard]] bool finishRejectedConnect() noexcept {
        if (connectRejectedAwaitingEndStream() == nullptr) {
            return false;
        }
        state_ = State(Http2RemoteEndStream());
        return true;
    }

    [[nodiscard]] bool abort() noexcept {
        if (aborted() != nullptr) {
            return false;
        }
        state_ = State(Http2RemoteAborted());
        return true;
    }

public:
    [[nodiscard]] constexpr const Http2RemoteHeadPending* headPending() const& noexcept {
        return std::get_if<Http2RemoteHeadPending>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteHeadPending* headPending() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteHeadEndStreamPending* headEndStreamPending() const& noexcept {
        return std::get_if<Http2RemoteHeadEndStreamPending>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteHeadEndStreamPending* headEndStreamPending() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteContentOpen* contentOpen() const& noexcept {
        return std::get_if<Http2RemoteContentOpen>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteContentOpen* contentOpen() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteConnectPending* connectPending() const& noexcept {
        return std::get_if<Http2RemoteConnectPending>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteConnectPending* connectPending() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteConnectPendingEndStream* connectPendingEndStream() const& noexcept {
        return std::get_if<Http2RemoteConnectPendingEndStream>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteConnectPendingEndStream* connectPendingEndStream() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteConnectRejectedAwaitingEndStream* connectRejectedAwaitingEndStream() const& noexcept {
        return std::get_if<Http2RemoteConnectRejectedAwaitingEndStream>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteConnectRejectedAwaitingEndStream* connectRejectedAwaitingEndStream() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteTunnelOpen* tunnelOpen() const& noexcept {
        return std::get_if<Http2RemoteTunnelOpen>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteTunnelOpen* tunnelOpen() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteEndStream* endStream() const& noexcept {
        return std::get_if<Http2RemoteEndStream>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteEndStream* endStream() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteAborted* aborted() const& noexcept {
        return std::get_if<Http2RemoteAborted>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteAborted* aborted() const&& = delete;

private:
    using State = std::variant<Http2RemoteHeadPending, Http2RemoteHeadEndStreamPending, Http2RemoteContentOpen, Http2RemoteConnectPending, Http2RemoteConnectPendingEndStream, Http2RemoteConnectRejectedAwaitingEndStream, Http2RemoteTunnelOpen, Http2RemoteEndStream, Http2RemoteAborted>;

    State state_;
};

}  // namespace ruvia::detail
