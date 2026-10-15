#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_sans_io_stream_signal.h"

namespace ruvia::detail {

inline constexpr std::size_t http2_data_output_credit_bytes = 16 * 1024;
inline constexpr std::size_t http2_data_output_credit_slots = 4;

// Four frame-sized DATA reservations bound producer-owned/core-queued DATA at
// 64 KiB per connection. HTTP/2 control frames have independent protocol output
// storage and are deliberately not included in this DATA-only bound.
class http2_data_output_budget final {
public:
    explicit http2_data_output_budget(const worker_handle& worker_value)
        : changed_(worker_value) {}

    http2_data_output_budget(const http2_data_output_budget&) = delete;
    http2_data_output_budget& operator=(const http2_data_output_budget&) = delete;

    [[nodiscard]] task<bool> acquire(std::uint32_t stream_id, http2_sans_io_stream_signal& stream) {
        while (!stream.terminated()) {
            const bool already_held = std::ranges::find(held_, stream_id) != held_.end();
            if (!already_held) {
                for (auto& held : held_) {
                    if (held == 0) {
                        held = stream_id;
                        co_return true;
                    }
                }
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
                if (submitted_[i] == 0) {
                    clear(i);
                }
                return;
            }
        }
    }

    void note_data_submitted(std::uint32_t stream_id, std::size_t bytes_value) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == stream_id) {
                submitted_[i] += bytes_value;
                return;
            }
        }
    }

    void note_data_output(std::uint32_t stream_id, std::size_t bytes_value) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == stream_id) {
                emitted_[i] += bytes_value;
                batch_output_[i] += bytes_value;
                return;
            }
        }
    }

    // Release a stream's reservation against the current core state immediately.
    // This is required when reset/close discards queued DATA without producing a
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
            if (socket_write_completed) {
                written_[i] += batch_output_[i];
                batch_output_[i] = 0;
            }
            // Stream removal is not evidence that serialized DATA was written.
            // Keep its credit until every submitted payload byte crossed the socket.
            if ((cancelled_[i] && written_[i] >= emitted_[i] &&
                    connection.pending_data_output_bytes(stream_id) == 0) ||
                (!cancelled_[i] && written_[i] >= submitted_[i] &&
                    connection.pending_data_output_bytes(stream_id) == 0 &&
                    connection.data_queue_state(stream_id) != ruvia::http2_data_queue_state::queued)) {
                clear(i);
            }
        }
    }

    [[nodiscard]] task<void> wait_for_change() {
        co_await changed_.wait();
    }

    void wake() noexcept {
        changed_.notify();
    }

private:
    void clear(std::size_t i) noexcept {
        held_[i] = 0;
        submitted_[i] = 0;
        emitted_[i] = 0;
        written_[i] = 0;
        cancelled_[i] = false;
        batch_output_[i] = 0;
        changed_.notify();
    }

    worker_signal changed_;
    std::array<std::uint32_t, http2_data_output_credit_slots> held_{};
    std::array<std::size_t, http2_data_output_credit_slots> submitted_{};
    std::array<std::size_t, http2_data_output_credit_slots> emitted_{};
    std::array<std::size_t, http2_data_output_credit_slots> written_{};
    std::array<bool, http2_data_output_credit_slots> cancelled_{};
    std::array<std::size_t, http2_data_output_credit_slots> batch_output_{};
};

}  // namespace ruvia::detail
