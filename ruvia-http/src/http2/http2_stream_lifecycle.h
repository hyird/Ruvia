#pragma once

#include "http2/http2_local_send_state.h"
#include "http2/http2_remote_receive_state.h"

namespace ruvia::detail {

class http2_stream_state;

class http2_stream_lifecycle final {
public:
    [[nodiscard]] bool aborted() const noexcept {
        return local_send_.aborted() != nullptr;
    }

    [[nodiscard]] const http2_local_send_state& local_send() const& noexcept {
        return local_send_;
    }
    [[nodiscard]] const http2_local_send_state& local_send() const&& = delete;

    [[nodiscard]] const http2_remote_receive_state& remote_receive() const& noexcept {
        return remote_receive_;
    }
    [[nodiscard]] const http2_remote_receive_state& remote_receive() const&& = delete;

    [[nodiscard]] bool queued() const noexcept {
        return queued_;
    }

    [[nodiscard]] bool dispatch_started() const noexcept {
        return dispatch_started_;
    }

private:
    friend class http2_stream_state;

    constexpr http2_stream_lifecycle() noexcept = default;

    [[nodiscard]] bool hold_peer_concurrency_slot() noexcept {
        if (peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = true;
        return true;
    }

    [[nodiscard]] bool release_peer_concurrency_slot() noexcept {
        if (!peer_concurrency_slot_held_) {
            return false;
        }
        peer_concurrency_slot_held_ = false;
        return true;
    }

    [[nodiscard]] bool abort(http2_stream_close_source source_value) noexcept {
        if (remote_receive_.aborted() != nullptr) {
            return false;
        }
        if (!local_send_.abort(source_value)) {
            return false;
        }
        if (!remote_receive_.abort()) {
            return false;
        }
        queued_ = false;
        return true;
    }

    [[nodiscard]] bool record_remote_head_end_stream() noexcept {
        return remote_receive_.record_head_end_stream();
    }

    [[nodiscard]] bool rollback_remote_head_end_stream() noexcept {
        return remote_receive_.rollback_head_end_stream();
    }

    [[nodiscard]] bool finalize_remote_content_head() noexcept {
        return remote_receive_.finalize_content_head();
    }

    [[nodiscard]] bool finalize_remote_connect_head() noexcept {
        return remote_receive_.finalize_connect_head();
    }

    [[nodiscard]] bool can_accept_remote_connect() const noexcept {
        return remote_receive_.can_accept_connect();
    }

    [[nodiscard]] bool accept_remote_connect() noexcept {
        return remote_receive_.accept_connect();
    }

    [[nodiscard]] bool can_reject_remote_connect() const noexcept {
        return remote_receive_.can_reject_connect();
    }

    [[nodiscard]] bool reject_remote_connect() noexcept {
        return remote_receive_.reject_connect();
    }

    [[nodiscard]] bool finish_remote_content() noexcept {
        return remote_receive_.finish_content();
    }

    [[nodiscard]] bool finish_remote_pending_connect() noexcept {
        return remote_receive_.finish_pending_connect();
    }

    [[nodiscard]] bool finish_remote_tunnel() noexcept {
        return remote_receive_.finish_tunnel();
    }

    [[nodiscard]] bool finish_remote_rejected_connect() noexcept {
        return remote_receive_.finish_rejected_connect();
    }

    [[nodiscard]] bool begin_local_request_content() noexcept {
        return local_send_.begin_request_content();
    }

    [[nodiscard]] bool begin_local_response_content() noexcept {
        return local_send_.begin_response_content();
    }

    [[nodiscard]] bool begin_local_response_trailers_only() noexcept {
        return local_send_.begin_response_trailers_only();
    }

    [[nodiscard]] bool commit_local_head_end_stream() noexcept {
        return local_send_.commit_head_end_stream();
    }

    [[nodiscard]] bool begin_local_connect_request() noexcept {
        return local_send_.begin_connect_request();
    }

    [[nodiscard]] bool open_local_connect_tunnel() noexcept {
        return local_send_.open_tunnel();
    }

    [[nodiscard]] bool reject_local_connect() noexcept {
        return local_send_.reject_connect();
    }

    [[nodiscard]] bool queue_local_end_stream() noexcept {
        return local_send_.queue_end_stream();
    }

    [[nodiscard]] bool commit_local_end_stream() noexcept {
        return local_send_.commit_end_stream();
    }

    [[nodiscard]] bool try_mark_queued() noexcept {
        if (queued_ || aborted()) {
            return false;
        }
        queued_ = true;
        return true;
    }

    void clear_queued() noexcept {
        queued_ = false;
    }

    [[nodiscard]] bool try_start_dispatch() noexcept {
        queued_ = false;
        if (dispatch_started_ || aborted()) {
            return false;
        }
        dispatch_started_ = true;
        return true;
    }

    http2_local_send_state local_send_;
    http2_remote_receive_state remote_receive_;
    bool queued_ : 1 {false};
    bool dispatch_started_ : 1 {false};
    bool peer_concurrency_slot_held_ : 1 {false};
};

}  // namespace ruvia::detail
