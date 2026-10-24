#pragma once

#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/context.h"

#include "body/http_lazy_buffered_body.h"
#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "http/http_cors.h"
#include "server/connection_scanner.h"
#include "server/http1_peer_eof_monitor.h"
#include "server/http_response_stream_state.h"
#include "server/http_response_writer.h"
#include "server/http_streaming_response_compression.h"
#include "server/inbound_transport_error.h"
#include "server/worker_codecs.h"

namespace ruvia::detail {

// Writes a response stream to the connection. The handler runs in the
// session's idle deadman phase (idle_timeout, like a buffered handler); only
// the sink's own socket writes count as `writing`, so write_timeout measures a
// slow reader and not the time a handler spends waiting for its next event.
template <typename stream_type>
class response_stream_sink final {
public:
    // `cors` is the application's CORS policy (null when none is configured);
    // like `request` and `request_body`, it must outlive the sink. `plan`
    // assumes the request body is left unread; `consumed_plan` is the plan for
    // the same request once `request_body` reports its complete consumption,
    // which commit checks so a route that read its body before streaming keeps
    // the connection reusable.
    response_stream_sink(stream_type& stream, worker_memory& memory, worker_codecs& codecs,
        http_response_head_buffer& head, connection_scanner::entry& scanner_entry,
        const worker_handle& worker_value, const http_request& request, const cors_options* cors,
        http1_response_stream_plan plan, http1_response_stream_plan consumed_plan,
        const lazy_buffered_body<stream_type>& request_body, http_response_coding_selection response_coding,
        http_response_coding_availability response_coding_availability,
        std::pmr::memory_resource* inbound_resource = nullptr) noexcept
        : stream_(stream),
          head_(head),
          trailers_(memory.resource()),
          scanner_entry_(scanner_entry),
          worker_(worker_value),
          request_(request),
          cors_(cors),
          plan_(plan),
          consumed_plan_(consumed_plan),
          request_body_(request_body),
          connection_plan_(plan.request_connection_plan().require_close()),
          compression_(memory.resource(), codecs, response_coding, response_coding_availability),
          inbound_resource_(inbound_resource != nullptr ? inbound_resource : memory.resource()) {}

    response_stream_sink(stream_type&, worker_memory&, worker_codecs&, http_response_head_buffer&,
        connection_scanner::entry&, worker_handle&&, const http_request&, const cors_options*,
        http1_response_stream_plan, http1_response_stream_plan,
        const lazy_buffered_body<stream_type>&, http_response_coding_selection,
        http_response_coding_availability, std::pmr::memory_resource* = nullptr) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    // A completed body-suppressed head (HEAD / 304) can deliver nothing more,
    // so it reads as aborted and a handler loop polling aborted() ends.
    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || state_.body_suppressed_complete() || peer_aborted();
    }

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    [[nodiscard]] stop_token cancel_on_peer_eof() {
        if (!worker_.is_current()) {
            throw std::logic_error("peer EOF monitoring must start on the connection worker");
        }
        if (peer_monitor_) {
            return peer_monitor_->token();
        }
        if (state_.body_suppressed_complete()) {
            // A body-suppressed head already ended a successful response, so
            // there is nothing left to wait for: a stopped token, as sleep()
            // reports stop_requested.
            if (!head_only_stop_) {
                head_only_stop_.emplace();
                head_only_stop_->request_stop();
            }
            return head_only_stop_->token();
        }
        if (state_.ended() || state_.aborted()) {
            throw std::logic_error("peer EOF monitoring cannot start on a finished response stream");
        }
        if (request_body_.consumption() != http1_request_body_consumption::complete) {
            throw std::logic_error("peer EOF monitoring requires a fully consumed request body");
        }
        peer_monitor_.emplace(stream_, worker_, inbound_resource_, request_body_.pipeline_size());
        try {
            peer_monitor_->start();
        } catch (...) {
            peer_monitor_.reset();
            throw;
        }
        return peer_monitor_->token();
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_monitor_ && peer_monitor_->peer_aborted();
    }

    [[nodiscard]] bool peer_eof_monitoring() const noexcept {
        return peer_monitor_.has_value();
    }

