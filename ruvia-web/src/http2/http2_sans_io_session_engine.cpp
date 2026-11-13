#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/error.h"
#include "ruvia/web/streaming.h"

#include "http/context_access.h"
#include "http/http_tunnel_session.h"
#include "http/http_websocket_connection.h"
#include "http2/http2_sans_io_session.h"
#include "server/connection_lifecycle.h"
#include "server/http_server_options.h"
#include "server/multiplexed_stream_signal.h"
#include "server/route_table.h"
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

namespace ruvia::detail {
namespace {

// Product limit on a drain's wait for the PING round trip before the final
// GOAWAY (a request cap or a connection timeout; a stop closes at once).
constexpr std::chrono::milliseconds http2_drain_round_trip_bound{std::chrono::seconds(1)};

// Streaming request-body reader for the sans-I/O session; the body_reader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine read_body_chunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class http2_sans_io_request_body_reader final
    : public multiplexed_request_body_reader_base<http2_sans_io_request_body_reader> {
public:
    static constexpr const char* terminated_error_message = "HTTP/2 connection ended during body read";
    static constexpr const char* reset_error_message = "HTTP/2 request stream reset during body read";

    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so an unread content never prompts the client to send it.
    http2_sans_io_request_body_reader(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, const std::optional<http_request_body_failure>& body_failure,
        multiplexed_stream_signal& signal, worker_signal& write_signal,
        http_interim_response_output* continue_output) noexcept
        : multiplexed_request_body_reader_base(signal, continue_output),
          connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          body_failure_(body_failure),
          write_signal_(write_signal) {}

private:
    friend class multiplexed_request_body_reader_base<http2_sans_io_request_body_reader>;

    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const noexcept {
        return body_failure_;
    }

    [[nodiscard]] bool stream_aborted() const noexcept {
        return connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed;
    }

    [[nodiscard]] std::string_view pop_chunk() noexcept {
        const auto chunk = body_queue_.pop();
        // pop() returns the previous chunk's receive-window credit; the
        // resulting WINDOW_UPDATE must reach the socket even if this reader
        // now suspends while the peer is blocked on that window.
        if (connection_.wants_write()) {
            write_signal_.notify();
        }
        return chunk;
    }

    [[nodiscard]] bool body_ended() const noexcept {
        return body_queue_.ended();
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    const std::optional<http_request_body_failure>& body_failure_;
    worker_signal& write_signal_;
};

class http2_sans_io_response_stream_sink final
    : public multiplexed_response_stream_sink_base<http2_sans_io_response_stream_sink, http2_sans_io_stream_runtime> {
public:
    http2_sans_io_response_stream_sink(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_stream_runtime& stream_runtime,
        worker_signal& write_signal, multiplexed_stream_signal& stream_signal,
        http2_data_output_budget& output_budget,
        http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding,
        const compression_config* compression, const http_cors_policy* cors) noexcept
        : multiplexed_response_stream_sink_base(
              stream_runtime, stream_signal, request_method, codecs, response_coding, compression, cors),
          connection_(connection),
          stream_id_(stream_id),
          write_signal_(write_signal),
          output_budget_(output_budget) {}

private:
    friend class multiplexed_response_stream_sink_base<http2_sans_io_response_stream_sink,
        http2_sans_io_stream_runtime>;

    static constexpr const char* terminated_error_message = "HTTP/2 connection ended during response stream";
    static constexpr const char* sleep_terminated_error_message = "HTTP/2 connection ended during sleep";
    static constexpr const char* reset_error_message = "HTTP/2 response stream reset";
    static constexpr const char* sleep_reset_error_message = "HTTP/2 response stream reset during sleep";

    [[nodiscard]] bool stream_aborted() const noexcept {
        return connection_.stream_aborted(stream_id_);
    }

    void wake_writer() noexcept {
        write_signal_.notify();
    }

    [[nodiscard]] http_response_stream_commit_plan submit_streaming_head(
        http_response&& response, http_response_trailer_intent trailer_intent) {
        const auto head_result =
            connection_.submit_streaming_response_head(stream_id_, std::move(response), trailer_intent);
        const auto* submitted_head = std::get_if<http_response_stream_commit_plan>(&head_result);
        if (submitted_head != nullptr) {
            return *submitted_head;
        }
        const auto head_error = std::get<ruvia::http2_response_head_submit_error>(head_result);
        if (head_error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
            // A promised push whose stream the client's concurrency limit
            // cannot admit is cancelled; the connection is unaffected.
            (void)connection_.submit_reset(stream_id_, ruvia::http2_error_code::cancel);
            wake_writer();
        }
        if (head_error == ruvia::http2_response_head_submit_error::closed ||
            head_error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
        throw std::logic_error("HTTP/2 streaming response head submission failed");
    }

    task<void> write_encoded_data(std::string_view chunk) {
        const auto status = co_await write_http2_stream_data(connection_, stream_id_, &signal_,
            write_signal_, output_budget_, chunk, http2_end_stream::keep_open);
        switch (status) {
            case http2_stream_data_write_status::completed:
                co_return;
            case http2_stream_data_write_status::session_terminated:
                throw inbound_transport_error(signal_.terminal_error(), terminated_error_message);
            case http2_stream_data_write_status::local_aborted:
            case http2_stream_data_write_status::peer_closed:
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), reset_error_message);
            case http2_stream_data_write_status::invalid_state:
                throw std::logic_error("invalid HTTP/2 response stream DATA state");
            case http2_stream_data_write_status::content_length_exceeded:
                throw std::length_error("HTTP/2 response exceeds Content-Length");
        }
    }

    task<void> finish_stream_response(const http_response_trailer_section& trailer_section) {
        // An empty trailer section ends the stream with an empty DATA frame,
        // which needs both send windows open (RFC 9113 §6.9).
        for (;;) {
            http2_finish_status result_value{};
            try {
                result_value = connection_.finish_response(stream_id_, trailer_section);
            } catch (...) {
                wake_writer();
                throw;
            }
            wake_writer();
            if (result_value == http2_finish_status::accepted) {
                co_return;
            }
            if (result_value == http2_finish_status::invalid_message) {
                throw std::invalid_argument("HTTP/2 response ended before its declared Content-Length");
            }
            if (result_value == http2_finish_status::backpressured &&
                co_await await_http2_send_window(connection_, stream_id_, &signal_) ==
                    http2_send_window_wait_status::ready) {
                continue;
            }
            throw_if_terminated();
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), reset_error_message);
        }
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
};

// Duplex CONNECT transport driven by one HTTP/2 session reader and writer.
// Queued input retains its receive-window credit until the application consumes it.
class http2_sans_io_tunnel_transport final
    : public multiplexed_tunnel_transport_base<http2_sans_io_tunnel_transport> {
public:
    http2_sans_io_tunnel_transport(http2_sans_io_session_engine& engine, ruvia::http2_connection& connection,
        std::uint32_t stream_id, http2_sans_io_body_queue& body_queue, multiplexed_stream_signal& signal,
        worker_signal& write_signal, http2_data_output_budget& output_budget) noexcept
        : multiplexed_tunnel_transport_base(signal),
          engine_(&engine),
          connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          write_signal_(write_signal),
          output_budget_(output_budget) {}

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end disposition) {
        const auto terminal = disposition == http_stream_end::end
                                  ? http2_end_stream::end_stream
                                  : http2_end_stream::keep_open;
        const auto status = co_await write_http2_stream_data(connection_, stream_id_, &signal_,
            write_signal_, output_budget_, bytes_value, terminal, &aborted_);
        switch (status) {
            case http2_stream_data_write_status::completed:
                co_return std::error_code{};
            case http2_stream_data_write_status::local_aborted:
                co_return std::make_error_code(std::errc::operation_canceled);
            case http2_stream_data_write_status::session_terminated:
                co_return signal_.terminal_error();
            case http2_stream_data_write_status::peer_closed:
                co_return std::make_error_code(std::errc::connection_reset);
            case http2_stream_data_write_status::invalid_state:
            case http2_stream_data_write_status::content_length_exceeded:
                co_return std::make_error_code(std::errc::protocol_error);
        }
        co_return std::make_error_code(std::errc::protocol_error);
    }

    // Malformed received data is a stream error of type PROTOCOL_ERROR
    // (RFC 9113 section 8.1.1); any other abort cancels the stream.
    void abort(bool malformed = false) noexcept {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        engine_->reset_stream_no_throw(
            stream_id_, malformed ? http2_error_code::protocol_error : http2_error_code::cancel);
        signal_.wake();
    }

private:
    friend class multiplexed_tunnel_transport_base<http2_sans_io_tunnel_transport>;

