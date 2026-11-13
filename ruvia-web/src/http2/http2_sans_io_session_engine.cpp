#include "http2/http2_sans_io_session_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/request_memory.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/streaming.h"

#include "body/http_request_body_facade.h"
#include "context/context_access.h"
#include "context/http_connection_advertisement_output.h"
#include "context/http_interim_response_output.h"
#include "context/http_push_output.h"
#include "http/http_protocol_error_info.h"
#include "http/http_stream_end.h"
#include "http/http_tunnel_session.h"
#include "http2/http2_sans_io_request_body.h"
#include "http2/http2_sans_io_send_window.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_writer.h"
#include "server/http_upgrade_route.h"
#include "server/inbound_transport_error.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_session.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t http2_request_arena_stack_bytes = request_arena_initial_bytes;
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
      base_services_(session_.services().with_inbound_buffer_pool(inbound_buffers_)),
      remote_address_(session_.services().get_conn_info().remote().address()),
      connection_(ruvia::http2_connection::server({.resource_ = process_resource(),
          .frame_observer_ = session_.options().http2_frame_
                                 ? +[](void* target, const http2_frame_metadata& metadata) noexcept {
                                       auto& self = *static_cast<http2_sans_io_session_engine*>(target);
                                       self.session_.options().http2_frame_(server_observation_access::http2_frame(
                                           self.base_services_.get_conn_info().id(), metadata));
                                   }
                                 : nullptr,
          .header_observer_ = session_.options().http2_headers_ ? +[](void* target, const http2_header_metadata& metadata) noexcept {
              auto& self = *static_cast<http2_sans_io_session_engine*>(target);
              self.session_.options().http2_headers_(server_observation_access::http2_headers(
                  self.base_services_.get_conn_info().id(), metadata));
          }
                                                                : nullptr,
          .observer_context_ = this})),
      write_signal_(session_.services().worker()),
      output_budget_(session_.services().worker()),
      handler_finished_(session_.services().worker()),
      writer_finished_(session_.services().worker()),
      admission_policy_(http_request_admission_policy::server(session_.options())),
      stream_runtimes_(process_resource(), termination_, &inbound_buffers_),
      buffered_response_writer_(*this, connection_, stream_runtimes_, write_signal_, output_budget_, response_file_io{.worker_ = session_.services().worker(), .stop_ = session_.services().get_stop_token(), .timeout_ = session_.options().server_.write_timeout_}) {
    // A plaintext handoff reuses the HTTP/1 scanner entry. It now tracks a
    // multiplexed session, not an unadmitted HTTP/1 request-head read.
    base_services_.bind_routes(routes_);
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

void http2_sans_io_session_engine::take_output(std::pmr::string& output) {
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

        std::array<std::byte, http2_request_arena_stack_bytes> arena_block;
        std::optional<request_memory> request_memory_storage;
        request_memory& request_memory_value =
            emplace_multiplexed_request_memory(request_memory_storage, session_.options().server_.request_arena_initial_bytes_, std::span<std::byte>(arena_block));
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
        http_request request = request_head->build_request(request_memory_value.resource(),
            streaming_body ? std::string_view{} : stream_runtime->buffered_body());
        http_response response(request_memory_value.resource());
        response_output_target output_target{connection_, write_signal_, stream_id};
        http_interim_response_output interim_output(process_resource(), &output_target, [](void* raw, const http_interim_response_head& head) -> task<void> {
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
        auto reset_request = [this, stream_id](std::optional<std::uint64_t> error) {
            reset_stream_no_throw(stream_id, error ? static_cast<http2_error_code>(*error) : http2_error_code::cancel);
        };
        request_stream_control stream_control{
            callback_access::bind<void(std::optional<std::uint64_t>)>(reset_request)};
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
                .resource_ = request_memory_value.resource(),
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
                routes_, request, request_memory_value, *body_failure, request_services, admission.tail());
        } else {
            switch (admission.endpoint()) {
                case http_request_endpoint::rejection:
                    admission.tail().disable_response_coding();
                    response = co_await respond_http_request_rejection(
                        routes_, request, request_memory_value, *admission.rejection(), request_services);
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
                            handshake->subprotocol(), *ruvia::process_resource());
                    };
                    http2_sans_io_response_stream_sink refusal_sink(connection_, stream_id,
                        *stream_runtime, write_signal_, *stream_signal, output_budget_,
                        process_resource(), request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                        options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto refusal_writer = make_response_stream_writer(refusal_sink, *process_resource());
                    auto upgrade_services = dispatch_services;
                    if (is_request_stream_tunnel_route(resolved->route())) {
                        upgrade_services.bind_response_stream(refusal_writer);
                    }
                    auto buffered = co_await execute_tunnel_upgrade_session<http2_sans_io_tunnel_transport>(
                        routes_, request, *resolved, request_memory_value, upgrade_services, options,
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
                        response = co_await routes_.handle_error(request, request_memory_value,
                            copy_http_protocol_error_info(request_memory_value.resource(), failure->protocol_error()),
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
                            submitted_handshake->compression(), submitted_handshake->subprotocol(),
                            *ruvia::process_resource());
                    };
                    auto buffered = co_await execute_websocket_upgrade_session<http2_sans_io_tunnel_transport>(
                        routes_, request, *resolved, request_memory_value, dispatch_services, options,
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
                        *stream_runtime, write_signal_, *stream_signal, output_budget_,
                        process_resource(), request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                        options.compression_ ? &*options.compression_ : nullptr, admission.tail().cors(options));
                    auto result_value = co_await dispatch_response_stream_with(sink_value, routes_, request, *resolved,
                        request_memory_value, stream_services,
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
                        co_await routes_.dispatch_buffered_response(request, resolution, request_memory_value,
                            options.document_root_.binding(), dispatch_services, admission.static_file_mode(), response);
                        break;
                    }
                    [[fallthrough]];
                case http_request_endpoint::stream_body:
                    body_failure_handled = co_await routes_.dispatch_resolved_route(request, request_memory_value,
                        *resolved, dispatch_services, &stream_runtime->body_failure(), response);
                    break;
            }
        }

        co_await finish_multiplexed_buffered_response(stream_id, stream_runtime->body_failure(),
            body_failure_handled, response, request, routes_, request_memory_value, request_services,
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
        if (auto credit = body_chunk.take_credit(); credit.valid()) {
            (void)connection_.acknowledge(std::move(credit));
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
        if (auto credit = body_chunk.take_credit(); credit.valid()) {
            (void)connection_.acknowledge(std::move(credit));
        }
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
    if (store_status == http2_request_body_store_status::too_large) {
        stream_runtime->mark_body_failure(http_request_body_failure::too_large);
        if (auto credit = body_chunk.take_credit(); credit.valid()) {
            (void)connection_.acknowledge(std::move(credit));
        }
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
        (void)connection_.acknowledge(body_chunk.take_credit());
        wake_writer();
    }
}

void http2_sans_io_session_engine::on_tunnel_data(http2_tunnel_data_event& tunnel_data) {
    const auto& options = session_.options();
    const auto stream_id = tunnel_data.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* signal = stream_runtime != nullptr ? stream_runtime->signal() : nullptr;
    if (stream_runtime == nullptr || signal == nullptr) {
        if (auto credit = tunnel_data.take_credit(); credit.valid()) {
            (void)connection_.acknowledge(std::move(credit));
            wake_writer();
        }
        return;
    }
    if (!stream_runtime->route_selected() || stream_runtime->body_mode() != request_body_mode::stream) {
        // Only a CONNECT that no tunnel route will accept lacks a streaming body:
        // its optimistic DATA is never consumed, so return the credit at once.
        (void)connection_.acknowledge(tunnel_data.take_credit());
        wake_writer();
        return;
    }
    if (!stream_runtime->body_queue().enqueue_bounded(tunnel_data.bytes(),
            tunnel_data.take_credit(), options.server_.max_buffered_body_bytes_)) {
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
        observer(server_observation_access::http2_input(base_services_.get_conn_info().id(),
            std::as_bytes(std::span(bytes_value.data(), bytes_value.size()))));
    }
    for (;;) {
        const auto result_value = connection_.feed(bytes_value);
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
    std::pmr::string write_scratch(process_resource());
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
            http_clear_pmr_string_retaining_small(
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

task<http_connection_close> run_http2_sans_io_session(asio::ip::tcp::socket& stream, const route_table& routes_value,
    http2_sans_io_session_context session_value, std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream, routes_value, std::move(session_value), initial_bytes);
}

task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream.lowest_layer(), routes_value, std::move(session_value), initial_bytes);
}

}  // namespace ruvia::detail
