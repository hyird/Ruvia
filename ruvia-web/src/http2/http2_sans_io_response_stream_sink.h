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

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response_server.h"

#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/inbound_transport_error.h"
#include "server/multiplexed_stream_signal.h"

namespace ruvia::detail {

class http2_sans_io_response_stream_sink final
    : public multiplexed_response_stream_sink_base<http2_sans_io_response_stream_sink, http2_sans_io_stream_runtime> {
public:
    http2_sans_io_response_stream_sink(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_stream_runtime& stream_runtime,
        worker_signal& write_signal, multiplexed_stream_signal& stream_signal,
        http2_data_output_budget& output_budget, std::pmr::memory_resource* resource,
        http_known_method request_method, worker_codecs& codecs,
        http_response_coding_selection response_coding,
        const compression_config* compression, const cors_options* cors) noexcept
        : multiplexed_response_stream_sink_base(
              stream_runtime, stream_signal, resource, request_method, codecs, response_coding, compression, cors),
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

}  // namespace ruvia::detail