    void require_live() const {
        if (aborted_ || signal_.terminated()) {
            throw inbound_transport_error(
                aborted_ ? std::make_error_code(std::errc::operation_canceled)
                         : signal_.terminal_error(),
                "HTTP/2 tunnel read");
        }
        if (connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), "HTTP/2 tunnel read");
        }
    }

    template <typename buffer_type>
    [[nodiscard]] bool consume_tunnel_chunk(buffer_type& buffer) {
        const auto chunk = body_queue_.pop();
        // pop() returns the previous chunk's receive-window credit; flush
        // the WINDOW_UPDATE even if this reader suspends below.
        if (connection_.wants_write()) {
            wake_writer();
        }
        if (chunk.empty()) {
            return false;
        }
        buffer.append(std::string_view(chunk.data(), chunk.size()));
        body_queue_.release_active_credits();
        if (connection_.wants_write()) {
            wake_writer();
        }
        return true;
    }

    [[nodiscard]] bool receive_ended() const noexcept {
        return connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::ended;
    }

    void wake_writer() noexcept {
        write_signal_.notify();
    }

    http2_sans_io_session_engine* engine_;
    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    bool aborted_{false};
};
struct response_output_target final {
    ::ruvia::http2_connection& connection_;
    worker_signal& writer_;
    std::uint32_t stream_id_;
};

struct push_target final {
    http2_sans_io_session_engine& owner_;
    std::uint32_t stream_id_;
};

[[nodiscard]] http2_sans_io_stream_runtime* http2_select_stream_route(const route_table& routes_value,
    bool request_stream, const http2_server_request_view& request, http2_sans_io_stream_runtime_table& stream_runtimes,
    std::uint32_t stream_id) {
    auto& runtime = stream_runtimes.ensure_accepted(stream_id);
    auto [resolution, body_mode] = resolve_multiplexed_stream_route(
        routes_value, request_stream, request.method_, request.path_, request.authority_, request.protocol_);
    return runtime.select_route(std::move(resolution), body_mode) ? &runtime : nullptr;
}

}  // namespace
http2_sans_io_session_engine::http2_sans_io_session_engine(asio::any_io_executor executor,
    asio::ip::tcp::socket& socket, const route_table& routes_value,
    http2_sans_io_session_context session_value)
    : executor_(std::move(executor)),
      socket_(socket),
      routes_(routes_value),
      session_(std::move(session_value)),
      inbound_buffers_(*session_.options().inbound_budget_,
          session_.options().server_.max_inbound_buffer_bytes_per_connection_),
      base_services_(session_.services().with_inbound_buffers(inbound_buffers_)),
      remote_address_(session_.services().remote_address()),
      connection_(ruvia::http2_connection::server({.frame_observer_ = session_.options().http2_frame_
                                                                          ? +[](void* target, const http2_frame_metadata& metadata) noexcept {
                                                                                auto& self = *static_cast<http2_sans_io_session_engine*>(target);
                                                                                self.session_.options().http2_frame_(server_observation_access::http2_frame(
                                                                                    self.base_services_.get_conn_info().connection_id(), metadata));
                                                                            }
                                                                          : nullptr,
          .header_observer_ = session_.options().http2_headers_ ? +[](void* target, const http2_header_metadata& metadata) noexcept {
              auto& self = *static_cast<http2_sans_io_session_engine*>(target);
              self.session_.options().http2_headers_(server_observation_access::http2_headers(
                  self.base_services_.get_conn_info().connection_id(), metadata));
          }
                                                                : nullptr,
          .observer_context_ = this})),
      write_signal_(session_.services().worker()),
      output_budget_(session_.services().worker()),
      handler_finished_(session_.services().worker()),
      writer_finished_(session_.services().worker()),
      admission_policy_(http_request_admission_policy::server(session_.options())),
      stream_runtimes_(termination_, &inbound_buffers_),
      buffered_response_writer_(*this, connection_, stream_runtimes_, write_signal_, output_budget_, response_file_io{.worker_ = session_.services().worker(), .stop_ = session_.services().get_stop_token(), .timeout_ = session_.options().server_.write_timeout_}) {
    // A plaintext handoff reuses the HTTP/1 scanner entry. It now tracks a
    // multiplexed session, not an unadmitted HTTP/1 request-head read.
    base_services_.bind_routes(routes_);
    session_.scanner_entry().bind_lifecycle(base_services_.connection_lifecycle_value());
    set_inactivity_phase();
    session_.scanner_entry().register_periodic_check(
        scanner_checks_, this,
        [](void* target, std::int64_t now_ms) noexcept {
            static_cast<http2_sans_io_session_engine*>(target)->expire_stream_deadlines(now_ms);
        },
        [](void* target) noexcept { static_cast<http2_sans_io_session_engine*>(target)->request_drain(); });
}

bool http2_sans_io_session_engine::wants_write() const noexcept {
    return connection_.wants_write();
}

void http2_sans_io_session_engine::take_output(std::string& output) {
    // Preserve assign() semantics when the caller reuses its scratch string. The
    // core batch operation only consumes output and invokes its observer after the
    // copy succeeds, so allocation failure leaves protocol bytes and budget intact.
    output.clear();
    (void)connection_.take_output_batch(
        http2_data_output_credit_slots * http2_data_output_credit_bytes + 4096, output,
        [](void* context_value, std::uint32_t stream_id, std::size_t payload_bytes) noexcept {
            static_cast<http2_data_output_budget*>(context_value)->note_data_output(stream_id, payload_bytes);
        },
        &output_budget_);
    output_write_in_flight_ = !output.empty();
}

bool http2_sans_io_session_engine::write_failed() const noexcept {
    return lifecycle_.write_failed();
}

bool http2_sans_io_session_engine::writer_should_exit() const noexcept {
    return lifecycle_.stopping() && active_handler_tasks_ == 0;
}

worker_signal::wait_operation http2_sans_io_session_engine::wait_for_write() {
    return write_signal_.wait();
}

void http2_sans_io_session_engine::output_write_completed() noexcept {
    output_write_in_flight_ = false;
    finish_drain_if_ready();
    output_budget_.reconcile(connection_, true);
}

void http2_sans_io_session_engine::writer_write_failed(std::error_code error) noexcept {
    lifecycle_.mark_write_failed();
    terminate(error);
}

void http2_sans_io_session_engine::writer_submitting() noexcept {
    lifecycle_.mark_writer_submitted();
}

void http2_sans_io_session_engine::writer_launch_failed() noexcept {
    lifecycle_.mark_writer_launch_failed();
}

void http2_sans_io_session_engine::writer_completed(std::exception_ptr exception) noexcept {
    if (exception != nullptr) {
        lifecycle_.record_writer_failure(std::move(exception));
        terminate(std::make_error_code(std::errc::operation_canceled));
    }
    lifecycle_.mark_writer_done();
    writer_finished_.notify();
}

bool http2_sans_io_session_engine::connection_failed() const noexcept {
    return connection_.connection_error().has_value();
}

bool http2_sans_io_session_engine::terminated() const noexcept {
    return termination_.terminated();
}

bool http2_sans_io_session_engine::drain_finished() const noexcept {
    return drain_finished_;
}

void http2_sans_io_session_engine::request_drain() noexcept {
    // The scanner asks for a drain only when a connection timeout fired; it
    // recorded the close request before invoking this callback.
    drain_timer_.request_timeout();
    begin_drain();
    finish_drain_if_ready();
}

void http2_sans_io_session_engine::begin_drain() noexcept {
    try {
        connection_.begin_drain();
    } catch (...) {
        terminate(std::make_error_code(std::errc::not_enough_memory));
        return;
    }
    wake_writer();
    arm_drain_timer();
}

void http2_sans_io_session_engine::arm_drain_timer() noexcept {
    // A final boundary needs no time bound, unless a timeout's drain must
    // still end the session.
    if (!drain_timer_.needed(termination_.terminated(), connection_.drain_final())) {
        return;
    }
    // RFC 9113 §6.8 asks only for "at least one round-trip time" before the
    // final GOAWAY; nothing requires waiting for the PING ACK.
    drain_timer_.arm(session_.services().worker(), http2_drain_round_trip_bound, [this]() noexcept { drain_timer_expired(); }, [this]() noexcept { terminate(std::make_error_code(std::errc::not_enough_memory)); });
}

