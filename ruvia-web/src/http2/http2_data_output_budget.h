#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "server/multiplexed_stream_signal.h"

namespace ruvia::detail {

inline constexpr std::size_t http2_data_output_credit_bytes = 16 * 1024;
inline constexpr std::size_t http2_data_output_credit_slots = 4;

// Four frame-sized DATA reservations bound encoded DATA at 64 KiB per
// connection through socket completion. HTTP/2 control frames have independent
// protocol output storage and are deliberately not included in this bound.
class http2_data_output_budget final {
public:
    explicit http2_data_output_budget(const worker_ref& worker_value)
        : changed_(worker_value) {}
    http2_data_output_budget(worker_ref&&) = delete;
    http2_data_output_budget(const http2_data_output_budget&) = delete;
    http2_data_output_budget& operator=(const http2_data_output_budget&) = delete;

    [[nodiscard]] bool try_acquire(std::uint32_t stream_id) noexcept {
        if (std::ranges::find(held_, stream_id) != held_.end()) {
            return false;
        }
        for (auto& held : held_) {
            if (held == 0) {
                held = stream_id;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] task<bool> acquire(std::uint32_t stream_id, multiplexed_stream_signal& stream,
        const ruvia::http2_connection& connection, const bool* aborted) {
        while (!stream.terminated() && !connection.stream_aborted(stream_id) &&
               (aborted == nullptr || !*aborted)) {
            if (try_acquire(stream_id)) {
                co_return true;
            }
            // A stream may own only one DATA reservation; wait for the current
            // chunk to drain rather than admitting another and confusing release.
            co_await changed_.wait();
        }
        co_return false;
    }

    void release(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == stream_id) {
                cancelled_[i] = true;
                if (progress_[i].submitted_ == 0) {
                    clear(i);
                }
                return;
            }
        }
    }

    void note_data_submitted(std::uint32_t stream_id, std::size_t bytes_value) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == stream_id) {
                progress_[i].submitted_ += bytes_value;
                return;
            }
        }
    }

    void note_data_output(std::uint32_t stream_id, std::size_t bytes_value) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == stream_id) {
                progress_[i].emitted_ += bytes_value;
                progress_[i].batch_output_ += bytes_value;
                return;
            }
        }
    }

    // Release a stream's reservation against the current core state immediately.
    // This is required when reset/close discards encoded DATA without producing a
    // socket completion callback. Already serialized bytes remain charged until
    // their containing socket batch completes.
    void release_and_reconcile(std::uint32_t stream_id,
        const ruvia::http2_connection& connection) noexcept {
        release(stream_id);
        reconcile(connection, false);
    }

    void reconcile(const ruvia::http2_connection& connection, bool socket_write_completed) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            const auto stream_id = held_[i];
            if (stream_id == 0) {
                continue;
            }
            auto& progress = progress_[i];
            if (socket_write_completed) {
                progress.written_ += progress.batch_output_;
                progress.batch_output_ = 0;
            }
            // Stream removal is not evidence that serialized DATA was written.
            // Keep its credit until every submitted payload byte crossed the socket.
            if ((cancelled_[i] && progress.written_ >= progress.emitted_ &&
                    connection.pending_data_output_bytes(stream_id) == 0) ||
                (!cancelled_[i] && progress.written_ >= progress.submitted_ &&
                    connection.pending_data_output_bytes(stream_id) == 0)) {
                clear(i);
            }
        }
    }

    void wake() noexcept {
        changed_.notify();
    }

private:
    struct credit_progress final {
        std::size_t submitted_{};
        std::size_t emitted_{};
        std::size_t written_{};
        std::size_t batch_output_{};
    };

    void clear(std::size_t i) noexcept {
        held_[i] = 0;
        progress_[i] = {};
        cancelled_[i] = false;
        changed_.notify();
    }

    worker_signal changed_;
    std::array<std::uint32_t, http2_data_output_credit_slots> held_{};
    std::array<credit_progress, http2_data_output_credit_slots> progress_{};
    std::array<bool, http2_data_output_credit_slots> cancelled_{};
};

}  // namespace ruvia::detail