    task<void> finish_peer_monitor() {
        if (peer_monitor_) {
            peer_monitor_->cancel();
            co_await peer_monitor_->join();
        }
    }

    [[nodiscard]] std::string_view monitored_pipeline() const noexcept {
        return peer_monitor_ ? peer_monitor_->pipeline() : std::string_view{};
    }

    template <typename sink>
    friend task<void> response_stream_write_thunk(void*, std::string_view);
    template <typename sink>
    friend task<void> response_stream_end_thunk(void*, std::span<const http_header_view>);
    template <typename sink>
    friend task<timer_sleep_result> response_stream_sleep_thunk(
        void*, std::chrono::milliseconds, const stop_token&);
    template <typename sink>
    friend void response_stream_bind_context_thunk(void*, context*);
    template <typename sink>
    friend void response_stream_release_context_thunk(void*) noexcept;

private:
    void ensure_peer_writable() const {
        if (peer_monitor_) {
            peer_monitor_->ensure_writable();
        }
    }

    void mark_write_failure(const std::error_code& error) noexcept {
        if (peer_monitor_) {
            peer_monitor_->write_failed(error);
        }
        state_.mark_aborted();
    }

    void bind_context(context* context_value) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    // The caller's coroutine frame retains the buffers and temporary scanner phase.
    // Return the native awaiter instead of allocating another task frame per write.
    template <typename buffers_type>
    [[nodiscard]] auto write_to_stream(const buffers_type& buffers) {
        return ruvia::async_asio([this, &buffers](auto handler) mutable {
            ensure_peer_writable();
            asio::async_write(stream_, buffers, http_write_progress(scanner_entry_), std::move(handler));
        });
    }

    // A body write after a body-suppressed head completed the response (HEAD,
    // 304) succeeds without sending: the body a GET would carry is discarded.
    // It still suspends for the minimal positive tick (a zero duration is
    // await_ready), so a handler that keeps writing in a loop yields the
    // worker thread instead of hard-spinning the event loop.
    [[nodiscard]] task<timer_sleep_result> yield_discarded_body_write() const {
        return sleep_for(worker_, std::chrono::steady_clock::duration(1));
    }

    task<void> commit(http_response_trailer_intent trailer_intent) {
        ensure_peer_writable();
        if (state_.committed()) {
            if (trailer_intent == http_response_trailer_intent::present) {
                state_.ensure_trailers_allowed();
            }
            co_return;
        }

        try {
            auto response = co_await context_access::commit_streaming_head(state_.bound_context());
            ensure_peer_writable();
            // The same final-representation order as a buffered response: the
            // application CORS policy first, then content coding.
            if (cors_ != nullptr) {
                apply_cors_headers(request_, response, *cors_);
            }
            compression_.prepare(plan_.request_method(), response);
            // The request body is settled at commit: read in full by now, the
            // connection stays reusable; anything less closes it after the
            // stream, since the body bytes still in the socket would be read as
            // the next request.
            if (request_body_.consumption() == http1_request_body_consumption::complete) {
                plan_ = consumed_plan_;
            }
            auto stream_head =
                prepare_http1_response_stream_head(std::move(response), plan_, trailer_intent);
            compression_.activate(stream_head.commit_plan().body_plan());

            head_.reset();
            append_http1_response_head(stream_head.response(), head_, stream_head.response_head_plan());
            connection_plan_ = stream_head.connection_plan();
            // Mark committed before the write; a partial header flush must never be
            // followed by the normal error-response path on the same socket.
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            state_.mark_committed(stream_head.commit_plan());
        } catch (...) {
            // Representation metadata and protocol framing are one pre-wire
            // transaction. Once either side has failed, no handler retry may
            // manufacture a second head from half-prepared compression state.
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto completion = co_await write_to_stream(asio::buffer(head_.view()));
            ec = completion.error_code();
        }
        if (ec) {
            mark_write_failure(ec);
            throw inbound_transport_error(ec, "failed to write HTTP response stream head");
        }
        if (peer_monitor_ && state_.body_suppressed_complete()) {
            // The head ended the response; waits on the peer-EOF token end now.
            peer_monitor_->request_stop();
        }
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        if (state_.body_suppressed_complete()) {
            co_return timer_sleep_result::stop_requested;
        }
        co_return co_await sleep_for(worker_, duration, stop_token_value);
    }