void http2_sans_io_session_engine::drain_timer_expired() noexcept {
    if (termination_.terminated()) {
        return;
    }
    try {
        connection_.finish_drain();
    } catch (...) {
        terminate(std::make_error_code(std::errc::not_enough_memory));
        return;
    }
    wake_writer();
    drain_timer_.mark_expired();
    finish_drain_if_ready();
}

void http2_sans_io_session_engine::finish_drain_if_ready() noexcept {
    // The first GOAWAY of a server drain still admits in-flight streams; only the
    // final one (after the round trip or its time bound) lets the connection close.
    // A write in flight still owns the socket; its completion re-checks.
    if (drain_finished_ || !connection_.drain_final() || output_write_in_flight_) {
        return;
    }
    if (stream_runtimes_.size() == 0 && active_handler_tasks_ == 0 && !connection_.wants_write()) {
        drain_finished_ = true;
        std::error_code error;
        socket_.cancel(error);
        return;
    }
    if (drain_timer_.expired()) {
        // A connection timeout's drain reached its bound with the write side
        // idle: cancel the remaining streams and end the session now. The
        // writer still flushes the final GOAWAY before the close alert.
        drain_finished_ = true;
        terminate(std::make_error_code(std::errc::timed_out));
    }
}

void http2_sans_io_session_engine::peer_input_ended() noexcept {
    base_services_.set_connection_close_reason(connection_close_reason::peer_eof);
    // RFC 9113 §6.8: "Endpoints SHOULD always send a GOAWAY frame before
    // closing a connection". The session closes either way; a failed
    // allocation only loses this frame.
    try {
        connection_.finish_drain();
    } catch (...) {
        return;
    }
    wake_writer();
}

void http2_sans_io_session_engine::set_inactivity_phase() noexcept {
    session_.scanner_entry().set_phase(multiplexed_inactivity_phase(connection_.received_peer_settings(),
        connection_.header_block_in_progress(), stream_runtimes_.size() != 0 || active_handler_tasks_ != 0,
        stream_runtimes_.tunnel_count() != 0));
}

void http2_sans_io_session_engine::expire_stream_deadlines(std::int64_t now_ms) noexcept {
    const auto& server = session_.options().server_;
    // Resetting a stream can remove its runtime, so each pass resets at most
    // one stream, outside the table walk. Each expiry is one-shot (the body is
    // marked closed, a send-window wait expires once), so the passes end.
    while (!termination_.terminated()) {
        std::uint32_t overdue_stream = 0;
        bool body_overdue = false;
        stream_runtimes_.for_each([&](http2_sans_io_stream_runtime& runtime) {
            if (overdue_stream != 0) {
                return;
            }
            if (runtime.request_body_overdue(now_ms, server.request_body_timeout_, server.body_completion_timeout_)) {
                overdue_stream = runtime.stream_id();
                body_overdue = true;
                return;
            }
            auto* signal = runtime.signal();
            if (signal != nullptr && server.write_timeout_.has_value() &&
                signal->output_wait_expired(now_ms, *server.write_timeout_)) {
                overdue_stream = runtime.stream_id();
            }
        });
        if (overdue_stream == 0) {
            return;
        }
        if (body_overdue) {
            stream_runtimes_.mark_request_body_closed(overdue_stream);
        }
        // Only this stream ends, without a response, where HTTP/1 closes its
        // connection for the same timeout.
        reset_event_stream(overdue_stream, http2_error_code::cancel);
    }
}

void http2_sans_io_session_engine::remove_stream_runtime(std::uint32_t stream_id) noexcept {
    if (stream_runtimes_.remove(stream_id)) {
        set_inactivity_phase();
    }
}

void http2_sans_io_session_engine::touch_activity() noexcept {
    session_.scanner_entry().touch();
}

void http2_sans_io_session_engine::wake_writer() noexcept {
    if (connection_.wants_write() || writer_should_exit()) {
        write_signal_.notify();
    }
}

void http2_sans_io_session_engine::terminate(std::error_code error) noexcept {
    if (!termination_.terminate(error)) {
        return;
    }
    if (auto* lifecycle = base_services_.connection_lifecycle_value();
        lifecycle != nullptr && (!drain_finished_ || error == std::errc::timed_out)) {
        lifecycle->note_error(error);
    }
    output_budget_.wake();
    std::error_code ignored;
    socket_.cancel(ignored);
    stream_runtimes_.for_each([](http2_sans_io_stream_runtime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->stop();
        }
    });
    write_signal_.notify();
}

void http2_sans_io_session_engine::reset_stream_no_throw(
    std::uint32_t stream_id, http2_error_code error) noexcept {
    try {
        (void)connection_.submit_reset(stream_id, error);
        if (auto* signal = stream_runtimes_.signal_for(stream_id)) {
            signal->stop();
        }
    } catch (...) {
        terminate(std::make_error_code(std::errc::not_enough_memory));
    }
    output_budget_.release_and_reconcile(stream_id, connection_);
    output_budget_.wake();
    wake_writer();
}

