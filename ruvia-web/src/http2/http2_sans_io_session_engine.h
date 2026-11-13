#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_handlers.h"

#include "http2/http2_sans_io_session.h"
#include "router/route_resolution.h"
#include "server/http_request_admission.h"
#include "server/inbound_buffer_budget.h"
#include "server/multiplexed_stream_signal.h"
#include "server/response_file_source.h"

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

namespace ruvia::detail {

enum class http2_body_queue_store_status : std::uint8_t {
    stored,
    backlog_overflow,
    buffer_limit,
};

// Web-owned storage for body/tunnel bytes that have already crossed the
// http2_connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
// Pending DATA is concatenated so each frame is copied once, not into a new
// string per frame. pop() swaps the pending buffer aside so enqueue cannot
// invalidate the borrowed view.
class http2_sans_io_body_queue final {
public:
    explicit http2_sans_io_body_queue(inbound_buffer_account* inbound = nullptr) noexcept
        : queued_reservation_(inbound),
          active_reservation_(inbound) {}

    void enqueue(std::string_view data, http2_received_data_credit&& credit) {
        if (data.empty()) {
            return;
        }
        // Transfer credit only after append succeeds; allocation failure leaves
        // the incoming token with its owner for unwinding.
        inbound_append(queued_chunk_, queued_reservation_, data);
        if (!credit.valid()) {
            return;
        }
        if (!queued_credit_.has_value()) {
            queued_credit_.emplace(std::move(credit));
            return;
        }
        queued_credit_->merge(std::move(credit));
    }

    [[nodiscard]] http2_body_queue_store_status enqueue_bounded(std::string_view data,
        http2_received_data_credit&& credit, std::size_t backlog_limit) {
        const auto queued_bytes = queued_chunk_.size();
        if (queued_bytes > backlog_limit || data.size() > backlog_limit - queued_bytes) {
            return http2_body_queue_store_status::backlog_overflow;
        }
        try {
            enqueue(data, std::move(credit));
        } catch (const inbound_buffer_limit_error&) {
            return http2_body_queue_store_status::buffer_limit;
        }
        return http2_body_queue_store_status::stored;
    }

    // Release credits only after the consumer has copied/finished the active view.
    void release_active_credits() noexcept {
        active_credit_.reset();
    }

    [[nodiscard]] bool empty() const noexcept {
        return queued_chunk_.empty();
    }

    // The message-end event for this body has been delivered. The core's own
    // receive state can run ahead of event delivery within one input batch, so
    // readers end on this flag rather than on the stream's receive status.
    void mark_ended() noexcept {
        ended_ = true;
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() & {
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        release_active_credits();
        if (queued_chunk_.empty()) {
            return {};
        }
        active_chunk_.swap(queued_chunk_);
        active_reservation_.swap(queued_reservation_);
        if (queued_credit_.has_value()) {
            active_credit_.emplace(std::move(*queued_credit_));
            queued_credit_.reset();
        }
        inbound_clear_retaining_small(queued_chunk_, queued_reservation_);
        return std::string_view(active_chunk_);
    }
    std::string_view pop() && = delete;

    void discard() noexcept {
        inbound_discard(queued_chunk_, queued_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        queued_credit_.reset();
        active_credit_.reset();
    }

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<http2_received_data_credit> queued_credit_;
    std::optional<http2_received_data_credit> active_credit_;
    std::string queued_chunk_;
    inbound_buffer_reservation queued_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    bool ended_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http2_request_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

class http2_sans_io_stream_runtime final {
public:
    // The owning table resolves metadata and body resources before construction.
    explicit http2_sans_io_stream_runtime(std::uint32_t stream_id, inbound_buffer_account* inbound = nullptr) noexcept
        : stream_id_(stream_id),
          buffered_reservation_(inbound),
          body_queue_(inbound) {}

    void bind_push_parent(std::uint32_t id) noexcept {
        push_parent_ = id;
    }
    [[nodiscard]] std::uint32_t push_parent() const noexcept {
        return push_parent_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority_update() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_update_;
    }
    const std::optional<http_priority>& priority_update() const&& = delete;
    void reprioritize(http_priority priority) noexcept {
        priority_update_ = priority;
    }
    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;
    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

    [[nodiscard]] bool hold_request_head(http2_request_head_event&& request_head) noexcept {
        if (request_head_ || request_head.stream_id() != stream_id_) {
            return false;
        }
        request_head_.emplace(std::move(request_head));
        return true;
    }

    [[nodiscard]] http2_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND {
        return request_head_ ? &*request_head_ : nullptr;
    }
    http2_request_head_event* request_head() && = delete;

    [[nodiscard]] bool select_route(route_resolution resolution, request_body_mode body_mode) noexcept {
        if (route_selected_) {
            return false;
        }
        resolution_ = std::move(resolution);
        body_mode_ = body_mode;
        route_selected_ = true;
        return true;
    }

    [[nodiscard]] bool route_selected() const noexcept {
        return route_selected_;
    }

    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;

    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }

    [[nodiscard]] http2_request_body_store_status store_body(
        http2_message_body_chunk_event& chunk, protocol_byte_limit total_limit, std::size_t backlog_limit) {
        const auto data = chunk.bytes();
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http2_request_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream) {
            switch (body_queue_.enqueue_bounded(data, chunk.take_credit(), backlog_limit)) {
                case http2_body_queue_store_status::stored:
                    break;
                case http2_body_queue_store_status::backlog_overflow:
                    return http2_request_body_store_status::backlog_overflow;
                case http2_body_queue_store_status::buffer_limit:
                    return http2_request_body_store_status::buffer_limit;
            }
        } else {
            try {
                if (!data.empty()) {
                    inbound_append(buffered_body_, buffered_reservation_, data);
                }
            } catch (const inbound_buffer_limit_error&) {
                return http2_request_body_store_status::buffer_limit;
            }
        }
        received_bytes_ += data.size();
        return http2_request_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return buffered_body_;
    }
    std::string_view buffered_body() const&& = delete;

    [[nodiscard]] http2_sans_io_body_queue& body_queue() & noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    http2_sans_io_body_queue& body_queue() && = delete;

    [[nodiscard]] const http2_sans_io_body_queue& body_queue() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    const http2_sans_io_body_queue& body_queue() const&& = delete;

    void discard_body() noexcept {
        inbound_discard(buffered_body_, buffered_reservation_);
        body_queue_.discard();
    }

    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }

    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;

    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    // Request-body DATA arrived at `now_ms` (the time of the read carrying it).
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }

    // The request body is still open and has gone `inactivity` without DATA,
    // or `completion` since it opened (the stream's request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

private:
    friend class http2_sans_io_stream_runtime_table;

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (!route_selected_ || signal_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        return &*signal_;
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }

    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }

    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }

    std::uint32_t stream_id_;
    std::uint32_t push_parent_{};
    http_request_trailers trailers_;
    std::optional<http_priority> priority_update_{};
    std::optional<http2_request_head_event> request_head_;
    route_resolution resolution_{};
    request_body_mode body_mode_{request_body_mode::buffered};
    std::string buffered_body_;
    inbound_buffer_reservation buffered_reservation_;
    http2_sans_io_body_queue body_queue_;
    std::size_t received_bytes_{0};
    std::optional<multiplexed_stream_signal> signal_;
    std::optional<http_request_body_failure> body_failure_{};
    multiplexed_request_body_deadline request_body_deadline_;
    bool route_selected_{false};
    bool tunnel_{false};
    bool response_committed_{false};
};

// Stable per-stream Web runtime storage. The common multiplexing case uses inline
// slots; overflow objects are PMR-owned and remain stable when the pointer vector
// compacts, so request body views and dispatch signal references cannot be invalidated
// by another stream being admitted or erased.
class http2_sans_io_stream_runtime_table final {
public:
    explicit http2_sans_io_stream_runtime_table(
        multiplexed_session_termination& termination, inbound_buffer_account* body_account = nullptr) noexcept
        : termination_(termination),
          body_account_(body_account) {}