    task<void> write(std::string_view chunk) {
        if (state_.body_suppressed_complete()) {
            static_cast<void>(co_await yield_discarded_body_write());
            co_return;
        }
        ensure_peer_writable();
        if (chunk.empty()) {
            co_return;
        }
        // Only the first chunk needs the asynchronous head-commit path.
        if (!state_.committed()) {
            co_await commit(http_response_trailer_intent::none);
            if (state_.body_suppressed_complete()) {
                // The head just ended a body-suppressed response (HEAD, 304):
                // the chunk is the body a GET would carry and is discarded.
                co_return;
            }
        }
        ensure_peer_writable();
        state_.ensure_body_allowed();

        if (compression_.active()) {
            try {
                compression_.write(chunk);
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            if (compression_.output().empty()) {
                co_return;
            }
            co_await write_encoded(compression_.output());
            co_return;
        }

        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }

        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            std::error_code raw_ec;
            {
                const connection_scanner::entry::phase_scope write_phase(
                    scanner_entry_, connection_scanner::phase::writing);
                const auto completion = co_await write_to_stream(asio::buffer(chunk));
                raw_ec = completion.error_code();
            }
            if (raw_ec) {
                mark_write_failure(raw_ec);
                throw inbound_transport_error(raw_ec, "failed to write HTTP response stream body");
            }
            co_return;
        }

        const http1_chunk_header chunk_header(chunk.size());
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(chunk_header.view()),
            asio::buffer(chunk), asio::buffer(http1_chunk_data_terminator)};
        std::error_code write_ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto completion = co_await write_to_stream(buffers);
            write_ec = completion.error_code();
        }
        if (write_ec) {
            mark_write_failure(write_ec);
            throw inbound_transport_error(write_ec, "failed to write HTTP response stream body");
        }
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (state_.body_suppressed_complete()) {
            // Trailers are optional metadata (RFC 9110 6.5) that a
            // body-suppressed response cannot carry: validated like any
            // trailer section, then discarded.
            static_cast<void>(validate_http_response_trailers(trailers));
            co_return;
        }
        ensure_peer_writable();
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!trailer_section.empty()) {
            http_clear_pmr_string_retaining_small(trailers_);
            append_http1_response_trailers(trailers_, trailer_section);
        } else {
            http_clear_pmr_string_retaining_small(trailers_);
        }

        co_await commit(trailer_intent);
        ensure_peer_writable();
        if (state_.ended()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
        }
        ensure_peer_writable();
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.mark_ended();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(http1_last_chunk_prefix),
            asio::buffer(trailers_), asio::buffer(http1_trailer_section_terminator)};
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto completion = co_await write_to_stream(buffers);
            ec = completion.error_code();
        }
        if (ec) {
            mark_write_failure(ec);
            throw inbound_transport_error(ec, "failed to write HTTP response stream end");
        }
        state_.mark_ended();
    }

    stream_type& stream_;
    http_response_head_buffer& head_;
    std::pmr::string trailers_;
    connection_scanner::entry& scanner_entry_;
    // The connection/server owns an address-stable handle for the complete
    // route dispatch. Streaming must not acquire shared ownership per request.
    const worker_handle& worker_;
    const http_request& request_;
    const cors_options* cors_;
    http1_response_stream_plan plan_;
    http1_response_stream_plan consumed_plan_;
    const lazy_buffered_body<stream_type>& request_body_;
    http1_request_connection_plan connection_plan_;
    http_streaming_response_compression compression_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    std::pmr::memory_resource* inbound_resource_;
    std::optional<http1_peer_eof_monitor<stream_type>> peer_monitor_;
    // A stopped token for cancel_on_peer_eof() after a body-suppressed head.
    std::optional<stop_source> head_only_stop_;
};

}  // namespace ruvia::detail