bool http2_sans_io_session_engine::push_request(std::uint32_t associated_stream_id, http_push_request_view request) {
    auto* parent_value = stream_runtimes_.find(associated_stream_id);
    const auto* head = parent_value != nullptr ? parent_value->request_head() : nullptr;
    if (head == nullptr || (associated_stream_id & 1U) == 0) {
        return false;
    }
    const auto original = head->request_view();
    if (!http_ascii_equals_ignore_case(original.scheme_, request.scheme_) ||
        !http_authorities_equal(borrowed_text(original.authority_), request.authority_,
            http_ascii_equals_ignore_case(original.scheme_, "https") ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    auto promised = connection_.submit_push_request(associated_stream_id, request);
    if ((promised.index() != 0)) {
        if (std::get<1>(promised) == http2_push_submit_error::invalid_request) {
            throw std::invalid_argument("invalid HTTP/2 push request");
        }
        return false;
    }
    auto& promised_event = std::get<0>(promised);
    const auto id = promised_event.stream_id();
    http2_sans_io_stream_runtime* runtime = nullptr;
    try {
        runtime = http2_select_stream_route(
            routes_, static_cast<bool>(base_services_.request_stream()), promised_event.request_view(), stream_runtimes_, id);
        if (runtime == nullptr || !runtime->hold_request_head(std::move(promised_event))) {
            throw std::logic_error("HTTP/2 push dispatch admission failed");
        }
    } catch (...) {
        reset_stream_no_throw(id, http2_error_code::cancel);
        remove_stream_runtime(id);
        throw;
    }
    runtime->bind_push_parent(associated_stream_id);
    has_pushed_streams_ = true;
    if (!admit_stream(id)) {
        reset_event_stream(id, http2_error_code::cancel);
        return false;
    }
    wake_writer();
    return true;
}

task<void> http2_sans_io_session_engine::dispatch_one(std::uint32_t stream_id) {
    struct stream_cleanup_guard final {
        http2_sans_io_session_engine& engine_;
        std::uint32_t stream_id_;
        ~stream_cleanup_guard() noexcept {
            if (auto* runtime = engine_.stream_runtimes_.find(stream_id_)) {
                runtime->discard_body();
                if (auto* request_head = runtime->request_head()) {
                    try {
                        (void)engine_.connection_.release(std::move(*request_head));
                    } catch (...) {
                        engine_.terminate(std::make_error_code(std::errc::not_enough_memory));
                    }
                }
            }
            engine_.output_budget_.release_and_reconcile(stream_id_, engine_.connection_);
            engine_.remove_stream_runtime(stream_id_);
            engine_.wake_writer();
        }
    } cleanup{*this, stream_id};
    try {
        const auto& options = session_.options();
        const auto request_start = options.access_log_.callback_
                                       ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        auto* stream_runtime = stream_runtimes_.find(stream_id);
        auto* request_head = stream_runtime != nullptr ? stream_runtime->request_head() : nullptr;
        if (request_head == nullptr) {
            co_return;
        }
        if (!stream_runtime->route_selected()) {
            reset_stream_no_throw(stream_id, http2_error_code::internal_error);
            co_return;
        }
        const bool streaming_body = stream_runtime->body_mode() == request_body_mode::stream;
        auto* stream_signal = stream_runtime->signal();
        if (stream_signal == nullptr) {
            reset_stream_no_throw(stream_id, http2_error_code::internal_error);
            co_return;
        }
        http_request request = request_head->build_request(streaming_body ? std::string_view{} : stream_runtime->buffered_body());
        http_response response;
        response_output_target output_target{connection_, write_signal_, stream_id};
        http_interim_response_output interim_output(&output_target, [](void* raw, const http_interim_response_head& head) -> task<void> {
            auto& target = *static_cast<response_output_target*>(raw);
            const auto status = target.connection_.submit_interim_response_head(target.stream_id_, head);
            if (status != http2_submit_status::accepted) {
                throw std::invalid_argument("HTTP/2 interim response rejected");
            }
            target.writer_.notify();
            co_return;
        });
        http_connection_advertisement_output advertisements(&output_target, [](void* raw, std::span<const std::string_view> origins) {
            auto& target = *static_cast<response_output_target*>(raw);
            if (target.connection_.submit_origin_advertisement(origins) != http2_submit_status::accepted) {
                throw std::invalid_argument("HTTP/2 ORIGIN advertisement rejected");
            }
            target.writer_.notify(); }, [](void* raw, std::string_view value) {
            auto& target = *static_cast<response_output_target*>(raw);
            if (target.connection_.submit_alternative_service_advertisement(target.stream_id_, {}, value) != http2_submit_status::accepted) {
                throw std::invalid_argument("HTTP/2 ALTSVC advertisement rejected");
            }
            target.writer_.notify(); });
        push_target push_target_value{*this, stream_id};
        http_push_output push_output(&push_target_value, [](void* raw, http_push_request_view promised_request) -> bool {
            auto& target = *static_cast<push_target*>(raw);
            return target.owner_.push_request(target.stream_id_, promised_request);
        });
        const auto& resolution = stream_runtime->resolution();
        const auto expectation_plan = request_head->expectation_plan(http_unsupported_expectation_policy::reject);
        auto admission_services = base_services_;
        const auto& initial = request_head->snapshot();
        admission_services.bind_body_state({
            .framing_ = initial.body_open_ ? request_body_framing::multiplexed : request_body_framing::none,
            .content_length_ = initial.declared_content_length_,
            .initial_end_stream_ = (stream_id & 1U) != 0 ? std::optional(initial.initial_end_stream_) : std::nullopt,
        });
        auto reset_request = [this, stream_id](request_reset_code error) {
            const auto* code = std::get_if<http2_error_code>(&error);
            reset_stream_no_throw(stream_id, code != nullptr ? *code : http2_error_code::cancel);
        };
        request_stream_control stream_control{callback_access::bind<void(request_reset_code)>(reset_request)};
        admission_services.bind_stream_control(stream_control);
        admission_services.bind_request_channels({
            .interim_output_ = &interim_output,
            .connection_advertisements_ = &advertisements,
            .push_output_ = &push_output,
            .priority_update_ = &stream_runtime->priority_update(),
        });
        admission_services.bind_request_trailers(stream_runtime->trailers());
        auto admission = admit_http_request(
            {
                .request_ = request,
                .resolution_ = resolution,
                .coding_ = http_response_coding_selection_for(request),
                .expectation_ = &expectation_plan,
                .start_ = request_start,
            },
            admission_policy_, admission_services);
        const auto& request_services = admission.services();
        const auto* resolved = admission.resolved();

        std::optional<body_reader_binding<http2_sans_io_request_body_reader>> body_reader_storage;
        if (streaming_body && !request_head->snapshot().connect_pending_ &&
            !stream_runtime->body_failure().has_value()) {
            const bool continue_on_read = expectation_plan == http_server_expectation_plan::send_continue && resolved != nullptr;
            body_reader_storage.emplace(connection_, stream_id, stream_runtime->body_queue(),
                stream_runtime->body_failure(), *stream_signal, write_signal_,
                continue_on_read ? &interim_output : nullptr);
        }
        auto dispatch_services = request_services;
        if (body_reader_storage) {
            dispatch_services.bind_streaming_request_body(body_reader_storage->facade());
        }
        const stop_group dispatch_stop(dispatch_services.get_stop_token(), stream_signal->terminal_stop_token());
        const auto dispatch_stop_token = dispatch_stop.token();
        dispatch_services.bind_stop_token(dispatch_stop_token);

        const auto reset_dispatch_stream = [this, stream_id](bool cancelled) noexcept {
            reset_stream_no_throw(stream_id, cancelled ? http2_error_code::cancel : http2_error_code::internal_error);
        };
        const auto finish_upgrade_commit = [this, stream_id, stream_runtime, stream_signal, &admission, &options](
                                               http_status_code status, context& context_value) {
            stream_runtime->mark_response_committed();
            set_inactivity_phase();
            context_access::commit_upgrade(context_value);
            wake_writer();
            admission.tail().record_access(options.access_log_, status);
            return http2_sans_io_tunnel_transport(*this, connection_, stream_id,
                stream_runtime->body_queue(), *stream_signal, write_signal_, output_budget_);
        };
        bool body_failure_handled = false;
        if (const auto& body_failure = stream_runtime->body_failure();
            body_failure.has_value() &&
            admission.endpoint() != http_request_endpoint::rejection) {
            body_failure_handled = true;
            response = co_await respond_http_request_body_failure(
                routes_, request, *body_failure, request_services, admission.tail());
        } else {
            switch (admission.endpoint()) {
                case http_request_endpoint::rejection:
                    admission.tail().disable_response_coding();
                    response = co_await respond_http_request_rejection(
                        routes_, request, *admission.rejection(), request_services);
                    break;
                case http_request_endpoint::tunnel: {
                    if (!streaming_body || !request_head->snapshot().connect_pending_) {
                        reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                        co_return;
                    }
                    auto commit = [this, stream_id, &finish_upgrade_commit](
                                      context& context_value, const http_response& head_response) {
                        if (connection_.submit_connect_response_head(stream_id, head_response) !=
                                http2_submit_status::accepted ||
                            !stream_runtimes_.mark_tunnel(stream_id)) {
                            throw std::invalid_argument("HTTP/2 CONNECT response head rejected");
                        }
                        return finish_upgrade_commit(head_response.status(), context_value);
                    };
                    auto commit_websocket = [this, stream_id, &request, &finish_upgrade_commit](
                                                context& context_value, const websocket_route_endpoint& endpoint,
                                                std::span<const http_header_view> headers)
                        -> task<websocket_upgrade_attempt<http2_sans_io_tunnel_transport>> {
                        const auto validation = ruvia::validate_http2_websocket_handshake(
                            connection_, stream_id, request, endpoint.handshake_validation());
                        if (const auto* failure = validation.failure()) {
                            co_return reject_request_stream_websocket(context_value, *failure);
                        }
                        const auto result = connection_.submit_websocket_handshake(stream_id, request, validation,
                            {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
                        const auto* handshake = std::get_if<websocket_extended_connect_negotiation>(&result);
                        if (handshake == nullptr || !stream_runtimes_.mark_tunnel(stream_id)) {
                            throw std::invalid_argument("HTTP/2 WebSocket handshake rejected");
                        }
                        co_return websocket_upgrade_commit<http2_sans_io_tunnel_transport>(
                            finish_upgrade_commit(http_status::ok, context_value), handshake->compression(),
                            handshake->subprotocol());
                    };
                    http2_sans_io_response_stream_sink refusal_sink(connection_, stream_id,
                        *stream_runtime, write_signal_, *stream_signal, output_budget_, request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                        options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto refusal_writer = make_response_stream_writer(refusal_sink);
                    auto upgrade_services = dispatch_services;
                    if (is_request_stream_tunnel_route(resolved->route())) {
                        upgrade_services.bind_response_stream(refusal_writer);
                    }
                    auto buffered = co_await execute_tunnel_upgrade_session<http2_sans_io_tunnel_transport>(
                        routes_, request, *resolved, upgrade_services, options,
                        session_.scanner_entry(), inbound_buffers_, admission.udp_tunnel(),
                        std::string_view{}, commit, commit_websocket);
                    if (refusal_sink.committed()) {
                        admission.tail().record_access(options.access_log_, committed_response_stream_status(refusal_sink));
                        co_return;
                    }
                    if (!buffered) {
                        co_return;
                    }
                    response = std::move(*buffered);
                    break;
                }
                case http_request_endpoint::websocket: {
                    const auto& websocket_endpoint = *resolved->route().endpoint().websocket();
                    const auto handshake_validation = ruvia::validate_http2_websocket_handshake(
                        connection_, stream_id, request, websocket_endpoint.handshake_validation());
                    if (handshake_validation.accepted() == nullptr) {
                        const auto* failure = handshake_validation.failure();
                        if (failure == nullptr) {
                            throw std::logic_error("HTTP/2 WebSocket validation returned no outcome");
                        }
                        response = co_await routes_.handle_error(request,
                            http_error_info({.status_ = failure->protocol_error().status(), .code_ = failure->protocol_error().code(), .message_ = std::string_view(failure->protocol_error().what())}),
                            request_services);
                        failure->apply_required_response_headers(response);
                        break;
                    }
                    if (!streaming_body) {
                        reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                        co_return;
                    }
                    auto commit = [this, stream_id, &request, &handshake_validation, &websocket_endpoint,
                                      &finish_upgrade_commit](
                                      context& context_value, std::span<const http_header_view> headers) {
                        const auto handshake_result = connection_.submit_websocket_handshake(stream_id, request,
                            handshake_validation,
                            {.supported_subprotocols_ = websocket_endpoint.subprotocols(),
                                .response_headers_ = headers,
                                .deflate_ = websocket_endpoint.deflate()});
                        const auto* submitted_handshake =
                            std::get_if<websocket_extended_connect_negotiation>(&handshake_result);
                        if (submitted_handshake == nullptr || !stream_runtimes_.mark_tunnel(stream_id)) {
                            // The handshake is not started, so stream dispatch still
                            // answers this pending CONNECT with an error response.
                            throw std::invalid_argument("HTTP/2 WebSocket handshake rejected");
                        }
                        return websocket_upgrade_commit<http2_sans_io_tunnel_transport>(
                            finish_upgrade_commit(http_status::ok, context_value),
                            submitted_handshake->compression(), submitted_handshake->subprotocol());
                    };
                    auto buffered = co_await execute_websocket_upgrade_session<http2_sans_io_tunnel_transport>(
                        routes_, request, *resolved, dispatch_services, options,
                        session_.scanner_entry(), inbound_buffers_, std::string_view{}, commit);
                    if (!buffered) {
                        co_return;
                    }
                    response = std::move(*buffered);
                    break;
                }
                case http_request_endpoint::response_stream: {
                    // RST_STREAM or the connection ending stops the request:
                    // context::stop_token() combines it with the worker's stop.
                    if (connection_.stream_aborted(stream_id)) {
                        stream_signal->stop();
                    }
                    const stop_group request_stop(
                        dispatch_services.get_stop_token(), stream_signal->terminal_stop_token());
                    const auto request_stop_token = request_stop.token();
                    auto stream_services = dispatch_services;
                    stream_services.bind_stop_token(request_stop_token);
                    http2_sans_io_response_stream_sink sink_value(connection_, stream_id,
                        *stream_runtime, write_signal_, *stream_signal, output_budget_, request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                        options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto result_value = co_await dispatch_response_stream_with(sink_value, routes_, request, *resolved, stream_services,
                        [this, stream_id]() noexcept { return connection_.stream_aborted(stream_id); });
                    if (complete_multiplexed_response_stream_dispatch(result_value,
                            stream_runtime->body_failure().has_value(), options, remote_address_,
                            admission.tail(), response, body_failure_handled, reset_dispatch_stream)) {
                        co_return;
                    }
                    break;
                }
                case http_request_endpoint::buffered:
                    if (resolved == nullptr) {
                        co_await routes_.dispatch_buffered_response(request, resolution,
                            options.document_root_.binding(), dispatch_services, admission.static_file_mode(), response);
                        break;
                    }
                    [[fallthrough]];
                case http_request_endpoint::stream_body:
                    body_failure_handled = co_await routes_.dispatch_resolved_route(request,
                        *resolved, dispatch_services, &stream_runtime->body_failure(), response);
                    break;
            }
        }

        co_await finish_multiplexed_buffered_response(stream_id, stream_runtime->body_failure(),
            body_failure_handled, response, request, routes_, request_services,
            options, remote_address_, admission.tail(), buffered_response_writer_, reset_dispatch_stream);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!connection_.stream_aborted(stream_id)) {
            reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        }
        session_.options().connection_failure_.invoke(remote_address_, failure);
    }
}

bool http2_sans_io_session_engine::admit_stream(std::uint32_t stream_id) {
    bool counted = false;
    try {
        auto* signal = stream_runtimes_.begin_dispatch(stream_id, session_.services().worker());
        if (signal == nullptr) {
            return false;
        }
        ++active_handler_tasks_;
        counted = true;
        ruvia::async_start(dispatch_one(stream_id), asio::bind_executor(executor_, [this](std::exception_ptr exception) noexcept {
            if (exception != nullptr) {
                if (auto* lifecycle = base_services_.connection_lifecycle_value()) {
                    lifecycle->note_exception(exception);
                }
                session_.options().connection_failure_.invoke(remote_address_, std::move(exception));
                terminate(std::make_error_code(std::errc::operation_canceled));
            }
            --active_handler_tasks_;
            set_inactivity_phase();
            finish_drain_if_ready();
            if (active_handler_tasks_ == 0) {
                handler_finished_.notify();
            }
            wake_writer();
        }));
    } catch (...) {
        if (counted) {
            --active_handler_tasks_;
        }
        if (auto* runtime = stream_runtimes_.find(stream_id)) {
            runtime->discard_body();
        }
        session_.options().connection_failure_.invoke(remote_address_, std::current_exception());
        return false;
    }
    return true;
}

void http2_sans_io_session_engine::reset_event_stream(
    std::uint32_t stream_id, http2_error_code error) noexcept {
    auto* signal = stream_runtimes_.signal_for(stream_id);
    reset_stream_no_throw(stream_id, error);
    if (signal != nullptr) {
        signal->wake();
    } else {
        remove_stream_runtime(stream_id);
    }
    wake_writer();
}

http2_sans_io_stream_runtime* http2_sans_io_session_engine::resolve_stream_route(
    const http2_request_head_event& message_head) {
    const auto request_view = message_head.request_view();
    if (request_view.method_.empty()) {
        return nullptr;
    }
    return http2_select_stream_route(routes_, static_cast<bool>(base_services_.request_stream()), request_view,
        stream_runtimes_, message_head.stream_id());
}

void http2_sans_io_session_engine::on_message_head(http2_request_head_event& message_head) {
    const auto& options = session_.options();
    const auto stream_id = message_head.stream_id();
    base_services_.note_connection_request();
    ++accepted_request_heads_;
    if (!connection_.draining() && options.server_.max_requests_per_connection_.has_value() &&
        accepted_request_heads_ >= *options.server_.max_requests_per_connection_) {
        begin_drain();
    }
    const auto expectation_plan =
        message_head.expectation_plan(http_unsupported_expectation_policy::reject);
    const auto snapshot = message_head.snapshot();
    auto* stream_runtime = resolve_stream_route(message_head);
    if (stream_runtime == nullptr || !stream_runtime->hold_request_head(std::move(message_head))) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    // An accepted stream ends the stream-less idle wait (awaiting_stream),
    // even when it starts and ends within this input batch.
    set_inactivity_phase();
    if (snapshot.priority_update_.has_value()) {
        stream_runtime->reprioritize(snapshot.priority_update_->request_priority());
    }
    const bool connect_request = snapshot.connect_pending_;
    if (snapshot.body_open_ && !connect_request) {
        stream_runtimes_.mark_request_body_open(stream_id, session_.scanner_entry().last_active_ms());
    }
    const bool resolved_route =
        stream_runtime->route_selected() && stream_runtime->resolution().resolved() != nullptr;
    const bool streaming_body = !connect_request && stream_runtime->route_selected() &&
                                stream_runtime->body_mode() == request_body_mode::stream &&
                                snapshot.body_open_;
    const bool body_limit_exceeded = [&]() noexcept {
        if (!resolved_route || connect_request || !snapshot.declared_content_length_.has_value()) {
            return false;
        }
        const auto total_limit = http_request_body_limit(
            stream_runtime->resolution(), stream_runtime->body_mode(), admission_policy_);
        const auto failure = http_request_body_size_failure(*snapshot.declared_content_length_, total_limit);
        if (!failure.has_value()) {
            return false;
        }
        stream_runtime->mark_body_failure(*failure);
        return true;
    }();
    // Same policy as HTTP/1: 100 Continue is sent when the server starts
    // consuming the content. A buffered route consumes it before dispatch, so
    // it is sent now; a streaming handler's first body read sends it; an
    // unresolved or already-oversized request never reads the content and
    // answers with its final status alone (RFC 9110 §10.1.1).
    if (expectation_plan == http_server_expectation_plan::send_continue && resolved_route && !connect_request &&
        !body_limit_exceeded && stream_runtime->body_mode() == request_body_mode::buffered) {
        const auto status = connection_.submit_interim_response_head(
            stream_id, http_interim_response_head(ruvia::http_status::continue_value));
        if (status == http2_submit_status::accepted) {
            wake_writer();
        } else {
            if (status != http2_submit_status::closed) {
                reset_event_stream(stream_id, http2_error_code::internal_error);
            } else {
                remove_stream_runtime(stream_id);
            }
            return;
        }
    }
    // An unresolved or already-oversized request is answered from its head
    // alone; waiting for content the client may withhold (Expect) would stall
    // the stream.
    if (expectation_plan == http_server_expectation_plan::reject_unsupported || connect_request ||
        streaming_body || !resolved_route || body_limit_exceeded) {
        if (!admit_stream(stream_id)) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
        }
    }
}

void http2_sans_io_session_engine::on_body_chunk(http2_message_body_chunk_event& body_chunk) {
    const auto& options = session_.options();
    const auto stream_id = body_chunk.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    if (stream_runtime == nullptr) {
        // Destroying the taken credit returns its bytes to the receive windows.
        if (body_chunk.take_credit().valid()) {
            wake_writer();
        }
        return;
    }
    // The reader stamped its scanner entry when this input arrived.
    stream_runtime->note_request_body_progress(session_.scanner_entry().last_active_ms());
    if (!stream_runtime->route_selected()) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    const bool streaming_body = stream_runtime->body_mode() == request_body_mode::stream;
    if (stream_runtime->body_failure().has_value() || (!streaming_body && stream_runtime->dispatched())) {
        // A buffered body is dispatched before its end only when its
        // expectation was rejected, its body limit was exceeded, or its route
        // is unresolved: the handler never receives that content and the
        // request borrows the buffered bytes as they were at dispatch.
        // Appending would reallocate under that borrowed view, so later DATA
        // is discarded and only its flow-control credit returned.
        (void)body_chunk.take_credit();
        wake_writer();
        return;
    }
    const auto total_limit =
        http_request_body_limit(stream_runtime->resolution(), stream_runtime->body_mode(), admission_policy_);
    const auto store_status =
        stream_runtime->store_body(body_chunk, total_limit, options.server_.max_buffered_body_bytes_);
    if (store_status == http2_request_body_store_status::backlog_overflow) {
        reset_event_stream(stream_id, http2_error_code::cancel);
        return;
    }
    if (store_status == http2_request_body_store_status::too_large ||
        store_status == http2_request_body_store_status::buffer_limit) {
        const auto failure = store_status == http2_request_body_store_status::buffer_limit
                                 ? http_request_body_failure::buffer_limit
                                 : http_request_body_failure::too_large;
        if (failure == http_request_body_failure::buffer_limit) {
            stream_runtime->discard_body();
        }
        stream_runtime->mark_body_failure(failure);
        (void)body_chunk.take_credit();
        if (stream_runtime->response_committed()) {
            reset_event_stream(stream_id, http2_error_code::cancel);
            return;
        }
        wake_writer();
        if (auto* signal = stream_runtime->signal()) {
            signal->wake();
        } else if (!admit_stream(stream_id)) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
        }
        return;
    }
    if (streaming_body) {
        auto* signal = stream_runtime->signal();
        if (signal == nullptr) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
            return;
        }
        signal->wake();
    } else {
        (void)body_chunk.take_credit();
        wake_writer();
    }
}

void http2_sans_io_session_engine::on_tunnel_data(http2_tunnel_data_event& tunnel_data) {
    const auto& options = session_.options();
    const auto stream_id = tunnel_data.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* signal = stream_runtime != nullptr ? stream_runtime->signal() : nullptr;
    if (stream_runtime == nullptr || signal == nullptr) {
        if (tunnel_data.take_credit().valid()) {
            wake_writer();
        }
        return;
    }
    if (!stream_runtime->route_selected() || stream_runtime->body_mode() != request_body_mode::stream) {
        // Only a CONNECT that no tunnel route will accept lacks a streaming body:
        // its optimistic DATA is never consumed, so return the credit at once.
        (void)tunnel_data.take_credit();
        wake_writer();
        return;
    }
    if (stream_runtime->body_queue().enqueue_bounded(tunnel_data.bytes(),
            tunnel_data.take_credit(), options.server_.max_buffered_body_bytes_) !=
        http2_body_queue_store_status::stored) {
        reset_event_stream(stream_id, http2_error_code::cancel);
        return;
    }
    signal->wake();
}

void http2_sans_io_session_engine::on_tunnel_end(const http2_tunnel_end_event& tunnel_end) noexcept {
    if (auto* signal = stream_runtimes_.signal_for(tunnel_end.stream_id())) {
        signal->wake();
    }
}

void http2_sans_io_session_engine::on_message_end(const http2_message_end_event& message_end) {
    const auto stream_id = message_end.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    if (stream_runtime == nullptr) {
        if (!connection_.stream_aborted(stream_id)) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
        }
        return;
    }
    stream_runtimes_.mark_request_body_closed(stream_id);
    if (stream_runtime->trailers().append_received(message_end.trailers()).has_value()) {
        throw std::logic_error("HTTP/2 decoder published invalid request trailers");
    }
    if (stream_runtime->route_selected() && stream_runtime->body_mode() == request_body_mode::stream) {
        // Before admission: the handler may start inline and read at once.
        stream_runtime->body_queue().mark_ended();
    }
    if (auto* signal = stream_runtime->signal()) {
        signal->wake();
    } else if (!admit_stream(stream_id)) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
    }
}