    [[nodiscard]] http2_sans_io_stream_runtime* find(std::uint32_t stream_id) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                return &*slot;
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->stream_id() == stream_id) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    // The dispatch signal of a stream that has one. A stream with no runtime,
    // or one admitted but not yet dispatched, has nothing to wake.
    [[nodiscard]] multiplexed_stream_signal* signal_for(std::uint32_t stream_id) noexcept {
        auto* runtime = find(stream_id);
        return runtime != nullptr ? runtime->signal() : nullptr;
    }

    [[nodiscard]] const http2_sans_io_stream_runtime* find(std::uint32_t stream_id) const noexcept {
        for (const auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                return &*slot;
            }
        }
        for (const auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->stream_id() == stream_id) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    // Protocol admission is already committed before Web attaches application
    // runtime state. The HTTP stream identifier is sufficient here; this table
    // does not retain or inspect protocol stream storage.
    [[nodiscard]] http2_sans_io_stream_runtime& ensure_accepted(std::uint32_t stream_id) {
        if (auto* existing = find(stream_id)) {
            return *existing;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id, body_account_);
                ++size_;
                return *slot;
            }
        }
        auto runtime = std::make_unique<http2_sans_io_stream_runtime>(stream_id, body_account_);
        auto* result_value = runtime.get();
        overflow_.push_back(std::move(runtime));
        ++size_;
        return *result_value;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        std::uint32_t stream_id, const worker_ref& worker_value) {
        auto* runtime = find(stream_id);
        if (runtime == nullptr) {
            return nullptr;
        }
        return runtime->begin_dispatch(worker_value, termination_);
    }
    multiplexed_stream_signal* begin_dispatch(std::uint32_t, worker_ref&&) = delete;

    [[nodiscard]] bool mark_tunnel(std::uint32_t stream_id) noexcept {
        auto* runtime = find(stream_id);
        if (runtime == nullptr || runtime->tunnel()) {
            return false;
        }
        runtime->mark_tunnel();
        ++tunnel_count_;
        return true;
    }

    // `now_ms` is the time of the read that carried the request head.
    void mark_request_body_open(std::uint32_t stream_id, std::int64_t now_ms) noexcept {
        if (auto* runtime = find(stream_id)) {
            runtime->mark_request_body_open(now_ms);
        }
    }

    void mark_request_body_closed(std::uint32_t stream_id) noexcept {
        if (auto* runtime = find(stream_id)) {
            runtime->mark_request_body_closed();
        }
    }

    [[nodiscard]] bool remove(std::uint32_t stream_id) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->stream_id() == stream_id) {
                account_removal(*slot);
                slot.reset();
                --size_;
                return true;
            }
        }
        for (std::size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] == nullptr || overflow_[i]->stream_id() != stream_id) {
                continue;
            }
            account_removal(*overflow_[i]);
            if (i + 1 != overflow_.size()) {
                overflow_[i] = std::move(overflow_.back());
            }
            overflow_.pop_back();
            --size_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t tunnel_count() const noexcept {
        return tunnel_count_;
    }

    template <typename callback>
    void for_each(callback&& callback_value) {
        for (auto& slot : inline_) {
            if (slot) {
                callback_value(*slot);
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr) {
                callback_value(*runtime);
            }
        }
    }

private:
    // Mirrors http2_stream_table::inline_capacity: two inline slots for the
    // typical cadence, pmr overflow for deeper multiplexing, so the table's
    // resident footprint stays small in every connection.
    static constexpr std::size_t inline_capacity = 2;
    using overflow_runtime_type =
        std::unique_ptr<http2_sans_io_stream_runtime>;

    void account_removal(const http2_sans_io_stream_runtime& runtime) noexcept {
        if (runtime.tunnel()) {
            --tunnel_count_;
        }
    }

    multiplexed_session_termination& termination_;
    inbound_buffer_account* body_account_{nullptr};
    std::array<std::optional<http2_sans_io_stream_runtime>, inline_capacity> inline_{};
    std::vector<overflow_runtime_type> overflow_;
    std::size_t size_{0};
    std::size_t tunnel_count_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http2_send_window_wait_status : std::uint8_t {
    ready,
    aborted,
};

// Wait for payload window availability. Input processing wakes the Web-owned
// signal even when SETTINGS/WINDOW_UPDATE produces no application event.
// Stream existence, abort and session end are re-checked after every wake.
// The session resets a stream whose wait outlasts write_timeout_ (Pingora
// bounds the h2 capacity wait by its write timeout), which ends it as aborted.
[[nodiscard]] task<http2_send_window_wait_status> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    multiplexed_stream_signal* signal, const bool* aborted = nullptr);

enum class http2_stream_data_write_status : std::uint8_t {
    completed,
    local_aborted,
    session_terminated,
    peer_closed,
    invalid_state,
    content_length_exceeded,
};

struct http2_stream_data_submit_result final {
    http2_data_submit_result progress_{};
    // Only invalid_state or content_length_exceeded; no bytes were consumed.
    std::optional<http2_stream_data_write_status> failure_{};
};

// One synchronous DATA submission. The caller already owns output credit for
// nonempty bytes and keeps the payload alive. Records consumed credit, releases
// unused credit on every exit, and wakes the socket writer even on exceptions.
[[nodiscard]] http2_stream_data_submit_result submit_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    worker_signal& write_signal, http2_data_output_budget& output_budget,
    std::string_view bytes, http2_end_stream end_stream);

