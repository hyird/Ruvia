#pragma once

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/cancellation_signal.hpp>

#include "borrowed_text.h"
#include "byte_view.h"
#include "connection_scanner.h"
#include "context.h"
#include "context_access.h"
#include "event_loop.h"
#include "http_error.h"
#include "http_header.h"
#include "http_multiplexed_stream.h"
#include "http_priority.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_request_admission.h"
#include "http_request_body_failure.h"
#include "http_response.h"
#include "http_response_stream.h"
#include "http_server.h"
#include "inbound_buffer.h"
#include "io_operation.h"
#include "middleware.h"
#include "response_coding_policy.h"
#include "response_file.h"
#include "response_stream_state.h"
#include "route_stream_dispatch.h"
#include "route_table.h"
#include "server_options.h"
#include "server_socket_stream.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "transport_stream_end.h"
#include "websocket_handshake.h"
#include "worker_ref.h"

// Server stream machinery shared by HTTP/2 and HTTP/3 sessions: per-stream
// deadlines and wake-ups, session termination and drain, the per-stream
// request state with its body queue and the DATA chunk store policy, the
// bounded file reader, the CRTP bases for response sinks, request body readers
// and tunnel transports over one multiplexed stream, and the endpoint tails
// both dispatch spines end in.

namespace ruvia::detail {

// The session already owns and counts the dispatch task. Defer before
// materializing request views so peer events can advance without borrowing them.
class multiplexed_dispatch_turn final {
public:
    explicit multiplexed_dispatch_turn(const worker_ref& worker_value) :
        worker_(worker_value) {
        if (!worker_.valid()) {
            throw std::invalid_argument("multiplexed dispatch requires a worker");
        }
    }
    multiplexed_dispatch_turn(worker_ref&&) = delete;
    multiplexed_dispatch_turn(const worker_ref&&) = delete;
    multiplexed_dispatch_turn(const multiplexed_dispatch_turn&) = delete;
    multiplexed_dispatch_turn& operator=(const multiplexed_dispatch_turn&) = delete;
    multiplexed_dispatch_turn(multiplexed_dispatch_turn&&) = delete;
    multiplexed_dispatch_turn& operator=(multiplexed_dispatch_turn&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() {
        if (std::exchange(started_, true)) {
            throw std::logic_error("multiplexed dispatch turn already awaited");
        }
        if (!worker_.is_current()) {
            throw std::logic_error("multiplexed dispatch requires its worker");
        }
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}

private:
    const worker_ref& worker_;
    bool started_{false};
};

// Inactivity belongs to the connection; request bodies and output waits have
// independent per-stream deadlines.
[[nodiscard]] inline connection_scanner::phase multiplexed_inactivity_phase(bool peer_settings_received, bool header_block_in_progress, bool stream_active, bool tunnel_active) noexcept {
    if (!peer_settings_received || header_block_in_progress) {
        return connection_scanner::phase::reading_initial;
    }
    if (tunnel_active) {
        return connection_scanner::phase::long_lived;
    }
    return stream_active ? connection_scanner::phase::idle : connection_scanner::phase::awaiting_stream;
}

class multiplexed_request_body_deadline final {
public:
    void open(std::int64_t now_ms) noexcept {
        open_ = true;
        opened_ms_ = now_ms;
        progress_ms_ = now_ms;
    }
    void progress(std::int64_t now_ms) noexcept {
        progress_ms_ = now_ms;
    }
    void close() noexcept {
        open_ = false;
    }
    [[nodiscard]] bool overdue(std::int64_t now_ms, const std::optional<timeout_duration>& inactivity, const std::optional<timeout_duration>& completion) const noexcept {
        return open_ && ((inactivity.has_value() && now_ms - progress_ms_ >= inactivity->value().count()) || (completion.has_value() && now_ms - opened_ms_ >= completion->value().count()));
    }

private:
    std::int64_t opened_ms_{0};
    std::int64_t progress_ms_{0};
    bool open_{false};
};

// A timeout drain must end the connection at its bound; an ordinary drain only
// finalizes GOAWAY. Protocol owners keep their own wake/close ordering.
class multiplexed_drain_timer final {
public:
    void request_timeout() noexcept {
        timeout_drain_ = true;
    }
    [[nodiscard]] bool needed(bool terminated, bool final_boundary) const noexcept {
        return !timer_.registered() && !terminated && (!final_boundary || timeout_drain_);
    }
    void mark_expired() noexcept {
        expired_ = timeout_drain_;
    }
    [[nodiscard]] bool expired() const noexcept {
        return expired_;
    }
    void cancel() noexcept {
        timer_.cancel();
    }