void http2_sans_io_session_engine::on_stream_closed(const http2_stream_closed_event& stream_closed) noexcept {
    const auto stream_id = stream_closed.stream_id();
    if (has_pushed_streams_) {
        stream_runtimes_.for_each([this, stream_id](http2_sans_io_stream_runtime& child_value) {
            if (child_value.push_parent() == stream_id) {
                reset_stream_no_throw(child_value.stream_id(), http2_error_code::cancel);
                if (auto* child_signal = child_value.signal()) {
                    child_signal->wake();
                }
            }
        });
    }
    output_budget_.release_and_reconcile(stream_id, connection_);
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* signal = stream_runtime != nullptr ? stream_runtime->signal() : nullptr;
    if (signal != nullptr) {
        if (connection_.stream_aborted(stream_id)) {
            output_budget_.wake();
            signal->stop();
        } else {
            signal->wake();
        }
    } else {
        remove_stream_runtime(stream_id);
    }
}

void http2_sans_io_session_engine::drain_events() {
    for (;;) {
        auto event = connection_.next_event();
        if (!event.has_value()) {
            break;
        }
        if (auto* request_head = event->request_head()) {
            on_message_head(*request_head);
        } else if (auto* body_chunk = event->message_body_chunk()) {
            on_body_chunk(*body_chunk);
        } else if (auto* tunnel_data = event->tunnel_data()) {
            on_tunnel_data(*tunnel_data);
        } else if (const auto* tunnel_end = event->tunnel_end()) {
            on_tunnel_end(*tunnel_end);
        } else if (const auto* message_end = event->message_end()) {
            on_message_end(*message_end);
        } else if (const auto* stream_closed = event->stream_closed()) {
            on_stream_closed(*stream_closed);
        } else if (const auto* update = event->priority_update(); update != nullptr) {
            if (auto* runtime = stream_runtimes_.find(static_cast<std::uint32_t>(update->element_id_))) {
                runtime->reprioritize(update->fields_.request_priority());
            }
        }
    }
    // SETTINGS and WINDOW_UPDATE can restore DATA capacity without publishing
    // an application event; producers must retry their caller-owned suffixes.
    stream_runtimes_.for_each([](http2_sans_io_stream_runtime& runtime) {
        if (auto* signal = runtime.signal(); signal != nullptr && signal->waiting_output()) {
            signal->wake();
        }
    });
}

