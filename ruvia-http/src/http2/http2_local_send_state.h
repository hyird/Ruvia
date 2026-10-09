#pragma once

#include <variant>

#include "http2/http2_stream_close_source.h"

namespace ruvia::detail {

class http2_local_send_state;
class http2_stream_lifecycle;

class http2_local_head_pending final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_head_pending() noexcept = default;
};

class http2_local_request_content_open final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_request_content_open() noexcept = default;
};

class http2_local_response_content_open final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_response_content_open() noexcept = default;
};

class http2_local_response_trailers_only final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_response_trailers_only() noexcept = default;
};

class http2_local_connect_pending final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_connect_pending() noexcept = default;
};

class http2_local_tunnel_open final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_tunnel_open() noexcept = default;
};

// END_STREAM is accepted by the core but still sits behind flow-control-blocked
// DATA or a deferred trailer section. No further semantic submission is legal.
class http2_local_end_stream_queued final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_end_stream_queued() noexcept = default;
};

// The terminal HEADERS or DATA carrying END_STREAM has been materialized in the
// core-owned outbound buffer. This endpoint is now half-closed(local).
class http2_local_end_stream_committed final {
private:
    friend class http2_local_send_state;

    constexpr http2_local_end_stream_committed() noexcept = default;
};

// Only abnormal whole-stream termination owns a close source. This covers a local
// or peer RST_STREAM as well as a GOAWAY last-stream-id exclusion; GOAWAY is not a
// reset, so naming this alternative after RST_STREAM would be protocol-inaccurate.
// Open and normally ended states cannot expose a close source.
class http2_stream_aborted final {
public:
    [[nodiscard]] constexpr http2_stream_close_source source() const noexcept {
        return source_;
    }

private:
    friend class http2_local_send_state;

    explicit constexpr http2_stream_aborted(http2_stream_close_source source_value) noexcept
        : source_(source_value) {}

    http2_stream_close_source source_;
};

// Local frame permission is one exclusive protocol state. Request content,
// response content, response-trailers-only, and tunnel DATA are deliberately
// distinct, so a separate message-kind enum cannot contradict the active phase.
class http2_local_send_state final {
private:
    friend class http2_stream_lifecycle;

    constexpr http2_local_send_state() noexcept
        : state_(http2_local_head_pending()) {}

    [[nodiscard]] bool begin_request_content() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_request_content_open());
        return true;
    }

    [[nodiscard]] bool begin_response_content() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_response_content_open());
        return true;
    }

    [[nodiscard]] bool begin_response_trailers_only() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_response_trailers_only());
        return true;
    }

    [[nodiscard]] bool begin_connect_request() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_connect_pending());
        return true;
    }

    // A client opens from connect-pending after a peer 2xx; a server opens from
    // head-pending when it submits that 2xx. The owning stream validates the
    // separate CONNECT semantic state before calling this transition.
    [[nodiscard]] bool open_tunnel() noexcept {
        if (head_pending() == nullptr && connect_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_tunnel_open());
        return true;
    }

    [[nodiscard]] bool commit_head_end_stream() noexcept {
        if (head_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_end_stream_committed());
        return true;
    }

    [[nodiscard]] bool reject_connect() noexcept {
        if (connect_pending() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_end_stream_committed());
        return true;
    }

    [[nodiscard]] bool queue_end_stream() noexcept {
        if (!content_or_trailers_open()) {
            return false;
        }
        state_ = state_type(http2_local_end_stream_queued());
        return true;
    }

    [[nodiscard]] bool commit_end_stream() noexcept {
        if (!content_or_trailers_open() && end_stream_queued() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_end_stream_committed());
        return true;
    }

    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        if (!http2_is_valid_stream_close_source(source_value) || aborted() != nullptr) {
            return false;
        }
        state_ = state_type(http2_stream_aborted(source_value));
        return true;
    }

public:
    [[nodiscard]] constexpr const http2_local_head_pending* head_pending() const& noexcept {
        return std::get_if<http2_local_head_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_head_pending* head_pending() const&& = delete;

    [[nodiscard]] constexpr const http2_local_request_content_open* request_content_open()
        const& noexcept {
        return std::get_if<http2_local_request_content_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_request_content_open* request_content_open() const&& =
        delete;

    [[nodiscard]] constexpr const http2_local_response_content_open* response_content_open()
        const& noexcept {
        return std::get_if<http2_local_response_content_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_response_content_open* response_content_open() const&& =
        delete;

    [[nodiscard]] constexpr const http2_local_response_trailers_only* response_trailers_only()
        const& noexcept {
        return std::get_if<http2_local_response_trailers_only>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_response_trailers_only* response_trailers_only() const&& =
        delete;

    [[nodiscard]] constexpr const http2_local_connect_pending* connect_pending() const& noexcept {
        return std::get_if<http2_local_connect_pending>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_connect_pending* connect_pending() const&& = delete;

    [[nodiscard]] constexpr const http2_local_tunnel_open* tunnel_open() const& noexcept {
        return std::get_if<http2_local_tunnel_open>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_tunnel_open* tunnel_open() const&& = delete;

    [[nodiscard]] constexpr const http2_local_end_stream_queued* end_stream_queued() const& noexcept {
        return std::get_if<http2_local_end_stream_queued>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_end_stream_queued* end_stream_queued() const&& = delete;

    [[nodiscard]] constexpr const http2_local_end_stream_committed* end_stream_committed()
        const& noexcept {
        return std::get_if<http2_local_end_stream_committed>(&state_);
    }
    [[nodiscard]] constexpr const http2_local_end_stream_committed* end_stream_committed() const&& =
        delete;

    [[nodiscard]] constexpr const http2_stream_aborted* aborted() const& noexcept {
        return std::get_if<http2_stream_aborted>(&state_);
    }
    [[nodiscard]] constexpr const http2_stream_aborted* aborted() const&& = delete;

private:
    [[nodiscard]] bool content_or_trailers_open() const noexcept {
        return request_content_open() != nullptr || response_content_open() != nullptr ||
               response_trailers_only() != nullptr || tunnel_open() != nullptr;
    }

    using state_type = std::variant<http2_local_head_pending, http2_local_request_content_open,
        http2_local_response_content_open, http2_local_response_trailers_only, http2_local_connect_pending,
        http2_local_tunnel_open, http2_local_end_stream_queued, http2_local_end_stream_committed,
        http2_stream_aborted>;

    state_type state_;
};

}  // namespace ruvia::detail
