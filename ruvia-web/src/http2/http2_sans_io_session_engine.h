#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_buffered_response_write.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_session.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/http_request_admission.h"
#include "server/inbound_buffer_resource.h"
#include "server/multiplexed_stream_signal.h"

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
    void take_output(std::pmr::string& output);
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
    inbound_buffer_resource inbound_buffers_;
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