http2_feed_result http2_sans_io_session_engine::feed_and_drain(std::string_view bytes_value) {
    // Once per read, outside the retry loop below, which feeds the same bytes again.
    if (const auto observer = session_.options().http2_input_) {
        observer(server_observation_access::http2_input(base_services_.get_conn_info().connection_id(),
            std::as_bytes(std::span(bytes_value.data(), bytes_value.size()))));
    }
    for (;;) {
        const auto result_value = connection_.feed(bytes_value);
        if (result_value == http2_feed_result::protocol_failure) {
            base_services_.set_connection_close_reason(connection_close_reason::protocol_error);
        }
        drain_events();
        if (result_value != http2_feed_result::events_pending) {
            return result_value;
        }
    }
}

task<void> http2_sans_io_session_engine::finish() {
    lifecycle_.begin_stopping();
    wake_writer();
    while (active_handler_tasks_ != 0) {
        co_await handler_finished_.wait();
    }
    wake_writer();
    while (lifecycle_.writer_join_pending()) {
        co_await writer_finished_.wait();
    }
    lifecycle_.rethrow_writer_failure();
}

namespace {

template <typename stream_type>
task<void> run_http2_sans_io_writer(stream_type& stream, http2_sans_io_session_engine& engine) {
    std::string write_scratch;
    for (;;) {
        while (engine.wants_write()) {
            engine.take_output(write_scratch);
            if (engine.write_failed()) {
                continue;
            }

            std::error_code write_error;
            {
                // The reader keeps the entry in its own phase (readiness,
                // awaiting a stream, active streams, or long_lived for an
                // RFC 8441 tunnel); this clock lets write_timeout close a peer
                // that stopped reading regardless. Every transfer renews it.
                const connection_scanner::entry::write_scope write_stall_clock(engine.scanner_entry());
                const auto write_result = co_await connection_gather_write(
                    stream, write_scratch, {}, engine.scanner_entry());
                write_error = write_result.error_;
            }
            if (write_error) {
                engine.writer_write_failed(write_error);
                continue;
            }
            engine.output_write_completed();
            // Leave room for the frame header and string capacity growth, but keep retention bounded.
            http_clear_string_retaining_small(
                write_scratch, http2_data_output_credit_slots * http2_data_output_credit_bytes + 4096);
        }
        if (engine.writer_should_exit()) {
            co_return;
        }
        co_await engine.wait_for_write();
    }
}

template <typename stream_type>
task<http_connection_close> run_http2_sans_io_session_impl(stream_type& stream, asio::ip::tcp::socket& socket,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    auto executor = asio::any_io_executor(stream.get_executor());
    http2_sans_io_session_engine engine(executor, socket, routes_value, std::move(session_value));

    engine.writer_submitting();
    try {
        ruvia::async_start(run_http2_sans_io_writer(stream, engine),
            asio::bind_executor(executor, [&engine](std::exception_ptr exception) noexcept { engine.writer_completed(exception); }));
    } catch (...) {
        engine.writer_launch_failed();
        engine.terminate(std::make_error_code(std::errc::operation_canceled));
        throw;
    }

    std::error_code reader_terminal_error;
    std::exception_ptr reader_failure;
    try {
        engine.drain_events();
        if (!engine.connection_failed() && !initial_bytes.empty()) {
            (void)engine.feed_and_drain(initial_bytes);
        }
        engine.wake_writer();

        if (!engine.connection_failed() && !engine.terminated()) {
            std::array<char, 4096> read_buffer;
            for (;;) {
                engine.set_inactivity_phase();
                auto read_completion =
                    co_await ruvia::async_asio<std::size_t>([&stream, &read_buffer](auto handler) mutable {
                        stream.async_read_some(asio::buffer(read_buffer), std::move(handler));
                    });
                const auto error = read_completion.error_code();
                const auto bytes_read = read_completion.result();
                if (error || bytes_read == 0) {
                    reader_terminal_error =
                        error ? error
                              : std::make_error_code(std::errc::connection_reset);
                    if (reader_terminal_error == asio::error::eof) {
                        engine.peer_input_ended();
                    }
                    break;
                }
                engine.touch_activity();
                const auto result_value =
                    engine.feed_and_drain(std::string_view(read_buffer.data(), bytes_read));
                engine.wake_writer();
                if (result_value == http2_feed_result::protocol_failure || engine.write_failed()) {
                    if (result_value == http2_feed_result::protocol_failure) {
                        reader_terminal_error = std::make_error_code(std::errc::protocol_error);
                    }
                    break;
                }
            }
        }
    } catch (...) {
        reader_failure = std::current_exception();
        reader_terminal_error = std::make_error_code(std::errc::operation_canceled);
    }

    if (!engine.terminated()) {
        if (!reader_terminal_error) {
            reader_terminal_error = engine.connection_failed() ? std::make_error_code(std::errc::protocol_error)
                                                               : std::make_error_code(std::errc::connection_aborted);
        }
        engine.terminate(reader_terminal_error);
    }

    std::exception_ptr finish_failure;
    try {
        co_await engine.finish();
    } catch (...) {
        finish_failure = std::current_exception();
    }
    if (reader_failure != nullptr) {
        std::rethrow_exception(reader_failure);
    }
    if (finish_failure != nullptr) {
        std::rethrow_exception(finish_failure);
    }
    // A failed write leaves nothing to end gracefully. A clean peer end (FIN,
    // or TLS close_notify) earns this side's final GOAWAY and close alert. A
    // completed drain (request limit, or a timeout's drain at its
    // bound) or a connection error ended the session with GOAWAY while peer
    // frames may still arrive: close lingering (RFC 9113 §9.1, RFC 9112 §9.6).
    // Transport errors close at once, and so does a stopping worker, whatever
    // this mode.
    if (engine.write_failed()) {
        co_return http_connection_close::abort;
    }
    if (reader_terminal_error == asio::error::eof) {
        co_return http_connection_close::notify;
    }
    co_return engine.drain_finished() || engine.connection_failed() ? http_connection_close::linger
                                                                    : http_connection_close::abort;
}

}  // namespace

