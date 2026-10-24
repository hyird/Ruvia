#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_buffered_response_write.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_session_context.h"
#include "http2/http2_sans_io_session_lifecycle.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "http2/http2_sans_io_termination.h"
#include "server/http_request_admission.h"
#include "server/inbound_buffer_resource.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

class route_table;

// Transport-independent application driver for one HTTP/2 connection. The
// transport wrapper owns only socket/TLS reads and writes; protocol event
// dispatch, route execution, stream lifetime, and joins compile once.
class http2_sans_io_session_engine final {
public:
    http2_sans_io_session_engine(asio::any_io_executor executor, asio::ip::tcp::socket& socket,
        const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value);

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
    [[nodiscard]] task<void> wait_for_write();
    void writer_write_failed(std::error_code error) noexcept;
    void output_write_completed() noexcept;
    void writer_submitting() noexcept;
    void writer_launch_failed() noexcept;
    void writer_completed(std::exception_ptr exception) noexcept;

    [[nodiscard]] bool connection_failed() const noexcept;
    [[nodiscard]] bool terminated() const noexcept;
    [[nodiscard]] bool header_block_in_progress() const noexcept;
    [[nodiscard]] std::size_t active_runtime_count() const noexcept;
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
    void wake_writer() noexcept;
    void terminate(std::error_code error) noexcept;
    [[nodiscard]] task<void> finish();

    [[nodiscard]] std::pmr::memory_resource* worker_resource() const noexcept;

private:
    struct tunnel_upgrade_commit;
    struct websocket_upgrade_commit;

    void reset_event_stream(std::uint32_t stream_id, http2_error_code error) noexcept;
    [[nodiscard]] http2_sans_io_stream_runtime* resolve_stream_route(const http2_request_head_event& message_head);
    void on_message_head(http2_request_head_event& message_head);
    void on_body_chunk(http2_message_body_chunk_event& body_chunk);
    void on_tunnel_data(http2_tunnel_data_event& tunnel_data);
    void on_tunnel_end(const http2_tunnel_end_event& tunnel_end) noexcept;
    void on_message_end(const http2_message_end_event& message_end);
    void on_stream_closed(const http2_stream_closed_event& stream_closed) noexcept;
    [[nodiscard]] bool push_request(std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] task<void> dispatch_one_inner(std::uint32_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::uint32_t stream_id);
    [[nodiscard]] bool admit_stream(std::uint32_t stream_id);
    void remove_stream_runtime(std::uint32_t stream_id) noexcept;
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;
    void reset_stream_no_throw(std::uint32_t stream_id, http2_error_code error) noexcept;

    asio::any_io_executor executor_;
    asio::ip::tcp::socket& socket_;
    const route_table& routes_;
    worker_memory& worker_;
    http2_sans_io_session_context session_;
    inbound_buffer_resource inbound_buffers_;
    std::string_view remote_address_;
    ruvia::http2_connection connection_;
    worker_signal write_signal_;
    http2_data_output_budget output_budget_;
    worker_signal handler_finished_;
    worker_signal writer_finished_;
    http2_sans_io_session_lifecycle lifecycle_;
    http2_sans_io_termination termination_;
    http_request_admission_policy admission_policy_;
    http2_sans_io_stream_runtime_table stream_runtimes_;
    http2_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    bool drain_finished_{};
    // A batch removed from protocol output still belongs to the socket writer.
    bool output_write_in_flight_{};
    // A connection timeout started (or joined) the drain: its time bound ends
    // the session instead of only making the GOAWAY boundary final.
    bool timeout_drain_{};
    bool drain_expired_{};
    connection_scanner::periodic_check_registration drain_registration_;
    // Declared last: destroyed (and so unregistered) before the state its
    // completion touches.
    worker_timer_registration drain_timer_;
};

}  // namespace ruvia::detail
