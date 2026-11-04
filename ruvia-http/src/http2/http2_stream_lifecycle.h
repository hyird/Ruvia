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

    [[nodiscard]] bool mark_remote_connect_tunnel_data() noexcept {
        return remote_receive_.mark_connect_tunnel_data();
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

    [[nodiscard]] bool commit_local_end_stream() noexcept {
        return local_send_.commit_end_stream();
    }

    http2_local_send_state local_send_;
    http2_remote_receive_state remote_receive_;
    bool peer_concurrency_slot_held_{false};
};

}  // namespace ruvia::detail