task<http_connection_close> run_http2_sans_io_session(inbound_socket_stream& stream, const route_table& routes_value,
    http2_sans_io_session_context session_value, std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream.lowest_layer(), routes_value, std::move(session_value), initial_bytes);
}

task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream.lowest_layer(), routes_value, std::move(session_value), initial_bytes);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] http2_stream_data_write_status classify_aborted_write(
    const multiplexed_stream_signal* signal, const bool* aborted) noexcept {
    if (aborted != nullptr && *aborted) {
        return http2_stream_data_write_status::local_aborted;
    }
    if (signal != nullptr && signal->terminated()) {
        return http2_stream_data_write_status::session_terminated;
    }
    return http2_stream_data_write_status::peer_closed;
}

}  // namespace

task<http2_send_window_wait_status> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    multiplexed_stream_signal* signal, const bool* aborted) {
    // Marks the whole wait, across wakes that find no capacity, so the
    // session's write_timeout check sees one continuous stall.
    std::optional<multiplexed_stream_signal::output_wait_guard> wait_guard;
    for (;;) {
        if ((aborted != nullptr && *aborted) ||
            connection.stream_aborted(stream_id) ||
            signal == nullptr || signal->terminated()) {
            co_return http2_send_window_wait_status::aborted;
        }
        const auto window = connection.send_window_state(stream_id);
        if (!window) {
            co_return http2_send_window_wait_status::aborted;
        }
        if (window->available_ != 0) {
            co_return http2_send_window_wait_status::ready;
        }
        if (!wait_guard.has_value()) {
            wait_guard.emplace(*signal);
        }
        co_await signal->wait();
    }
}

http2_stream_data_submit_result submit_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    worker_signal& write_signal, http2_data_output_budget& output_budget,
    std::string_view bytes, http2_end_stream end_stream) {
    struct submission_completion final {
        http2_data_output_budget& budget_;
        worker_signal& write_signal_;
        std::uint32_t stream_id_;
        bool release_credit_;
        ~submission_completion() noexcept {
            write_signal_.notify();
            if (release_credit_) {
                budget_.release(stream_id_);
            }
        }
    } completion{output_budget, write_signal, stream_id, !bytes.empty()};
    try {
        const auto progress = connection.submit_data(
            stream_id, bytes, end_stream, http2_data_output_credit_bytes);
        output_budget.note_data_submitted(stream_id, progress.consumed_bytes_);
        completion.release_credit_ = !bytes.empty() && progress.consumed_bytes_ == 0;
        if (progress.status_ == http2_data_submit_status::invalid_message) {
            return {.failure_ = http2_stream_data_write_status::content_length_exceeded};
        }
        return {.progress_ = progress};
    } catch (const std::logic_error&) {
        return {.failure_ = http2_stream_data_write_status::invalid_state};
    }
}

