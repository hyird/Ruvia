#pragma once

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/streaming.h"

// Streaming response sink for the sans-I/O HTTP/2 session (ruvia-web).
//
// Mirrors the coroutine Http2ResponseStreamSink but drives an http2_connection through
// its submit* API instead of a socket: commit() emits the streaming HEADERS (no
// Content-Length) via submit_streaming_response_head, write() streams DATA via submit_data,
// and end() closes the stream. The shared dispatch_response_stream_with machinery calls
// these via the response_stream*Thunk<Sink> function pointers, so the method set matches.
//
// Backpressure is driven by partial DATA consumption and the transport budget.
// Trailers are submitted semantically to the HTTP core, which owns their protocol bytes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_response_server.h"

#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "http/http_cors.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/http_response_stream_state.h"
#include "server/http_streaming_response_compression.h"
#include "server/inbound_transport_error.h"

namespace ruvia {
class context;  // only forwarded as context* via the type-erased bind_context thunk
}

namespace ruvia::detail {

class http2_sans_io_response_stream_sink final {
public:
    http2_sans_io_response_stream_sink(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_stream_runtime& stream_runtime,
        worker_signal& write_signal, http2_sans_io_stream_signal& stream_signal,
        http2_data_output_budget& output_budget, std::pmr::memory_resource* resource,
        http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding,
        const compression_config* compression, const cors_options* cors) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          stream_runtime_(stream_runtime),
          write_signal_(write_signal),
          stream_signal_(stream_signal),
          output_budget_(output_budget),
          request_method_(request_method),
          cors_(cors),
          compression_(resource, codecs, response_coding, compression) {}
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() &&
                                       (connection_.stream_aborted(stream_id_) || stream_signal_.terminated()));
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
        const auto status = co_await write_http2_stream_data(connection_, stream_id_, &stream_signal_,
            write_signal_, output_budget_, chunk, http2_end_stream::keep_open);
        if (status == http2_stream_data_write_status::completed) {
            co_return;
        }
        state_.mark_aborted();
        switch (status) {
            case http2_stream_data_write_status::completed:
                co_return;
            case http2_stream_data_write_status::session_terminated:
                throw inbound_transport_error(
                    stream_signal_.terminal_error(), "HTTP/2 connection ended during response stream");
            case http2_stream_data_write_status::local_aborted:
            case http2_stream_data_write_status::peer_closed:
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), "HTTP/2 response stream reset");
            case http2_stream_data_write_status::invalid_state:
                throw std::logic_error("invalid HTTP/2 response stream DATA state");
            case http2_stream_data_write_status::content_length_exceeded:
                throw std::length_error("HTTP/2 response exceeds Content-Length");
        }
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, stream_signal_.termination().token());
        const auto result = co_await sleep_for(write_signal_.worker(), duration, stop_owner.token());
        throw_if_terminated_during_sleep();
        if (connection_.stream_aborted(stream_id_)) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), "HTTP/2 response stream reset during sleep");
        }
        co_return result;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        // An empty terminal call is idempotent once our END_STREAM is already
        // committed locally; a later session/stream termination must not turn
        // dispatch cleanup into a transport failure.
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
        // Preflight through the HTTP-owned result before committing the initial
        // response head. The typed section carries that proof to finish_response.
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
        // An empty trailer section ends the stream with an empty DATA frame,
        // which needs both send windows open (RFC 9113 §6.9).
        for (;;) {
            http2_finish_status result_value{};
            try {
                result_value = connection_.finish_response(stream_id_, trailer_section);
            } catch (...) {
                state_.mark_aborted();
                wake_writer();
                throw;
            }
            wake_writer();
            if (result_value == http2_finish_status::accepted) {
                break;
            }
            if (result_value == http2_finish_status::backpressured &&
                co_await await_http2_send_window(connection_, stream_id_, &stream_signal_) ==
                    http2_send_window_wait_status::ready) {
                continue;
            }
            state_.mark_aborted();
            throw_if_terminated();
            throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/2 response stream reset");
        }
        state_.mark_ended();
    }

private:
    // Callers check committed() before constructing this head-commit task.
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
            const auto head_result = connection_.submit_streaming_response_head(
                stream_id_, std::move(response), trailer_intent);
            const auto* submitted_head = std::get_if<http_response_stream_commit_plan>(&head_result);
            if (submitted_head == nullptr) {
                const auto head_error = std::get<ruvia::http2_response_head_submit_error>(head_result);
                if (head_error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
                    // A promised push whose stream the client's concurrency limit
                    // cannot admit is cancelled; the connection is unaffected.
                    (void)connection_.submit_reset(stream_id_, ruvia::http2_error_code::cancel);
                    wake_writer();
                }
                if (head_error == ruvia::http2_response_head_submit_error::closed ||
                    head_error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
                    throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/2 response stream reset");
                }
                throw std::logic_error("HTTP/2 streaming response head submission failed");
            }
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_runtime_.mark_response_committed();
            state_.mark_committed(*submitted_head);
            wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }

    // Wake the session's single writer so submitted bytes actually flush; without
    // this, output produced between inbound frames sits in the core's buffer until
    // the peer happens to send something (SSE over a quiet connection stalls).
    void wake_writer() noexcept {
        write_signal_.notify();
    }

    void throw_if_terminated() const {
        if (stream_signal_.terminated()) {
            throw inbound_transport_error(stream_signal_.terminal_error(), "HTTP/2 connection ended during response stream");
        }
    }

    void throw_if_terminated_during_sleep() const {
        if (stream_signal_.terminated()) {
            throw inbound_transport_error(stream_signal_.terminal_error(), "HTTP/2 connection ended during sleep");
        }
    }

    void throw_if_body_failed() {
        if (const auto& body_failure = stream_runtime_.body_failure(); body_failure.has_value()) {
            if (state_.committed()) {
                state_.mark_aborted();
            }
            throw http_request_body_protocol_error(*body_failure);
        }
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_stream_runtime& stream_runtime_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    worker_signal& write_signal_;
    http2_sans_io_stream_signal& stream_signal_;
    http2_data_output_budget& output_budget_;
    http_known_method request_method_{http_known_method::unknown};
    const cors_options* cors_;
    http_streaming_response_compression compression_;
};

}  // namespace ruvia::detail
