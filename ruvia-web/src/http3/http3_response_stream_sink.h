#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "context/http_interim_response_output.h"
#include "http/http_cors.h"
#include "http3/http3_response_write.h"
#include "http3/http3_session.h"
#include "http3/http3_stream.h"
#include "server/http_response_stream_state.h"
#include "server/http_streaming_response_compression.h"
#include "server/inbound_transport_error.h"

namespace ruvia {
class context;
}

namespace ruvia::detail {

// Streaming response sink of one HTTP/3 request stream, the method set
// dispatch_response_stream_with drives: commit() submits the streaming final
// head (an application Content-Length becomes the exact content total),
// write() submits DATA within the stream's output budget, waiting for
// acknowledgements when it is spent, and end() finishes the content or sends
// the trailer section. The HTTP/3 core owns the field mapping and framing.
class http3_response_stream_sink final {
public:
    http3_response_stream_sink(http3_session& session, http3_stream& stream, http3_stream_signal& signal,
        std::pmr::memory_resource* resource, http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding, const compression_config* compression,
        const cors_options* cors) noexcept
        : session_(session),
          stream_(stream),
          signal_(signal),
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
        return state_.aborted() || (!state_.ended() && (stream_.aborted() || signal_.terminated()));
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
        http3_data_write_status status{};
        try {
            status = co_await write_http3_stream_data(session_, stream_.id(), signal_, chunk);
        } catch (...) {
            state_.mark_aborted();
            throw;
        }
        if (status == http3_data_write_status::completed) {
            co_return;
        }
        state_.mark_aborted();
        if (status == http3_data_write_status::terminated) {
            throw inbound_transport_error(signal_.terminal_error(), "HTTP/3 connection ended during response stream");
        }
        throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/3 response stream reset");
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        // Connection termination wakes the sleep through the combined token and
        // then fails it; the caller's own stop still reports stop_requested.
        const stop_group stop_owner(stop_token_value, signal_.termination().token());
        const auto result = co_await sleep_for(signal_.worker(), duration, stop_owner.token());
        throw_if_terminated();
        if (stream_.aborted()) {
            throw inbound_transport_error(
                std::make_error_code(std::errc::connection_reset), "HTTP/3 response stream reset during sleep");
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
        http3_status finished{};
        try {
            finished = session_.codec().finish_response(stream_.id(), trailer_section);
        } catch (...) {
            state_.mark_aborted();
            session_.wake_writer();
            throw;
        }
        session_.wake_writer();
        if (finished != http3_status::accepted) {
            state_.mark_aborted();
            throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/3 response stream reset");
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
            compression_.activate(plan_http_response_body(request_method_, response.status()));
            const auto head_result = session_.codec().submit_streaming_response_head(
                stream_.id(), request_method_, std::move(response), trailer_intent);
            const auto* submitted_head = std::get_if<http_response_stream_commit_plan>(&head_result);
            if (submitted_head == nullptr) {
                if (std::get<http3_response_head_error>(head_result) == http3_response_head_error::closed) {
                    throw inbound_transport_error(
                        std::make_error_code(std::errc::connection_reset), "HTTP/3 response stream reset");
                }
                throw std::logic_error("HTTP/3 streaming response head submission failed");
            }
            if (interim_output_ != nullptr) {
                interim_output_->commit_final();
            }
            stream_.mark_response_committed();
            state_.mark_committed(*submitted_head);
            session_.wake_writer();
        } catch (...) {
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
    }

    void throw_if_terminated() const {
        if (signal_.terminated()) {
            throw inbound_transport_error(signal_.terminal_error(), "HTTP/3 connection ended during response stream");
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

    http3_session& session_;
    http3_stream& stream_;
    http3_stream_signal& signal_;
    response_stream_state state_;
    http_interim_response_output* interim_output_{};
    http_known_method request_method_{http_known_method::unknown};
    const cors_options* cors_;
    http_streaming_response_compression compression_;
};

}  // namespace ruvia::detail