task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    multiplexed_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted) {
    // The caller owns the suffix until consumed. Each reservation covers only
    // encoded payload and remains charged through socket write completion.
    for (;;) {
        if (aborted != nullptr && *aborted) {
            co_return http2_stream_data_write_status::local_aborted;
        }
        if (signal != nullptr && signal->terminated()) {
            co_return http2_stream_data_write_status::session_terminated;
        }
        if (signal == nullptr) {
            co_return http2_stream_data_write_status::peer_closed;
        }
        // Match the protocol engine's nghttp2 backpressure contract, including
        // empty END_STREAM DATA.
        if (!bytes.empty() || http2_ends_stream(end_stream)) {
            const auto window = connection.send_window_state(stream_id);
            if (!window || connection.stream_aborted(stream_id)) {
                co_return classify_aborted_write(signal, aborted);
            }
            if (window->available_ == 0) {
                if (co_await await_http2_send_window(connection, stream_id, signal, aborted) ==
                    http2_send_window_wait_status::aborted) {
                    co_return classify_aborted_write(signal, aborted);
                }
            }
        }
        if (!bytes.empty()) {
            if (!output_budget.try_acquire(stream_id)) {
                if (!(co_await output_budget.acquire(stream_id, *signal, connection, aborted))) {
                    co_return classify_aborted_write(signal, aborted);
                }
                if ((aborted != nullptr && *aborted) || signal->terminated() ||
                    connection.stream_aborted(stream_id)) {
                    output_budget.release(stream_id);
                    co_return classify_aborted_write(signal, aborted);
                }
            }
        }
        const auto submitted = submit_http2_stream_data(
            connection, stream_id, write_signal, output_budget, bytes, end_stream);
        if (submitted.failure_) {
            co_return *submitted.failure_;
        }
        const auto& result_value = submitted.progress_;
        bytes.remove_prefix(result_value.consumed_bytes_);
        if (result_value.status_ == http2_data_submit_status::complete) {
            co_return http2_stream_data_write_status::completed;
        }
        if (result_value.status_ == http2_data_submit_status::closed) {
            co_return http2_stream_data_write_status::peer_closed;
        }
        if (result_value.status_ == http2_data_submit_status::expectation_pending) {
            co_await signal->wait();
        }
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] bool data_write_failed(http2_stream_data_write_status status) noexcept {
    switch (status) {
        case http2_stream_data_write_status::completed:
        case http2_stream_data_write_status::local_aborted:
        case http2_stream_data_write_status::session_terminated:
        case http2_stream_data_write_status::peer_closed:
            return false;
        case http2_stream_data_write_status::invalid_state:
        case http2_stream_data_write_status::content_length_exceeded:
            return true;
    }
    return true;
}

}  // namespace

http2_buffered_response_writer::http2_buffered_response_writer(http2_sans_io_session_engine& session,
    ruvia::http2_connection& connection, http2_sans_io_stream_runtime_table& stream_runtimes,
    worker_signal& write_signal, http2_data_output_budget& output_budget, response_file_io file_io) noexcept
    : session_(session),
      connection_(connection),
      stream_runtimes_(stream_runtimes),
      write_signal_(write_signal),
      output_budget_(output_budget),
      file_io_(std::move(file_io)),
      file_reads_(file_io_.worker_, http2_file_read_slots) {}
void http2_buffered_response_writer::wake_writer() noexcept {
    write_signal_.notify();
}

bool http2_buffered_response_writer::stream_ended(std::uint32_t stream_id) const noexcept {
    if (connection_.stream_aborted(stream_id)) {
        return true;
    }
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    return signal != nullptr && signal->terminated();
}

task<http2_stream_data_write_status> http2_buffered_response_writer::write_data(
    std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream) {
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    return write_http2_stream_data(
        connection_, stream_id, signal, write_signal_, output_budget_, chunk, end_stream);
}

task<multiplexed_buffered_response_write_result> http2_buffered_response_writer::write_file_body(
    std::uint32_t stream_id, response_file_source& source, http_status_code committed_status) {
    const auto fail_after_commit = [this, stream_id]() noexcept {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
    };
    wake_writer();
    for (;;) {
        std::optional<multiplexed_file_read_limiter::slot> slot;
        std::size_t max_bytes = response_file_read_ahead_bytes;
        if (source.file_bytes_pending()) {
            // Read only what the peer has granted: a stream without send
            // window holds neither a read buffer nor a pool task.
            auto* runtime = stream_runtimes_.find(stream_id);
            if (co_await await_http2_send_window(
                    connection_, stream_id, runtime != nullptr ? runtime->signal() : nullptr) ==
                http2_send_window_wait_status::aborted) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            auto acquired = co_await file_reads_.acquire(
                [this, stream_id]() noexcept { return stream_ended(stream_id); });
            if (!acquired) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            slot.emplace(std::move(*acquired));
            if (stream_ended(stream_id)) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            const auto window = connection_.send_window_state(stream_id);
            if (!window) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            if (window->available_ == 0) {
                // SETTINGS shrank the window while this stream waited for a
                // slot: give the slot back and wait again.
                continue;
            }
            max_bytes = window->available_;
        }
        const auto chunk = co_await source.next(max_bytes);
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            // The committed Content-Length can no longer be honoured.
            fail_after_commit();
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status, .file_failure_ = *source.failure()};
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            if (co_await write_data(stream_id, {}, http2_end_stream::end_stream) != http2_stream_data_write_status::completed) {
                fail_after_commit();
            }
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
        const auto bytes = chunk.bytes();
        if (connection_.stream_aborted(stream_id)) {
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
        const auto result_value = co_await write_data(stream_id, bytes, http2_end_stream::keep_open);
        source.release_buffer();
        if (result_value != http2_stream_data_write_status::completed) {
            if (data_write_failed(result_value)) {
                fail_after_commit();
            }
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
    }
}

task<multiplexed_buffered_response_write_result> http2_buffered_response_writer::write(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (stream_ended(stream_id)) {
        co_return multiplexed_buffered_response_write_result{};
    }

    std::optional<response_file_source> source;
    // Open and validate the file before HEADERS, without reading it: a file
    // that cannot be served still gets an error response, and a stream
    // without window holds no buffer. HEAD and status-suppressed bodies do not
    // transfer a representation.
    if (!write_plan.body_plan().body_suppressed() &&
        (response.file_body() || response.has_multipart_file_body())) {
        if (const auto open_failure = co_await file_reads_.open_file_body(
                source, response, file_io_,
                [this, stream_id]() noexcept { return stream_ended(stream_id); })) {
            co_return *open_failure;
        }
    }

    const auto head_result =
        connection_.submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        const auto error = std::get<http2_response_head_submit_error>(head_result);
        if (error == http2_response_head_submit_error::closed) {
            co_return multiplexed_buffered_response_write_result{};
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        session_.reset_stream_no_throw(stream_id,
            error == http2_response_head_submit_error::peer_stream_limit_reached
                ? http2_error_code::cancel
                : http2_error_code::internal_error);
        co_return multiplexed_buffered_response_write_result{};
    }

    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (runtime != nullptr) {
        runtime->mark_response_committed();
    }
    const auto committed_status = submitted_head->body_plan().response_status();
    const auto fail_after_commit = [this, stream_id, committed_status]() noexcept -> multiplexed_buffered_response_write_result {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        return {.committed_status_ = committed_status};
    };
    if (!submitted_head->send_body()) {
        wake_writer();
        co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
    }

    if (source) {
        co_return co_await write_file_body(stream_id, *source, committed_status);
    }

    // Keep the first DATA submission synchronous: a response fitting the
    // current window and one output credit needs no DATA coroutine.
    auto body = response.body_bytes();
    if (!body.empty() && signal != nullptr && !signal->terminated() && !connection_.stream_aborted(stream_id)) {
        const auto window = connection_.send_window_state(stream_id);
        if (window && window->available_ != 0 && output_budget_.try_acquire(stream_id)) {
            const auto submitted = submit_http2_stream_data(
                connection_, stream_id, write_signal_, output_budget_, body, http2_end_stream::end_stream);
            if (submitted.failure_) {
                co_return fail_after_commit();
            }
            body.remove_prefix(submitted.progress_.consumed_bytes_);
            if (submitted.progress_.status_ == http2_data_submit_status::complete ||
                submitted.progress_.status_ == http2_data_submit_status::closed) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
        } else {
            wake_writer();
        }
    } else {
        wake_writer();
    }
    if (!body.empty()) {
        const auto result_value = co_await write_data(stream_id, body, http2_end_stream::end_stream);
        if (data_write_failed(result_value)) {
            co_return fail_after_commit();
        }
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
}

}  // namespace ruvia::detail