// Submits caller-owned input with a credit-sized payload allowance, retaining
// each unconsumed suffix until window and output credit are available.
// An empty END_STREAM takes no output credit but, like any DATA frame, waits
// for both send windows to be positive. The caller keeps bytes alive.
[[nodiscard]] task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    multiplexed_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted = nullptr);

}  // namespace ruvia::detail

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {
enum class http2_stream_data_write_status : std::uint8_t;
class http2_sans_io_session_engine;
class http2_sans_io_stream_runtime_table;

// Streams of one connection that may open or read a file body on the blocking
// pool at once. The rest wait holding no read buffer and no pool task. Equal
// to the DATA output credit slots: more concurrent reads could not be
// submitted any sooner.
inline constexpr std::size_t http2_file_read_slots = http2_data_output_credit_slots;

// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, the file body (through response_file_source), and
// the typed terminal result chain. Keeping it non-template compiles that policy
// once for plain and TLS sessions. A stream it cannot complete is reset through
// the owning session's single reset entry point.
//
// A file body is validated before HEADERS without being read. Each later
// chunk is read only after the peer granted send window, at most that much,
// while holding one of http2_file_read_slots through its DATA submission.
class http2_buffered_response_writer final {
public:
    http2_buffered_response_writer(http2_sans_io_session_engine& session, ruvia::http2_connection& connection,
        http2_sans_io_stream_runtime_table& stream_runtimes, worker_signal& write_signal,
        http2_data_output_budget& output_budget, response_file_io file_io) noexcept;

    http2_buffered_response_writer(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer& operator=(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer(http2_buffered_response_writer&&) = delete;
    http2_buffered_response_writer& operator=(http2_buffered_response_writer&&) = delete;

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<http2_stream_data_write_status> write_data(
        std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream);
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(
        std::uint32_t stream_id, response_file_source& source, http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::uint32_t stream_id) const noexcept;
    void wake_writer() noexcept;

    http2_sans_io_session_engine& session_;
    ruvia::http2_connection& connection_;
    http2_sans_io_stream_runtime_table& stream_runtimes_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class route_table;

// Same-executor session state. Writer submission is recorded before co_spawn
// can invoke completion inline; completion remains true before reader stopping.
class http2_sans_io_session_lifecycle final {
public:
    [[nodiscard]] bool write_failed() const noexcept {
        return write_failed_;
    }

    [[nodiscard]] bool stopping() const noexcept {
        return stopping_;
    }

    [[nodiscard]] bool writer_join_pending() const noexcept {
        return writer_ == writer_state::submitted;
    }

    void mark_writer_submitted() noexcept {
        if (writer_ != writer_state::not_submitted) {
            std::terminate();
        }
        writer_ = writer_state::submitted;
    }

    void mark_writer_launch_failed() noexcept {
        if (writer_ == writer_state::submitted) {
            writer_ = writer_state::not_submitted;
        }
    }

    void mark_write_failed() noexcept {
        write_failed_ = true;
    }

    void begin_stopping() noexcept {
        stopping_ = true;
    }

    void mark_writer_done() noexcept {
        if (writer_ != writer_state::submitted) {
            std::terminate();
        }
        writer_ = writer_state::completed;
    }

    void record_writer_failure(std::exception_ptr failure) noexcept {
        if (writer_failure_ == nullptr) {
            writer_failure_ = std::move(failure);
        }
    }

    void rethrow_writer_failure() const {
        if (writer_failure_ != nullptr) {
            std::rethrow_exception(writer_failure_);
        }
    }

private:
    enum class writer_state : std::uint8_t { not_submitted,
        submitted,
        completed };
    writer_state writer_{writer_state::not_submitted};
    bool write_failed_{false};
    bool stopping_{false};
    std::exception_ptr writer_failure_;
};
// Transport-independent application driver for one HTTP/2 connection. The
// transport wrapper owns only socket/TLS reads and writes; protocol event
// dispatch, route execution, stream lifetime, and joins compile once.
class http2_sans_io_session_engine final {
public:
    http2_sans_io_session_engine(asio::any_io_executor executor, asio::ip::tcp::socket& socket,
        const route_table& routes_value, http2_sans_io_session_context session_value);

    http2_sans_io_session_engine(const http2_sans_io_session_engine&) = delete;
    http2_sans_io_session_engine& operator=(const http2_sans_io_session_engine&) = delete;
    http2_sans_io_session_engine(http2_sans_io_session_engine&&) = delete;
    http2_sans_io_session_engine& operator=(http2_sans_io_session_engine&&) = delete;

    void drain_events();
    [[nodiscard]] http2_feed_result feed_and_drain(std::string_view bytes);

    [[nodiscard]] bool wants_write() const noexcept;
    void take_output(std::string& output);
    [[nodiscard]] bool write_failed() const noexcept;
    [[nodiscard]] bool writer_should_exit() const noexcept;
    [[nodiscard]] worker_signal::wait_operation wait_for_write();
    void writer_write_failed(std::error_code error) noexcept;
    void output_write_completed() noexcept;
    void writer_submitting() noexcept;
    void writer_launch_failed() noexcept;
    void writer_completed(std::exception_ptr exception) noexcept;

    [[nodiscard]] bool connection_failed() const noexcept;
    [[nodiscard]] bool terminated() const noexcept;
    // The drain completed: the final GOAWAY was written and either every stream
    // ended, or a timeout's drain reached its bound with the write side idle and
    // the remaining streams were cancelled.
    [[nodiscard]] bool drain_finished() const noexcept;
    // The peer ended its input (TCP FIN or TLS close_notify): it can open no
    // further stream, so the final GOAWAY is queued for the writer to flush
    // before the session ends (RFC 9113 §6.8). In-flight streams are cancelled
    // with the session; their responses are not completed.
    void peer_input_ended() noexcept;
    void set_inactivity_phase() noexcept;
    void touch_activity() noexcept;
    // The connection's scanner entry: the reader times its phases, the
    // writer runs the entry's write stall clock around each socket write.
    [[nodiscard]] connection_scanner::entry& scanner_entry() const noexcept {
        return session_.scanner_entry();
    }
    void wake_writer() noexcept;
    void terminate(std::error_code error) noexcept;
    // The one way a stream is reset outside the protocol's own event
    // handling: queue RST_STREAM, stop the stream's waiters, release its DATA
    // budget and wake the writer. A RST_STREAM that cannot be queued would
    // leave the peer's stream open and counted against its concurrency limit,
    // so that failure terminates the session.
    void reset_stream_no_throw(std::uint32_t stream_id, http2_error_code error) noexcept;
    [[nodiscard]] task<void> finish();

private:
    void reset_event_stream(std::uint32_t stream_id, http2_error_code error) noexcept;
    [[nodiscard]] http2_sans_io_stream_runtime* resolve_stream_route(const http2_request_head_event& message_head);
    void on_message_head(http2_request_head_event& message_head);
    void on_body_chunk(http2_message_body_chunk_event& body_chunk);
    void on_tunnel_data(http2_tunnel_data_event& tunnel_data);
    void on_tunnel_end(const http2_tunnel_end_event& tunnel_end) noexcept;
    void on_message_end(const http2_message_end_event& message_end);
    void on_stream_closed(const http2_stream_closed_event& stream_closed) noexcept;
    [[nodiscard]] bool push_request(std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] task<void> dispatch_one(std::uint32_t stream_id);
    [[nodiscard]] bool admit_stream(std::uint32_t stream_id);
    void remove_stream_runtime(std::uint32_t stream_id) noexcept;
    // Scanner tick: resets each stream whose request body outlasted
    // request_body_timeout_ / body_completion_timeout_, or whose send-window
    // wait outlasted write_timeout_. Other streams are unaffected.
    void expire_stream_deadlines(std::int64_t now_ms) noexcept;
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;

    asio::any_io_executor executor_;
    asio::ip::tcp::socket& socket_;
    const route_table& routes_;
    http2_sans_io_session_context session_;
    inbound_buffer_account inbound_buffers_;
    context_services base_services_;
    std::string_view remote_address_;
    ruvia::http2_connection connection_;
    worker_signal write_signal_;
    http2_data_output_budget output_budget_;
    worker_signal handler_finished_;
    worker_signal writer_finished_;
    http2_sans_io_session_lifecycle lifecycle_;
    multiplexed_session_termination termination_;
    http_request_admission_policy admission_policy_;
    http2_sans_io_stream_runtime_table stream_runtimes_;
    http2_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    bool drain_finished_{};
    // A batch removed from protocol output still belongs to the socket writer.
    bool output_write_in_flight_{};
    bool has_pushed_streams_{};
    // Scanner tick (per-stream deadlines) and drain callback.
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: destroyed (and so unregistered) before the state its
    // completion touches.
    multiplexed_drain_timer drain_timer_;
};

}  // namespace ruvia::detail