    template <typename duration_type, typename expired_type, typename failure_type>
    void arm(const worker_ref& worker_value, duration_type bound, expired_type on_expired, failure_type on_failure) noexcept {
        try {
            const bool scheduled = timer_.schedule(worker_value, deadline_after(bound), [on_expired](worker_timer_outcome outcome) {
                // Cancellation belongs to worker shutdown, which closes the session.
                if (outcome == worker_timer_outcome::expired) {
                    on_expired();
                }
            });
            if (!scheduled) {
                on_expired();
            }
        } catch (...) {
            on_failure();
        }
    }

private:
    bool timeout_drain_{false};
    bool expired_{false};
    // Unregister before the state captured by its completion can retire.
    worker_timer_registration timer_;
};

// Connection-owned terminal state shared by every Web capability derived from
// one multiplexed HTTP/2 or HTTP/3 session. Stream waits that must end on
// termination combine their own stop token with token().
class multiplexed_session_termination final {
public:
    multiplexed_session_termination() noexcept = default;
    multiplexed_session_termination(const multiplexed_session_termination&) = delete;
    multiplexed_session_termination& operator=(const multiplexed_session_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Allocated on first use; already stopped once the session terminated.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

// The one asynchronous wake primitive of a dispatched multiplexed stream. Body
// readers and writers waiting for send window or output budget share it and
// always re-check their own readiness after a wake.
class multiplexed_stream_signal final {
public:
    class output_wait_guard final {
    public:
        explicit output_wait_guard(multiplexed_stream_signal& signal) noexcept :
            signal_(signal) {
            signal_.set_waiting_output(true);
        }
        ~output_wait_guard() noexcept {
            signal_.set_waiting_output(false);
        }
        output_wait_guard(const output_wait_guard&) = delete;
        output_wait_guard& operator=(const output_wait_guard&) = delete;

    private:
        multiplexed_stream_signal& signal_;
    };

    // Spans one write operation (one DATA or datagram submission call) across
    // all of its output waits: write_completion_timeout_ bounds the call from
    // its start, and progress between waits renews only the per-wait
    // write_timeout_ clock. Operations do not nest.
    class output_operation_guard final {
    public:
        explicit output_operation_guard(multiplexed_stream_signal& signal) noexcept :
            signal_(signal) {
            signal_.output_operation_started_ms_ = steady_stamp_ms();
        }
        ~output_operation_guard() noexcept {
            signal_.output_operation_started_ms_.reset();
        }
        output_operation_guard(const output_operation_guard&) = delete;
        output_operation_guard& operator=(const output_operation_guard&) = delete;

    private:
        multiplexed_stream_signal& signal_;
    };

    multiplexed_stream_signal(const worker_ref& worker_value, multiplexed_session_termination& termination) :
        signal_(worker_value),
        termination_(termination) {}
    multiplexed_stream_signal(worker_ref&&, multiplexed_session_termination&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    // The stream ended abnormally (peer reset, local reset): every wait ends.
    void stop() noexcept {
        stopped_ = true;
        if (stop_source_) {
            stop_source_->request_stop();
        }
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_ || termination_.terminated();
    }

    [[nodiscard]] stop_token terminal_stop_token() {
        if (!stop_source_) {
            stop_source_.emplace();
        }
        if (stopped()) {
            stop_source_->request_stop();
        }
        return stop_source_->token();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminal_error() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] multiplexed_session_termination& termination() & noexcept RUVIA_LIFETIMEBOUND {
        return termination_;
    }
    multiplexed_session_termination& termination() && = delete;

    // Spans one whole wait for peer window / acknowledgements to free output
    // capacity; the connection scanner times it with output_wait_expired().
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, in the scanner's clock, when the wait has been
    // observed for `inactivity` (write_timeout_) or its operation has run for
    // `completion` (write_completion_timeout_). The first observation starts
    // the wait clock; a wait outside any operation is its own operation.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, const std::optional<timeout_duration>& inactivity, const std::optional<timeout_duration>& completion) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        const auto operation_started_ms = output_operation_started_ms_.value_or(*output_wait_observed_ms_);
        output_wait_expired_ = (inactivity.has_value() && now_ms - *output_wait_observed_ms_ >= inactivity->value().count()) || (completion.has_value() && now_ms - operation_started_ms >= completion->value().count());
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_ref& worker() const&& = delete;

private:
    worker_signal signal_;
    multiplexed_session_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
    std::optional<std::int64_t> output_operation_started_ms_;
};

enum class multiplexed_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Request content of one multiplexed stream that already crossed the protocol
// core's event boundary and waits for a suspended route handler: a streamed
// body or tunnel bytes. The core owns framing and flow-control debt; this
// queue owns the bytes, charged to the connection's inbound budget, and the
// receive credit they hold until the handler consumed them. Pending DATA is
// concatenated so each frame is copied once; pop() swaps the pending buffer
// aside so a later enqueue cannot invalidate the borrowed view.
class multiplexed_request_body_queue final {
public:
    explicit multiplexed_request_body_queue(inbound_buffer_account* inbound = nullptr) noexcept :
        queued_reservation_(inbound),
        active_reservation_(inbound) {}

    // Never too_large: the total limit belongs to the stream. Credit moves in
    // only after the append succeeded; otherwise it stays with the caller,
    // whose token returns it.
    [[nodiscard]] multiplexed_body_store_status enqueue_bounded(std::string_view data, http_received_data_credit&& credit, std::size_t backlog_limit) {
        const auto queued_bytes = queued_chunk_.size();
        if (queued_bytes > backlog_limit || data.size() > backlog_limit - queued_bytes) {
            return multiplexed_body_store_status::backlog_overflow;
        }
        if (data.empty()) {
            return multiplexed_body_store_status::stored;
        }
        try {
            inbound_append(queued_chunk_, queued_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return multiplexed_body_store_status::buffer_limit;
        }
        if (credit.valid()) {
            if (queued_credit_.has_value()) {
                queued_credit_->merge(std::move(credit));
            } else {
                queued_credit_.emplace(std::move(credit));
            }
        }
        return multiplexed_body_store_status::stored;
    }

    // The consumer finished with the active view: its storage (beyond a small
    // retained capacity) and its credit return. True when credit returned, so
    // the caller can let the writer send it.
    bool release_active() noexcept {
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        return returned;
    }

    // The request content ended: the peer's end of stream was delivered, or
    // there never was content. The core's own receive state can run ahead of
    // event delivery within one input batch, so readers end on this flag.
    void mark_ended() noexcept {
        ended_ = true;
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

    // Every queued byte as one view, valid until the next pop() or
    // release_active(); it keeps its credit until then.
    [[nodiscard]] std::string_view pop() & {
        (void)release_active();
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

    // Every queued byte as storage the consumer owns from here on; its credit
    // returns now.
    [[nodiscard]] std::string take() noexcept {
        std::string taken;
        taken.swap(queued_chunk_);
        queued_reservation_.clear();
        queued_credit_.reset();
        return taken;
    }

    // Drops the content no consumer has taken and returns every credit. The
    // active chunk stays: the reader's last view borrows it until its next
    // read starts (body_reader::read).
    void discard_unread() noexcept {
        inbound_discard(queued_chunk_, queued_reservation_);
        queued_credit_.reset();
        active_credit_.reset();
    }

    // No consumer is left.
    bool discard() noexcept {
        const bool returned = queued_credit_.has_value() || active_credit_.has_value();
        discard_unread();
        inbound_discard(active_chunk_, active_reservation_);
        return returned;
    }

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<http_received_data_credit> queued_credit_;
    std::optional<http_received_data_credit> active_credit_;
    std::string queued_chunk_;
    inbound_buffer_reservation queued_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    bool ended_{false};
};

// Web-side state every HTTP/2 and HTTP/3 request stream keeps: the route it
// resolved to, the request content as that route consumes it (buffered whole
// and borrowed by the request at dispatch, or queued for a streaming reader
// or tunnel), the trailers, the priority, the body failure its response
// reports, the request-body deadline, and the wake/stop signal of its
// dispatched handler. Each protocol's stream type adds its request head and
// its own lifecycle flags.
class multiplexed_request_stream_state {
public:
    multiplexed_request_stream_state(const multiplexed_request_stream_state&) = delete;
    multiplexed_request_stream_state& operator=(const multiplexed_request_stream_state&) = delete;

    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
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

    // Once per stream: false when its route was already selected.
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

    // One DATA chunk of the request body. A buffered body takes no credit: it
    // returns as the content is stored. A streamed body queues the bytes with
    // their credit until the reader takes them. Credit not moved from stays
    // with the caller, whose token returns it.
    [[nodiscard]] multiplexed_body_store_status store_body(std::string_view data, http_received_data_credit&& credit, protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return multiplexed_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream) {
            if (const auto status = body_queue_.enqueue_bounded(data, std::move(credit), backlog_limit); status != multiplexed_body_store_status::stored) {
                return status;
            }
        } else {
            try {
                inbound_append(buffered_body_, buffered_reservation_, data);
            } catch (const inbound_buffer_limit_error&) {
                return multiplexed_body_store_status::buffer_limit;
            }
        }
        received_bytes_ += data.size();
        return multiplexed_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return buffered_body_;
    }
    std::string_view buffered_body() const&& = delete;

    [[nodiscard]] multiplexed_request_body_queue& body_queue() & noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    multiplexed_request_body_queue& body_queue() && = delete;
    [[nodiscard]] const multiplexed_request_body_queue& body_queue() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_queue_;
    }
    const multiplexed_request_body_queue& body_queue() const&& = delete;

    // The inbound budget is exhausted: the content no handler was given goes
    // back to it and every credit returns. What a handler borrows stays -- a
    // dispatched buffered body, the reader's last chunk.
    void discard_unread_body() noexcept {
        if (!dispatched()) {
            inbound_discard(buffered_body_, buffered_reservation_);
        }
        body_queue_.discard_unread();
    }

    // No handler borrows the content any more (it finished, or never ran):
    // the storage goes back to the inbound budget, and credit for content
    // received but never read returns so a stopped reader cannot strand
    // connection-level credit. True when any credit returned.
    bool discard_body() noexcept {
        inbound_discard(buffered_body_, buffered_reservation_);
        return body_queue_.discard();
    }

    // Null when the route is not selected yet or the stream was dispatched.
    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (!route_selected_ || signal_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        return &*signal_;
    }
    multiplexed_stream_signal* begin_dispatch(worker_ref&&, multiplexed_session_termination&) = delete;
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
    // Output capacity may have returned: a handler waiting for it retries.
    void wake_output_waiter() noexcept {
        if (signal_.has_value() && signal_->waiting_output()) {
            signal_->wake();
        }
    }

    // `now_ms` is the time of the read that carried the request head or DATA.
    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms, const std::optional<timeout_duration>& inactivity, const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    // The session counts its tunnel streams as it marks them.
    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

protected:
    explicit multiplexed_request_stream_state(inbound_buffer_account* inbound) noexcept :
        buffered_reservation_(inbound),
        body_queue_(inbound) {}
    ~multiplexed_request_stream_state() = default;

private:
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    route_resolution resolution_{};
    request_body_mode body_mode_{request_body_mode::buffered};
    std::string buffered_body_;
    inbound_buffer_reservation buffered_reservation_;
    multiplexed_request_body_queue body_queue_;
    std::size_t received_bytes_{0};
    std::optional<multiplexed_stream_signal> signal_;
    std::optional<http_request_body_failure> body_failure_{};
    multiplexed_request_body_deadline request_body_deadline_;
    bool route_selected_{false};
    bool tunnel_{false};
    bool response_committed_{false};
};

// Streams of one multiplexed connection that may open or read a file body on
// the blocking pool at once; the rest wait holding no read buffer and no pool
// task.
inline constexpr std::size_t multiplexed_file_read_slots = 4;

struct multiplexed_buffered_response_write_result final {
    // The final status, once the response head was submitted.
    std::optional<http_status_code> committed_status_{};
    // Why a file body failed. Without committed_status_ nothing of the
    // response was sent and the stream still awaits one.
    std::optional<response_file_failure> file_failure_{};
};

class multiplexed_file_read_limiter final {
public:
    class slot final {
    public:
        slot(slot&& other) noexcept :
            owner_(std::exchange(other.owner_, nullptr)) {}
        ~slot() noexcept {
            if (owner_ != nullptr) {
                auto* const owner = std::exchange(owner_, nullptr);
                --owner->active_;
                owner->released_.notify();
            }
        }

        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;
        slot& operator=(slot&&) = delete;

    private:
        friend class multiplexed_file_read_limiter;

        explicit slot(multiplexed_file_read_limiter& owner) noexcept :
            owner_(&owner) {}

        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(const worker_ref& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept :
        released_(worker_value),
        max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_ref&&, std::size_t = multiplexed_file_read_slots) = delete;
    multiplexed_file_read_limiter(const multiplexed_file_read_limiter&) = delete;
    multiplexed_file_read_limiter& operator=(const multiplexed_file_read_limiter&) = delete;

    // Admission and its return obligation travel as one move-only value.
    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<slot>> acquire(stream_ended_predicate stream_ended) {
        while (active_ >= max_slots_) {
            co_await released_.wait();
            if (stream_ended()) {
                co_return std::nullopt;
            }
        }
        ++active_;
        co_return std::optional<slot>(slot(*this));
    }

    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(std::optional<response_file_source>& source, const http_response& response, const response_file_io& file_io, stream_ended_predicate stream_ended) {
        source.emplace(response, file_io);
        bool opened = false;
        {
            const auto held = co_await acquire(stream_ended);
            if (!held) {
                co_return multiplexed_buffered_response_write_result{};
            }
            opened = co_await source->open_first();
        }
        if (stream_ended()) {
            co_return multiplexed_buffered_response_write_result{};
        }
        if (!opened) {
            co_return multiplexed_buffered_response_write_result{.file_failure_ = *source->failure()};
        }
        co_return std::nullopt;
    }

private:
    worker_signal released_;
    std::size_t max_slots_;
    std::size_t active_{0};
};

// Shared state and lifecycle for HTTP/2 and HTTP/3 streaming response sinks.
// Derived sinks supply the protocol-specific head submission, encoded DATA
// write, terminal finish, and abort check.
template <typename derived_type, typename stream_type>
class multiplexed_response_stream_sink_base {
public:
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && (derived().stream_aborted() || signal_.terminated()));
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    void bind_context(context* context_value) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    task<void> write(std::string_view chunk) {
        throw_if_terminated();
        throw_if_body_failed();
        state_.ensure_body_allowed();
        if (chunk.empty() || !compression_.active()) {
            return write_encoded(chunk);
        }
        return compression_.write(chunk, state_, [this](std::string_view encoded) {
            return write_encoded(encoded);
        });
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        try {
            co_await derived().write_encoded_data(chunk);
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
    }

    task<timer_sleep_result> sleep(timeout_duration duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner{stop_token_value, signal_.termination().token()};
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::sleep_terminated_error_message);
        }
        if (derived().stream_aborted()) {
            throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), derived_type::sleep_reset_error_message);
        }
        co_return result;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        // An empty terminal call is idempotent once the content ended.
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        throw_if_terminated();
        throw_if_body_failed();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!state_.committed()) {
            co_await commit_head(trailer_intent);
        }
        throw_if_body_failed();
        if (state_.ended()) {
            co_return;
        }
        if (!trailer_section.empty()) {
            state_.ensure_trailers_allowed();
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        try {
            if constexpr (std::is_void_v<decltype(derived().finish_stream_response(trailer_section))>) {
                derived().finish_stream_response(trailer_section);
            } else {
                co_await derived().finish_stream_response(trailer_section);
            }
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        state_.mark_ended();
    }

protected:
    multiplexed_response_stream_sink_base(stream_type& stream, multiplexed_stream_signal& signal, http_known_method request_method, worker_codecs& codecs, http_response_coding_selection response_coding, const compression_config* compression, const http_cors_policy* cors) noexcept :
        stream_(stream),
        signal_(signal),
        request_method_(request_method),
        cors_(cors),
        compression_(codecs, response_coding, compression) {}

    void throw_if_terminated() const {
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
        }
    }

    void throw_if_body_failed() {
        if (const auto& body_failure = stream_.body_failure(); body_failure.has_value()) {
            if (state_.committed()) {
                state_.mark_aborted();
            }
            if (*body_failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw make_http_protocol_error(*body_failure);
        }
    }

    stream_type& stream_;
    multiplexed_stream_signal& signal_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    http_known_method request_method_{http_known_method::unknown};
    const http_cors_policy* cors_;
    http_streaming_response_compression compression_;
    operation_interrupt_source output_interrupts_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        throw_if_terminated();
        throw_if_body_failed();
        try {
            if (context_access::output_committed(state_.bound_context())) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            validate_response_stream_status(context_access::request(state_.bound_context()), response);
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(context_access::request(state_.bound_context()), response);
            }
            throw_if_terminated();
            throw_if_body_failed();
            compression_.prepare(context_access::request(state_.bound_context()), response);
            const auto commit_body_plan = plan_http_response_body(request_method_, response.status());
            compression_.activate(commit_body_plan);
            const auto submitted_head = derived().submit_streaming_head(std::move(response), trailer_intent);
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_.mark_response_committed();
            state_.mark_committed(submitted_head);
            derived().wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }
};

// Shared streaming request-body reader loop for HTTP/2 and HTTP/3 streams.
// Derived readers provide protocol-specific body failure, reset check, chunk
// extraction (returning flow-control credit), and end-of-body check.
template <typename derived_type>
class multiplexed_request_body_reader_base {
public:
    // An interrupted read leaves queued chunks and flow-control credit for the
    // next read.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        operation_interrupt interrupt(interrupts_, signal_.worker(), operation, &signal_, &wake_signal, "request body read");
        throw_if_body_failed();
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            interrupt.throw_if_interrupted();
            throw_if_body_failed();
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
            }
            if (signal_.stopped() || derived().stream_aborted()) {
                throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), derived_type::reset_error_message);
            }
            const auto chunk = derived().pop_chunk();
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (derived().body_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

protected:
    multiplexed_request_body_reader_base(multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept :
        signal_(signal),
        continue_output_(continue_output) {}

    multiplexed_stream_signal& signal_;
    http_interim_response_output* continue_output_;
    operation_interrupt_source interrupts_;

private:
    static void wake_signal(void* raw) noexcept {
        static_cast<multiplexed_stream_signal*>(raw)->wake();
    }
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
    void throw_if_body_failed() const {
        if (const auto& failure = derived().body_failure(); failure.has_value()) {
            if (*failure == http_request_body_failure::buffer_limit) {
                throw inbound_buffer_limit_error{};
            }
            throw make_http_protocol_error(*failure);
        }
    }
};

// Shared duplex CONNECT tunnel read loop and WebSocket write disposition
// forwarding for HTTP/2 and HTTP/3 streams.
template <typename derived_type>
class multiplexed_tunnel_transport_base {
public:
    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        return read_more(buffer, []() noexcept {}, asio::cancellation_slot{});
    }

    // A cancellation emitted on `slot` wakes the wait; `check` runs before
    // every wait and may throw to abandon it. Input that has not been
    // consumed stays queued for the next read_more().
    template <typename buffer_type, typename check_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer, check_type check, asio::cancellation_slot slot) {
        const wake_assignment assignment(signal_, slot);
        for (;;) {
            derived().require_live();
            if (derived().consume_tunnel_chunk(buffer)) {
                co_return true;
            }
            if (derived().receive_ended()) {
                co_return false;
            }
            derived().require_live();
            check();
            co_await signal_.wait();
        }
    }

protected:
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept :
        signal_(signal) {}

