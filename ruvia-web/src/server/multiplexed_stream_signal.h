#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/cancellation_signal.hpp>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/request_memory.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "http/http_protocol_error_info.h"
#include "http/http_stream_end.h"
#include "server/connection_scanner.h"
#include "server/http_request_admission.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_stream_state.h"
#include "server/http_streaming_response_compression.h"
#include "server/inbound_transport_error.h"
#include "server/request_stream_dispatch.h"
#include "server/response_file_source.h"
#include "util/operation_interrupt.h"

namespace ruvia {
class context;
}

namespace ruvia::detail {

// Inactivity belongs to the connection; request bodies and output waits have
// independent per-stream deadlines.
[[nodiscard]] inline connection_scanner::phase multiplexed_inactivity_phase(bool peer_settings_received,
    bool header_block_in_progress, bool stream_active, bool tunnel_active) noexcept {
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
    [[nodiscard]] bool overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return open_ &&
               ((inactivity.has_value() && now_ms - progress_ms_ >= inactivity->value().count()) ||
                   (completion.has_value() && now_ms - opened_ms_ >= completion->value().count()));
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
    void arm(const worker_handle& worker_value, duration_type bound,
        expired_type on_expired, failure_type on_failure) noexcept {
        try {
            const auto status = worker_value.schedule_timer(timer_,
                deadline_after(bound), [on_expired](worker_timer_outcome outcome) {
                    // Cancellation belongs to worker shutdown, which closes the session.
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                });
            if (status == worker_timer_schedule_status::worker_stopping) {
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
        explicit output_wait_guard(multiplexed_stream_signal& signal) noexcept
            : signal_(signal) {
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

    multiplexed_stream_signal(const worker_handle& worker_value, multiplexed_session_termination& termination)
        : signal_(worker_value),
          termination_(termination) {}
    multiplexed_stream_signal(worker_handle&&, multiplexed_session_termination&) = delete;

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
    // capacity; the connection scanner times it against write_timeout_.
    void set_waiting_output(bool waiting) noexcept {
        waiting_output_ = waiting;
        output_wait_observed_ms_.reset();
        output_wait_expired_ = false;
    }

    [[nodiscard]] bool waiting_output() const noexcept {
        return waiting_output_;
    }

    // True once per wait, when it has been observed for `timeout` in the
    // scanner's clock. The first observation starts the clock.
    [[nodiscard]] bool output_wait_expired(std::int64_t now_ms, timeout_duration timeout) noexcept {
        if (!waiting_output_ || output_wait_expired_) {
            return false;
        }
        if (!output_wait_observed_ms_.has_value()) {
            output_wait_observed_ms_ = now_ms;
        }
        output_wait_expired_ = now_ms - *output_wait_observed_ms_ >= timeout.value().count();
        return output_wait_expired_;
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_.worker();
    }
    const worker_handle& worker() const&& = delete;

private:
    worker_signal signal_;
    multiplexed_session_termination& termination_;
    std::optional<stop_source> stop_source_;
    bool stopped_{false};
    bool waiting_output_{false};
    bool output_wait_expired_{false};
    std::optional<std::int64_t> output_wait_observed_ms_;
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
        slot(slot&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
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

        explicit slot(multiplexed_file_read_limiter& owner) noexcept
            : owner_(&owner) {}

        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(
        const worker_handle& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept
        : released_(worker_value),
          max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_handle&&, std::size_t = multiplexed_file_read_slots) = delete;
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
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(
        std::optional<response_file_source>& source, const http_response& response,
        const response_file_io& file_io, stream_ended_predicate stream_ended) {
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
        if (chunk.empty()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }
        co_await write_encoded(chunk);
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

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, signal_.termination().token());
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), derived_type::sleep_terminated_error_message);
        }
        if (derived().stream_aborted()) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), derived_type::sleep_reset_error_message);
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
    multiplexed_response_stream_sink_base(stream_type& stream, multiplexed_stream_signal& signal,
        std::pmr::memory_resource* resource, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const http_cors_policy* cors) noexcept
        : stream_(stream),
          signal_(signal),
          request_method_(request_method),
          cors_(cors),
          compression_(resource, codecs, response_coding, compression) {}

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
            throw http_request_body_protocol_error(*body_failure);
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
                (void)cors_->apply_response_headers(
                    context_access::request(state_.bound_context()), response);
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
        operation_interrupt interrupt(interrupts_, signal_.worker(), operation, &signal_, &wake_signal,
            "request body read");
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
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), derived_type::reset_error_message);
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
    multiplexed_request_body_reader_base(
        multiplexed_stream_signal& signal, http_interim_response_output* continue_output) noexcept
        : signal_(signal),
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
            throw http_request_body_protocol_error(*failure);
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
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept
        : signal_(signal) {}

    // While assigned, a cancellation emitted on the slot wakes this stream's waits.
    class wake_assignment final {
    public:
        wake_assignment(multiplexed_stream_signal& signal, asio::cancellation_slot slot)
            : slot_(slot) {
            if (slot_.is_connected()) {
                slot_.assign([&signal](asio::cancellation_type) noexcept { signal.wake(); });
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
[[nodiscard]] inline std::pair<route_resolution, request_body_mode> resolve_multiplexed_stream_route(
    const route_table& routes, bool request_stream, std::string_view method_token, std::string_view target_path,
    std::string_view authority, std::string_view protocol) noexcept {
    const auto method = classify_http_method(method_token);
    if (request_stream) {
        return {resolve_request_stream_route(method == http_known_method::connect), request_body_mode::stream};
    }
    const auto path = target_path.substr(0, target_path.find('?'));
    const auto extended_protocol = http_ascii_equals_ignore_case(protocol, "websocket")
                                       ? std::string_view("websocket")
                                       : protocol;
    auto resolution = routes.resolve(route_request_view{
        method, method_token, path, authority, extended_protocol});
    auto body_mode = request_body_mode::buffered;
    if (const auto* resolved = resolution.resolved()) {
        body_mode = resolved->route().endpoint().request_body_mode();
        if ((method == http_known_method::connect && resolved->route().endpoint().websocket() != nullptr) ||
            resolved->route().endpoint().tunnel() != nullptr) {
            body_mode = request_body_mode::stream;
        }
    }
    return {std::move(resolution), body_mode};
}
inline request_memory& emplace_multiplexed_request_memory(
    std::optional<request_memory>& storage, std::size_t initial_bytes, std::span<std::byte> initial_buffer) {
    if (initial_bytes <= initial_buffer.size()) {
        storage.emplace(initial_bytes, initial_buffer.first(initial_bytes));
    } else {
        storage.emplace(initial_bytes);
    }
    return *storage;
}

[[nodiscard]] inline task<http_response> respond_http_request_body_failure(const route_table& routes,
    const http_request& request, request_memory& memory, http_request_body_failure failure,
    const context_services& services, http_response_tail& tail) {
    tail.disable_response_coding();
    return respond_http_request_rejection(routes, request, memory,
        http_request_rejection::error(
            copy_http_protocol_error_info(memory.resource(), http_request_body_protocol_error(failure))),
        services);
}

// Completes a multiplexed response-stream route result. Returns true when the
// stream already terminated (peer abort before commit or committed head), or
// false when `response` holds a buffered route/recovered response to finish.
template <typename reset_type>
[[nodiscard]] inline bool complete_multiplexed_response_stream_dispatch(
    response_stream_dispatch_result& result_value, bool request_body_failed,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, http_response& response, bool& body_failure_handled,
    reset_type&& reset_stream) {
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
[[nodiscard]] task<void> finish_multiplexed_buffered_response(stream_id_type stream_id,
    const std::optional<http_request_body_failure>& body_failure, bool body_failure_handled,
    http_response& response, const http_request& request, const route_table& routes,
    request_memory& request_memory_value, const context_services& request_services,
    const http_server_options& options, std::string_view remote_address,
    http_response_tail& tail, writer_type& writer, reset_type reset_stream) {
    if (body_failure.has_value() && !body_failure_handled) {
        body_failure_handled = true;
        response = co_await respond_http_request_body_failure(
            routes, request, request_memory_value, *body_failure, request_services, tail);
    }
    auto preparation = co_await tail.prepare_or_finish(
        response, options, routes, request_memory_value, request_services);
    if (body_failure.has_value() && !body_failure_handled) {
        response = co_await respond_http_request_body_failure(
            routes, request, request_memory_value, *body_failure, request_services, tail);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_memory_value, request_services);
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
        response = co_await routes.handle_exception(
            request, request_memory_value, written.file_failure_->response_error(), request_services);
        preparation = co_await tail.prepare_or_finish(
            response, options, routes, request_memory_value, request_services);
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

}  // namespace ruvia::detail
