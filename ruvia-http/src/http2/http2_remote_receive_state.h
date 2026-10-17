#pragma once

#include <variant>

namespace ruvia::detail {

class http2_remote_receive_state;
class http2_stream_lifecycle;

// The initial/final field block has not selected the remote message semantics yet.
// Client-role 1xx responses deliberately return to this same alternative.
class http2_remote_head_pending final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_head_pending() noexcept = default;
};

// END_STREAM arrived on the initial/final HEADERS, but the complete field block must
// still be decoded before content, CONNECT, or tunnel semantics can be selected.
class http2_remote_head_end_stream_pending final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_head_end_stream_pending() noexcept = default;
};

class http2_remote_content_open final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_content_open() noexcept = default;
};

// A server decoded CONNECT without END_STREAM and has not accepted or rejected it.
// DATA is forbidden until that decision selects tunnel or rejection semantics.
class http2_remote_connect_pending final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_connect_pending() noexcept = default;
};

// A server decoded CONNECT and then observed END_STREAM on its HEADERS or on an empty
// DATA frame. The peer send half is closed while the application still owns the
// accept/reject decision.
class http2_remote_connect_pending_end_stream final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_connect_pending_end_stream() noexcept = default;
};

// The server rejected CONNECT before the peer closed its request half. CONNECT has no
// HTTP request content, so only empty DATA is legal; END_STREAM finishes normally.
class http2_remote_connect_rejected_awaiting_end_stream final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_connect_rejected_awaiting_end_stream() noexcept = default;
};

class http2_remote_tunnel_open final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_tunnel_open() noexcept = default;
};

// A peer HEADERS or DATA carrying END_STREAM has closed the remote send half.
class http2_remote_end_stream final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_end_stream() noexcept = default;
};

// Whole-stream abnormal termination is mirrored here so remote frame permission can
// never remain apparently open after local/peer RST_STREAM or GOAWAY exclusion.
class http2_remote_aborted final {
private:
    friend class http2_remote_receive_state;

    constexpr http2_remote_aborted() noexcept = default;
};

// Remote frame permission and peer-half lifecycle are one exclusive protocol state.
// In particular, HTTP CONNECT content completion is not conflated with END_STREAM:
// an accepted tunnel can keep receiving DATA and replenishing its stream window, while
// a pending/rejected CONNECT accepts empty framing DATA until the peer sends END_STREAM.
class http2_remote_receive_state final {
private:
    friend class http2_stream_lifecycle;

    constexpr http2_remote_receive_state() noexcept
        : state_(http2_remote_head_pending()) {}

    [[nodiscard]] bool record_head_end_stream() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_head_end_stream_pending());
        return true;
    }

    [[nodiscard]] bool rollback_head_end_stream() noexcept {
        if (head_end_stream_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_head_pending());
        return true;
    }

    [[nodiscard]] bool finalize_content_head() noexcept {
        if (head_pending() != nullptr) {
            state_ = state_type(http2_remote_content_open());
            return true;
        }
        if (head_end_stream_pending() != nullptr) {
            state_ = state_type(http2_remote_end_stream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finalize_connect_head() noexcept {
        if (head_pending() != nullptr) {
            state_ = state_type(http2_remote_connect_pending());
            return true;
        }
        if (head_end_stream_pending() != nullptr) {
            state_ = state_type(http2_remote_connect_pending_end_stream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_accept_connect() const noexcept {
        return head_pending() != nullptr || head_end_stream_pending() != nullptr ||
               connect_pending() != nullptr || connect_pending_end_stream() != nullptr;
    }

    [[nodiscard]] bool accept_connect() noexcept {
        if (head_pending() != nullptr || connect_pending() != nullptr) {
            state_ = state_type(http2_remote_tunnel_open());
            return true;
        }
        if (head_end_stream_pending() != nullptr || connect_pending_end_stream() != nullptr) {
            state_ = state_type(http2_remote_end_stream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_reject_connect() const noexcept {
        return can_accept_connect();
    }

    [[nodiscard]] bool reject_connect() noexcept {
        // Client role: a non-2xx CONNECT response resumes ordinary response-content
        // semantics. Server role: a rejected CONNECT request has no content, but its
        // peer send half can remain open until an empty DATA(END_STREAM) arrives.
        if (head_pending() != nullptr) {
            state_ = state_type(http2_remote_content_open());
            return true;
        }
        if (head_end_stream_pending() != nullptr || connect_pending_end_stream() != nullptr) {
            state_ = state_type(http2_remote_end_stream());
            return true;
        }
        if (connect_pending() != nullptr) {
            state_ = state_type(http2_remote_connect_rejected_awaiting_end_stream());
            return true;
        }
        return false;
    }

    [[nodiscard]] bool finish_content() noexcept {
        if (content_open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_end_stream());
        return true;
    }

    [[nodiscard]] bool finish_pending_connect() noexcept {
        if (connect_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_connect_pending_end_stream());
        return true;
    }

    [[nodiscard]] bool finish_tunnel() noexcept {
        if (tunnel_open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_end_stream());
        return true;
    }

    [[nodiscard]] bool finish_rejected_connect() noexcept {
        if (connect_rejected_awaiting_end_stream() == nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_end_stream());
        return true;
    }

    [[nodiscard]] bool abort() noexcept {
        if (aborted() != nullptr) {
            return false;
        }
        state_ = state_type(http2_remote_aborted());
        return true;
    }

public:
    [[nodiscard]] constexpr const http2_remote_head_pending* head_pending() const& noexcept {
        return std::get_if<http2_remote_head_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_head_pending* head_pending() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_head_end_stream_pending* head_end_stream_pending()
        const& noexcept {
        return std::get_if<http2_remote_head_end_stream_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_head_end_stream_pending* head_end_stream_pending() const&& =
        delete;

    [[nodiscard]] constexpr const http2_remote_content_open* content_open() const& noexcept {
        return std::get_if<http2_remote_content_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_content_open* content_open() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_connect_pending* connect_pending() const& noexcept {
        return std::get_if<http2_remote_connect_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_connect_pending* connect_pending() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_connect_pending_end_stream* connect_pending_end_stream()
        const& noexcept {
        return std::get_if<http2_remote_connect_pending_end_stream>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_connect_pending_end_stream* connect_pending_end_stream()
        const&& = delete;

    [[nodiscard]] constexpr const http2_remote_connect_rejected_awaiting_end_stream*
    connect_rejected_awaiting_end_stream() const& noexcept {
        return std::get_if<http2_remote_connect_rejected_awaiting_end_stream>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_connect_rejected_awaiting_end_stream*
    connect_rejected_awaiting_end_stream() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_tunnel_open* tunnel_open() const& noexcept {
        return std::get_if<http2_remote_tunnel_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_tunnel_open* tunnel_open() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_end_stream* end_stream() const& noexcept {
        return std::get_if<http2_remote_end_stream>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_end_stream* end_stream() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_aborted* aborted() const& noexcept {
        return std::get_if<http2_remote_aborted>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_aborted* aborted() const&& = delete;

private:
    using state_type = std::variant<http2_remote_head_pending, http2_remote_head_end_stream_pending,
        http2_remote_content_open, http2_remote_connect_pending, http2_remote_connect_pending_end_stream,
        http2_remote_connect_rejected_awaiting_end_stream, http2_remote_tunnel_open, http2_remote_end_stream,
        http2_remote_aborted>;

    state_type state_;
};

}  // namespace ruvia::detail