    // While assigned, a cancellation emitted on the slot wakes this stream's waits.
    class wake_assignment final {
    public:
        wake_assignment(multiplexed_stream_signal& signal, asio::cancellation_slot slot) :
            slot_(slot) {
            if (slot_.is_connected()) {
                slot_.assign([&signal](asio::cancellation_type) noexcept {
                    signal.wake();
                });
            }
        }
        ~wake_assignment() {
            if (slot_.is_connected()) {
                slot_.clear();
            }
        }
        wake_assignment(const wake_assignment&) = delete;
        wake_assignment& operator=(const wake_assignment&) = delete;

    private:
        asio::cancellation_slot slot_;
    };

    multiplexed_stream_signal& signal_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
};

// `request_stream`: the connection's listener is a request stream one, whose
// every request streams its body (a CONNECT is a tunnel request).
[[nodiscard]] inline std::pair<route_resolution, request_body_mode> resolve_multiplexed_stream_route(const route_table& routes, bool request_stream, std::string_view method_token, std::string_view target_path, std::string_view authority, std::string_view protocol) noexcept {
    const auto method = classify_http_method(method_token);
    if (request_stream) {
        return {resolve_request_stream_route(method == http_known_method::connect), request_body_mode::stream};
    }
    const auto path = target_path.substr(0, target_path.find('?'));
    const auto extended_protocol = http_canonical_extended_connect_protocol(protocol);
    auto resolution = routes.resolve(route_request_view{method, method_token, path, authority, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    if (const auto* resolved = resolution.resolved()) {
        body_mode = resolved->route().endpoint().request_body_mode();
        if ((method == http_known_method::connect && resolved->route().endpoint().websocket() != nullptr) || resolved->route().endpoint().tunnel() != nullptr) {
            body_mode = request_body_mode::stream;
        }
    }
    return {std::move(resolution), body_mode};
}

[[nodiscard]] inline task<http_response> respond_http_request_body_failure(const route_table& routes, const http_request& request, http_request_body_failure failure, const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(routes, request, http_request_rejection::error(make_http_protocol_error(failure)), services);
}

// The error response of a WebSocket handshake that failed validation. A
// generated rejection does not inherit the negotiated coding promise.
[[nodiscard]] inline task<http_response> respond_websocket_handshake_failure(const route_table& routes, const http_request& request, websocket_handshake_error failure, const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    const auto protocol_error = make_http_protocol_error(failure);
    auto response = co_await routes.respond_error(request, http_error_info({.status_ = protocol_error.status(), .code_ = protocol_error.code(), .message_ = std::string_view(protocol_error.what())}), services);
    apply_websocket_handshake_error_headers(failure, response);
    co_return response;
}

// What a request head decides before any content arrives, alike for HTTP/2
// and HTTP/3.
struct multiplexed_request_head_plan final {
    // Submit 100 Continue now.
    bool send_continue_{false};
    // Dispatch now instead of after the content.
    bool admit_{false};
};

// Same policy as HTTP/1. `resolved_route` names a resolved route of a
// request that is not CONNECT; its declared length past the body limit is
// recorded as the stream's body failure. 100 Continue is sent when the server
// starts consuming the content -- now for a buffered route, at the first read
// for a streaming one -- and never for a request answered from its head alone
// (RFC 9110 §10.1.1): an unresolved, CONNECT, oversized or
// rejected-expectation request, or one `dispatch_with_head` names, waiting
// for content the client may withhold would stall it. A streaming route
// whose body is still open is dispatched now to read it.
[[nodiscard]] inline multiplexed_request_head_plan plan_multiplexed_request_head(multiplexed_request_stream_state& stream, const http_request_admission_policy& policy, http_server_expectation_plan expectation, bool resolved_route, std::optional<std::uint64_t> declared_content_length, bool body_open, bool dispatch_with_head) noexcept {
    bool body_limit_exceeded = false;
    if (resolved_route && declared_content_length.has_value()) {
        if (const auto failure = http_request_body_size_failure(*declared_content_length, http_request_body_limit(stream.resolution(), stream.body_mode(), policy))) {
            stream.mark_body_failure(*failure);
            body_limit_exceeded = true;
        }
    }
    const bool answered_from_head = !resolved_route || body_limit_exceeded || dispatch_with_head || expectation == http_server_expectation_plan::reject_unsupported;
    return {
        .send_continue_ = expectation == http_server_expectation_plan::send_continue && !answered_from_head && stream.body_mode() == request_body_mode::buffered,
        .admit_ = answered_from_head || (stream.body_mode() == request_body_mode::stream && body_open),
    };
}

// Stores one DATA chunk of a request body that is still wanted and applies
// the outcome, alike for HTTP/2 and HTTP/3. A streamed backlog past its bound
// cancels the stream. Content past the body limit or the inbound buffer
// budget fails the body: the response reports it (413, or 503 for the
// budget), or the stream is cancelled once a response was committed. The
// host operations:
//   reset_stream(bool cancelled) noexcept: resets the stream as cancelled, or
//     with an internal error;
//   wake_writer() noexcept: lets the writer send returned credit;
//   admit_stream() -> bool: dispatches the stream, which has no handler yet.
template <typename reset_type, typename wake_writer_type, typename admit_type>
void store_multiplexed_request_body_chunk(multiplexed_request_stream_state& stream, std::string_view bytes, http_received_data_credit&& chunk_credit, protocol_byte_limit total_limit, std::size_t backlog_limit, reset_type&& reset_stream, wake_writer_type&& wake_writer, admit_type&& admit_stream) {
    auto status = multiplexed_body_store_status::stored;
    {
        // Credit the stream does not keep returns here, before the writer is
        // woken for it.
        auto credit = std::move(chunk_credit);
        status = stream.store_body(bytes, std::move(credit), total_limit, backlog_limit);
        if (status == multiplexed_body_store_status::buffer_limit) {
            stream.discard_unread_body();
        }
    }
    if (status == multiplexed_body_store_status::backlog_overflow) {
        reset_stream(true);
        return;
    }
    if (status == multiplexed_body_store_status::stored) {
        if (stream.body_mode() != request_body_mode::stream) {
            wake_writer();
        } else if (auto* signal = stream.signal()) {
            signal->wake();
        }
        return;
    }
    stream.mark_body_failure(status == multiplexed_body_store_status::buffer_limit ? http_request_body_failure::buffer_limit : http_request_body_failure::too_large);
    if (stream.response_committed()) {
        reset_stream(true);
        return;
    }
    wake_writer();
    if (auto* signal = stream.signal()) {
        signal->wake();
    } else if (!admit_stream()) {
        reset_stream(false);
    }
}

// Queues the bytes of an accepted tunnel for its reader, alike for HTTP/2 and
// HTTP/3. The tunnel has no response left to report a backlog or inbound
// budget overflow, so `cancel_stream() noexcept` resets the stream as
// cancelled rather than truncating the byte stream.
template <typename cancel_type>
void store_multiplexed_tunnel_bytes(multiplexed_request_stream_state& stream, std::string_view bytes, http_received_data_credit&& credit, std::size_t backlog_limit, cancel_type&& cancel_stream) {
    if (stream.body_queue().enqueue_bounded(bytes, std::move(credit), backlog_limit) != multiplexed_body_store_status::stored) {
        cancel_stream();
    } else if (auto* signal = stream.signal()) {
        signal->wake();
    }
}

// Completes a multiplexed response-stream route result. Returns true when the
// stream already terminated (peer abort before commit or committed head), or
// false when `response` holds a buffered route/recovered response to finish.
template <typename reset_type>
[[nodiscard]] inline bool complete_multiplexed_response_stream_dispatch(response_stream_dispatch_result& result_value, bool request_body_failed, const http_server_options& options, std::string_view remote_address, http_response_tail& tail, http_response& response, bool& body_failure_handled, reset_type&& reset_stream) {
    if (result_value.aborted_before_commit() != nullptr) {
        return true;
    }
    if (const auto committed_status = result_value.committed_status()) {
        if (const auto* failed = result_value.failed_after_commit()) {
            reset_stream(request_body_failed);
            if (!request_body_failed) {
                options.connection_failure_.invoke(remote_address, failed->exception());
            }
        } else if (request_body_failed) {
            reset_stream(true);
        }
        tail.record_access(options.access_log_, *committed_status);
        return true;
    }
    if (auto* route_response = result_value.route_response()) {
        response = std::move(*route_response).take_response();
    } else if (auto* recovered = result_value.recovered_failure()) {
        body_failure_handled = request_body_failed;
        response = std::move(*recovered).take_response();
    } else {
        throw std::logic_error("response stream dispatch returned no terminal alternative");
    }
    return false;
}

template <typename stream_id_type, typename writer_type, typename reset_type>
[[nodiscard]] task<void> finish_multiplexed_buffered_response(stream_id_type stream_id, const std::optional<http_request_body_failure>& body_failure, bool body_failure_handled, http_response& response, const http_request& request, const route_table& routes, const context_services& request_services, const http_server_options& options, std::string_view remote_address, http_response_tail& tail, writer_type& writer, reset_type reset_stream) {
    if (body_failure.has_value() && !body_failure_handled) {
        body_failure_handled = true;
        response = co_await respond_http_request_body_failure(routes, request, *body_failure, request_services, tail);
    }
    const auto first = tail.prepare(response, options, request_services.worker());
    const auto* first_ready = first.ready();
    auto preparation = first_ready != nullptr ? *first_ready : co_await tail.finish_prepare(first, response, options, routes, request_services);
    if (body_failure.has_value() && !body_failure_handled) {
        response = co_await respond_http_request_body_failure(routes, request, *body_failure, request_services, tail);
        const auto after_body_failure = tail.prepare(response, options, request_services.worker());
        const auto* ready = after_body_failure.ready();
        preparation = ready != nullptr ? *ready : co_await tail.finish_prepare(after_body_failure, response, options, routes, request_services);
    }
    if (request_services.get_stop_token().stop_requested()) {
        reset_stream(true);
        co_return;
    }
    auto written = co_await writer.write(stream_id, response, preparation.write_plan_);
    if (written.file_failure_ && !written.committed_status_) {
        // Nothing of the response was sent: the stream gets the error
        // response instead of a reset.
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
        tail.disable_response_coding();
        response = co_await routes.respond_exception(request, written.file_failure_->response_error(), request_services);
        const auto after_file_failure = tail.prepare(response, options, request_services.worker());
        const auto* ready = after_file_failure.ready();
        preparation = ready != nullptr ? *ready : co_await tail.finish_prepare(after_file_failure, response, options, routes, request_services);
        written = co_await writer.write(stream_id, response, preparation.write_plan_);
        if (written.file_failure_ && !written.committed_status_) {
            reset_stream(false);
        }
    }
    if (written.file_failure_) {
        options.connection_failure_.invoke(remote_address, written.file_failure_->fault());
    }
    if (written.committed_status_) {
        tail.record_access(options.access_log_, *written.committed_status_);
    }
}

// Runs a tunnel route over one multiplexed stream. A request stream
// listener's tunnel route may refuse through `refusal_sink`, the stream's
// ordinary response stream, instead of establishing the tunnel. Returns the
// buffered response still to finish; none when the stream is answered -- the
// tunnel ran, or the refusal was committed on the sink.
template <typename transport_type, typename sink_type, typename commit_upgrade_type, typename commit_websocket_type>
[[nodiscard]] task<std::optional<http_response>> execute_multiplexed_tunnel_endpoint(sink_type& refusal_sink, const route_table& routes, const http_request& request, const resolved_route& resolved, const context_services& dispatch_services, const http_server_options& options, connection_scanner::entry& scanner_entry, inbound_buffer_account& inbound, bool udp, http_response_tail& tail, commit_upgrade_type commit_upgrade, commit_websocket_type commit_websocket) {
    auto refusal_writer = make_response_stream_writer(refusal_sink);
    auto upgrade_services = dispatch_services;
    if (is_request_stream_tunnel_route(resolved.route())) {
        upgrade_services.bind_response_stream(refusal_writer);
    }
    auto buffered = co_await execute_tunnel_upgrade_session<transport_type>(routes, request, resolved, upgrade_services, options, scanner_entry, inbound, udp, std::string_view{}, std::move(commit_upgrade), std::move(commit_websocket));
    if (refusal_sink.committed()) {
        tail.record_access(options.access_log_, committed_response_stream_status(refusal_sink));
        co_return std::nullopt;
    }
    co_return buffered;
}

}  // namespace ruvia::detail
