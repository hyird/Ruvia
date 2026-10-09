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
#include "ruvia/http/http2_connection.h"

#include "http2/http2_buffered_response_write.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_session_context.h"
#include "http2/http2_sans_io_session_lifecycle.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "http2/http2_sans_io_termination.h"
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
    [[nodiscard]] bool worker_running() const noexcept;
    void set_inactivity_phase() noexcept;
    void touch_activity() noexcept;
    void wake_writer() noexcept;
    void terminate(std::error_code error) noexcept;
    [[nodiscard]] task<void> finish();

    [[nodiscard]] std::pmr::memory_resource* worker_resource() const noexcept;

private:
    [[nodiscard]] task<bool> push_request(std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] task<void> dispatch_one_inner(std::uint32_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::uint32_t stream_id);
    [[nodiscard]] bool admit_stream(std::uint32_t stream_id);
    void remove_stream_runtime(std::uint32_t stream_id) noexcept;
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
    http2_sans_io_stream_runtime_table stream_runtimes_;
    http2_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
};

}  // namespace ruvia::detail
