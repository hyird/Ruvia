#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"
#include "http2/http2_client_session.h"

namespace ruvia::detail {

class http_client_request_storage;
class http_client_pool;
class http_client_response_state;

// One fixed TCP slot of an http_client_pool: its transport, the HTTP/1 parse
// buffers and the HTTP/2 session with its stream registry. HTTP/1 exchanges
// run through http1_client_exchange; the pool owns admission and policy.
// Worker-affine and address-stable.
class http_client_connection final {
public:
    enum class protocol_type : std::uint8_t { unknown,
        http1,
        http2 };

    // One registered HTTP/2 request or push stream; a stack or driver record
    // that unregisters before its response state can be released.
    struct http2_stream final {
        http2_stream(const worker_handle& worker_value, http_client_response_state& state_value)
            : signal_(worker_value),
              state_(&state_value) {}
        http2_stream(worker_handle&&, http_client_response_state&) = delete;

        worker_signal signal_;
        http_client_response_state* state_;
        std::optional<http_client_error::code_type> error_;
        std::exception_ptr failure_;
        std::uint64_t request_id_{0};
        std::uint64_t cancellation_id_{0};
        std::uint32_t stream_id_{0};
        // Borrowed from the request or push driver while registered.
        const operation_timeout* timeout_{nullptr};
        std::string_view request_target_;
        bool complete_{false};
        bool retryable_{false};

        [[nodiscard]] bool failed() const noexcept {
            return error_.has_value() || failure_ != nullptr;
        }
    };

    http_client_connection(http_client_pool& pool, std::size_t index);
    ~http_client_connection();
    http_client_connection(const http_client_connection&) = delete;
    http_client_connection& operator=(const http_client_connection&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }

    // Serialized by connect_scheduler_; a live connection is reused.
    [[nodiscard]] task<void> ensure_connected(
        const operation_timeout& timeout, const operation_timeout& acquire_timeout, stop_token stop_token_value, http_client_response_state& response);
    // Terminal close of the current connection; a running HTTP/2 session
    // fails through its single transition so every stream is woken.
    void close() noexcept;
    // Applies the retention policy to both buffers. Only between exchanges,
    // when no transport operation borrows their storage.
    void retain_idle_buffers() noexcept;
    // Pool shutdown: permanently refuses new leases, aborts the connect or
    // HTTP/1 exchange in progress with closing, then closes.
    void shutdown() noexcept;
    // Lease cancellation: aborts the connect or HTTP/1 exchange in progress
    // with `reason` and closes the transport.
    void cancel(std::uint64_t generation, client_abort_reason reason) noexcept;
    // Stops the bound HTTP/1 response's output and wakes its producer waits.
    [[nodiscard]] bool cancel_by_id(std::uint64_t cancellation_id) noexcept;
    void abort_http1_response() noexcept;
    // Throws the HTTP client error of the transport's abort reason, if any.
    void throw_if_aborted() const;

    // HTTP/2 (http2/http_client_http2.cpp).
    [[nodiscard]] task<void> execute_http2(const http_client_request_storage& request,
        const operation_timeout& timeout, stop_token stop_token_value, http_client_response_state& state_value);
    void register_http2_stream(http2_stream& stream);
    void remove_http2_stream(http2_stream& stream) noexcept;
    void cancel_http2_stream(std::uint64_t request_id, client_abort_reason reason) noexcept;
    [[nodiscard]] std::uint64_t next_http2_request_id() noexcept;
    void reprioritize(http_client_response_state& state_value, http_priority priority);
    // Wakes the writer to flush WINDOW_UPDATE for released DATA credit.
    void release_response_data(http_client_response_state& state_value) noexcept;

    // http2_client_session handler.
    void on_http2_event(http2_event& event);
    void on_http2_data_drained(std::uint32_t stream_id) noexcept;
    void on_http2_failure(const client_transport_result& transport, std::exception_ptr failure) noexcept;

    client_tcp_transport transport_;
    http_io_buffer read_buffer_;
    http_io_buffer write_buffer_;
    http2_client_session<http_client_connection> http2_;
    pool_lease_scheduler connect_scheduler_;
    pool_lease_scheduler http1_scheduler_;
    // HTTP/1 exchanges holding or waiting for http1_scheduler_.
    std::size_t http1_operations_{0};
    // Borrowed only while one serialized HTTP/1 exchange is active.
    http_client_response_state* http1_response_{nullptr};
    // Bumped around every connect and HTTP/1 exchange so stale cancellations
    // cannot reach a later operation.
    std::uint64_t generation_{0};
    std::uint64_t cancellation_id_{0};
    protocol_type protocol_{protocol_type::unknown};
    bool connected_{false};
    bool retired_{false};

private:
    struct http2_state_waiter final {
        std::uint64_t cancellation_id_{};
        http2_state_waiter* next_{};
    };
    [[nodiscard]] http2_stream* find_http2_stream(std::uint32_t stream_id) noexcept;
    void fail_http2_stream(http2_stream& stream, std::exception_ptr failure, bool reset_stream) noexcept;
    void publish_http2_head(http2_stream& stream, http_client_response_head head);
    void consume_http2_data(std::uint32_t stream_id, std::string_view bytes, http2_received_data_credit credit);
    void finish_http2_message(std::uint32_t stream_id, http2_message_end_event& end);
    [[nodiscard]] task<void> wait_for_http2_session_stop(const operation_timeout& timeout, stop_token stop_token_value);

    http_client_pool& pool_;
    std::size_t index_;
    std::pmr::vector<http2_stream*> http2_streams_;
    std::uint64_t next_http2_request_id_{0};
    http2_state_waiter* state_waiters_{};
};

}  // namespace ruvia::detail
