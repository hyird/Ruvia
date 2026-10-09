#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "server/http_response_stream_state.h"
#include "server/http_server_response_state.h"
#include "server/http_streaming_response_compression.h"

namespace ruvia::detail {

template <typename stream_type, typename scanner_entry_type>
class response_stream_sink final {
public:
    response_stream_sink(stream_type& stream, worker_memory& memory, http_response_head_buffer& head,
        scanner_entry_type& scanner_entry, const worker_handle& worker_value, http_response_stream_kind kind,
        http1_response_stream_plan plan, http_response_coding_selection response_coding,
        http_response_coding_availability response_coding_availability) noexcept
        : stream_(stream),
          head_(head),
          trailers_(memory.resource()),
          scanner_entry_(scanner_entry),
          worker_(worker_value),
          kind_(kind),
          plan_(plan),
          connection_plan_(plan.request_connection_plan().require_close()),
          compression_(memory.resource(), response_coding, response_coding_availability) {}

    response_stream_sink(stream_type&, worker_memory&, http_response_head_buffer&, scanner_entry_type&, worker_handle&&,
        http_response_stream_kind, http1_response_stream_plan, http_response_coding_selection,
        http_response_coding_availability) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted();
    }

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    template <typename sink>
    friend task<void> response_stream_write_thunk(void*, std::string_view);
    template <typename sink>
    friend task<void> response_stream_end_thunk(void*, std::span<const http_header_view>);
    template <typename sink>
    friend task<timer_sleep_result> response_stream_sleep_thunk(
        void*, std::chrono::milliseconds, const stop_token&);
    template <typename sink>
    friend void response_stream_bind_context_thunk(
        void*, context*, response_stream_state::streaming_head_thunk_type);
    template <typename sink>
    friend void response_stream_release_context_thunk(void*) noexcept;

private:
    void bind_context(context* context_value, response_stream_state::streaming_head_thunk_type streaming_head) {
        interim_output_ = context_value != nullptr ? context_access::interim_output(*context_value) : nullptr;
        state_.bind_context(context_value, streaming_head);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    task<void> commit(http_response_trailer_intent trailer_intent) {
        if (state_.committed()) {
            if (trailer_intent == http_response_trailer_intent::present) {
                state_.ensure_trailers_allowed(http_response_stream_trailer_framing::http1_chunked);
            }
            co_return;
        }

        try {
            auto response = co_await state_.streaming_head();
            compression_.prepare(plan_.request_method(), response, kind_);
            auto prepare_result =
                prepare_http1_response_stream_head(std::move(response), kind_, plan_, trailer_intent);
            if (const auto* failure = prepare_result.failure()) {
                throw failure->exception();
            }
            auto* prepared = prepare_result.prepared();
            if (prepared == nullptr) {
                throw std::logic_error(
                    "HTTP/1 stream preparation returned no terminal alternative");
            }
            auto stream_head = std::move(*prepared);
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
        const auto write_completion =
            co_await ruvia::async_asio([this, head_view = head_.view()](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(head_view), std::move(handler));
            });
        const auto ec = write_completion.error_code();
        if (ec) {
            state_.mark_aborted();
            throw std::system_error(ec);
        }
        scanner_entry_.touch();
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        const auto result_value = co_await sleep_for(worker_, duration, stop_token_value);
        if (result_value == timer_sleep_result::elapsed) {
            scanner_entry_.touch();
        }
        co_return result_value;
    }

    task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit(http_response_trailer_intent::none);
        if (state_.body_suppressed_complete()) {
            // ensure_body_allowed() is about to throw response_stream_head_only_complete
            // synchronously (commit() returned without suspending on an already
            // body-suppressed head). Suspend once first: a handler that catches
            // the control signal and keeps writing (an SSE loop answering a HEAD
            // or 304) then yields the worker thread each pass instead of
            // hard-spinning the event loop with no suspension point. The minimal
            // positive delay is required because a zero duration is await_ready.
            static_cast<void>(co_await sleep_for(worker_, std::chrono::steady_clock::duration(1)));
        }
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
            const auto write_completion = co_await ruvia::async_asio([this, chunk](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
            const auto raw_ec = write_completion.error_code();
            if (raw_ec) {
                state_.mark_aborted();
                throw std::system_error(raw_ec);
            }
            scanner_entry_.touch();
            co_return;
        }

        const http1_chunk_header chunk_header(chunk.size());
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(chunk_header.view()),
            asio::buffer(chunk), asio::buffer(http1_chunk_data_terminator)};
        const auto write_completion = co_await ruvia::async_asio([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        const auto write_ec = write_completion.error_code();
        if (write_ec) {
            state_.mark_aborted();
            throw std::system_error(write_ec);
        }
        scanner_entry_.touch();
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }

        const auto trailer_result = validated_response_trailer_section(trailers);
        const auto& trailer_section = *trailer_result.section();
        const auto trailer_intent = response_trailer_intent(trailer_section);
        if (!trailer_section.empty()) {
            ::ruvia::clear_pmr_string_retaining_small(trailers_);
            append_http1_response_trailers(trailers_, trailer_section);
        } else {
            ::ruvia::clear_pmr_string_retaining_small(trailers_);
        }

        co_await commit(trailer_intent);
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
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.mark_ended();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(http1_last_chunk_prefix),
            asio::buffer(trailers_), asio::buffer(http1_trailer_section_terminator)};
        const auto write_completion = co_await ruvia::async_asio([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        const auto ec = write_completion.error_code();
        if (ec) {
            state_.mark_aborted();
            throw std::system_error(ec);
        }
        state_.mark_ended();
        scanner_entry_.touch();
    }

    stream_type& stream_;
    http_response_head_buffer& head_;
    std::pmr::string trailers_;
    scanner_entry_type& scanner_entry_;
    // The connection/server owns an address-stable handle for the complete
    // route dispatch. Streaming must not acquire shared ownership per request.
    const worker_handle& worker_;
    http_response_stream_kind kind_;
    http1_response_stream_plan plan_;
    http1_request_connection_plan connection_plan_;
    http_streaming_response_compression compression_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
};

}  // namespace ruvia::detail
