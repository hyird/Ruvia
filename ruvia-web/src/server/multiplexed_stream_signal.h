#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "http/http_cors.h"
#include "http/http_protocol_error_info.h"
#include "server/http_request_admission.h"
#include "server/http_response_stream_state.h"
#include "server/http_streaming_response_compression.h"
#include "server/inbound_transport_error.h"
#include "server/response_file_source.h"
#include "tls/tls_stream_end.h"

namespace ruvia {
class context;
}

namespace ruvia::detail {

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
        explicit slot(multiplexed_file_read_limiter& owner) noexcept
            : owner_(&owner) {}
        ~slot() {
            --owner_->active_;
            owner_->released_.notify();
        }

        slot(const slot&) = delete;
        slot& operator=(const slot&) = delete;

    private:
        multiplexed_file_read_limiter* owner_;
    };

    explicit multiplexed_file_read_limiter(
        const worker_handle& worker_value, std::size_t max_slots = multiplexed_file_read_slots) noexcept
        : released_(worker_value),
          max_slots_(max_slots) {}
    multiplexed_file_read_limiter(worker_handle&&, std::size_t = multiplexed_file_read_slots) = delete;
    multiplexed_file_read_limiter(const multiplexed_file_read_limiter&) = delete;
    multiplexed_file_read_limiter& operator=(const multiplexed_file_read_limiter&) = delete;

    template <typename stream_ended_predicate>
    [[nodiscard]] task<bool> acquire(stream_ended_predicate stream_ended) {
        while (active_ >= max_slots_) {
            co_await released_.wait();
            if (stream_ended()) {
                co_return false;
            }
        }
        ++active_;
        co_return true;
    }

    template <typename stream_ended_predicate>
    [[nodiscard]] task<std::optional<multiplexed_buffered_response_write_result>> open_file_body(
        std::optional<response_file_source>& source, const http_response& response,
        const response_file_io& file_io, stream_ended_predicate stream_ended) {
        source.emplace(response, file_io);
        bool opened = false;
        {
            if (!(co_await acquire(stream_ended))) {
                co_return multiplexed_buffered_response_write_result{};
            }
            const slot held(*this);
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
        const cors_options* cors) noexcept
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
    const cors_options* cors_;
    http_streaming_response_compression compression_;

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
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            if (cors_ != nullptr) {
                apply_cors_headers(context_access::request(state_.bound_context()), response, *cors_);
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
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() {
        throw_if_body_failed();
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            throw_if_body_failed();
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
            }
            if (derived().stream_aborted()) {
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
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), derived_type::terminated_error_message);
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

private:
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
        for (;;) {
            derived().require_live();
            if (derived().consume_tunnel_chunk(buffer)) {
                co_return true;
            }
            if (derived().receive_ended()) {
                co_return false;
            }
            derived().require_live();
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, websocket_transport_disposition disposition) {
        return derived().write_bytes(bytes_value,
            disposition == websocket_transport_disposition::end_transport ? http_stream_end::end
                                                                          : http_stream_end::keep_open);
    }

protected:
    explicit multiplexed_tunnel_transport_base(multiplexed_stream_signal& signal) noexcept
        : signal_(signal) {}

    multiplexed_stream_signal& signal_;

private:
    [[nodiscard]] derived_type& derived() noexcept {
        return static_cast<derived_type&>(*this);
    }
    [[nodiscard]] const derived_type& derived() const noexcept {
        return static_cast<const derived_type&>(*this);
    }
};

inline request_memory& emplace_multiplexed_request_memory(
    std::optional<request_memory>& storage, worker_memory& memory, std::span<std::byte> initial_buffer) {
    const auto initial_bytes = memory.request_initial_buffer_bytes();
    if (initial_bytes <= initial_buffer.size()) {
        storage.emplace(memory, initial_buffer.first(initial_bytes));
    } else {
        storage.emplace(memory);
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

}  // namespace ruvia::detail
