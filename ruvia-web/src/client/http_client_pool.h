#pragma once

#include <array>
#include <exception>
#include <list>
#include <memory>
#include <memory_resource>
#include <span>
#include <system_error>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/web/http_client_handle.h"

#include "client/client_quic_connections.h"
#include "client/client_request_policy.h"
#include "client/client_tcp_transport.h"
#include "client/http_client_advertisement_queue.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_result_budget.h"

namespace ruvia {
class http1_client_response_parser;
class http1_request_content_writer;
struct http3_message_head;
}  // namespace ruvia

namespace ruvia::detail {

class http3_client_body_budget;
class http3_client_connection;
class http3_quic_client_tls_context;

class http_client_pool;
using http_client_cancellation_target = worker_cancellation_target<http_client_pool>;

class http_client_pool final {
public:
    http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
        http_client_config_storage config, http_client_result_budget_config result_budget,
        std::pmr::memory_resource* resource);
    http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
        http_client_config_storage config,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
        std::pmr::memory_resource* resource);
    http_client_pool(asio::io_context&, worker_handle&&, http_client_config_storage,
        http_client_result_budget_config, std::pmr::memory_resource*) = delete;
    http_client_pool(asio::io_context&, worker_handle&&, http_client_config_storage,
        const std::shared_ptr<http_client_result_budget_domain>&, std::pmr::memory_resource*) = delete;
    ~http_client_pool();
    http_client_pool(const http_client_pool&) = delete;
    http_client_pool& operator=(const http_client_pool&) = delete;

    [[nodiscard]] task<http_client_response> execute(
        http_client_request_storage request, operation_options options);
    [[nodiscard]] task<http_client_exchange> open_request(http_client_request_storage request, http_client_upload_config upload, operation_options options);
    [[nodiscard]] task<http_client_tunnel_result> open_tunnel(http_client_request_storage request, http_client_tunnel_config config, operation_options options);
    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_stats stats() const noexcept;
    [[nodiscard]] ruvia::quic_path_migration start_quic_path_migration(
        const asio::ip::udp::endpoint& local_endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    [[nodiscard]] ruvia::quic_operation_status cancel_quic_path_migration(std::uint64_t id);
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() {
        return advertisements_.next();
    }
    [[nodiscard]] std::optional<http_client_push> next_push();
    [[nodiscard]] std::string_view host() const noexcept {
        return config_.host_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] http_scheme scheme() const noexcept {
        return config_.scheme_;
    }

private:
    friend class worker_cancellation_target<http_client_pool>;
    friend class ::ruvia::http_client_response;
    friend class ::ruvia::http_client_tunnel;
    friend class ::ruvia::http_client_exchange;
    friend class ::ruvia::http_client_push;
    friend class http_client_response_state;

    enum class wire_protocol_type : std::uint8_t { unknown,
        http1,
        http2 };
    struct http2_pending_stream_type final {
        http2_pending_stream_type(const worker_handle& worker_value, http_client_response& value)
            : signal_(worker_value),
              response_(&value) {}
        http2_pending_stream_type(worker_handle&&, http_client_response&) = delete;

        worker_signal signal_;
        http_client_response* response_;
        std::optional<http_client_error::code_type> error_;
        std::exception_ptr failure_;
        std::uint64_t request_id_{0};
        std::uint64_t cancellation_id_{0};
        std::uint32_t stream_id_{0};
        // Borrowed from execute_http2 while this stack record remains registered.
        const operation_timeout* timeout_{nullptr};
        bool complete_{false};
        bool retryable_{false};

        [[nodiscard]] bool failed() const noexcept {
            return error_.has_value() || failure_ != nullptr;
        }
    };

    struct http2_runtime_type final {
        http2_runtime_type(const worker_handle& worker_value, std::pmr::memory_resource* resource)
            : write_signal_(worker_value),
              state_signal_(worker_value),
              connect_scheduler_(1, worker_value, resource),
              http1_scheduler_(1, worker_value, resource),
              pending_(resource) {}
        http2_runtime_type(worker_handle&&, std::pmr::memory_resource*) = delete;

        worker_signal write_signal_;
        worker_signal state_signal_;
        pool_lease_scheduler connect_scheduler_;
        pool_lease_scheduler http1_scheduler_;
        std::pmr::vector<http2_pending_stream_type*> pending_;
        std::uint64_t generation_{0};
        std::uint64_t next_request_id_{0};
        std::uint64_t state_cancellation_id_{0};
        std::size_t session_tasks_{0};
        std::size_t http1_operations_{0};
        std::size_t state_cancellation_waiters_{0};
        bool running_{false};
        bool connecting_{false};
        bool draining_{false};
        bool failed_{false};
    };

    struct connection_type final {
        connection_type(asio::io_context& io_context, asio::ssl::context& tls_context,
            const worker_handle& worker_value, const http_client_config_storage& config, client_wire_counters& counters, std::pmr::memory_resource* resource);
        connection_type(asio::io_context&, asio::ssl::context&, worker_handle&&,
            const http_client_config_storage&, client_wire_counters&, std::pmr::memory_resource*) = delete;
        ~connection_type();
        connection_type(const connection_type&) = delete;
        connection_type& operator=(const connection_type&) = delete;
        connection_type(connection_type&&) noexcept;
        connection_type& operator=(connection_type&&) = delete;
        client_tcp_transport transport_;
        std::pmr::string read_buffer_;
        std::pmr::string write_buffer_;
        std::unique_ptr<::ruvia::http2_connection, pmr_object_deleter<::ruvia::http2_connection>> http2_;
        std::unique_ptr<http2_runtime_type, pmr_object_deleter<http2_runtime_type>> http2_runtime_;
        std::uint64_t generation_{0};
        std::uint64_t cancellation_id_{0};
        wire_protocol_type protocol_{wire_protocol_type::unknown};
        bool connected_{false};
    };

    class http2_pending_registration_type final {
    public:
        http2_pending_registration_type(
            http_client_pool& pool, connection_type& connection, http2_pending_stream_type& pending) noexcept
            : pool_(pool),
              connection_(connection),
              pending_(pending) {}
        ~http2_pending_registration_type() {
            reset();
        }

        http2_pending_registration_type(const http2_pending_registration_type&) = delete;
        http2_pending_registration_type& operator=(const http2_pending_registration_type&) = delete;

        void reset() noexcept;

    private:
        http_client_pool& pool_;
        connection_type& connection_;
        http2_pending_stream_type& pending_;
        bool active_{true};
    };

    struct http2_push_driver_type final {
        http2_push_driver_type(http_client_pool& owner_value, connection_type& connection, http_client_response response);
        ~http2_push_driver_type();
        http_client_pool& owner_;
        connection_type& connection_;
        http_client_response response_;
        operation_timeout timeout_;
        http2_pending_stream_type pending_;
        bool registered_{false};
    };
    [[nodiscard]] task<void> run_http2_push(std::unique_ptr<http2_push_driver_type, pmr_object_deleter<http2_push_driver_type>> driver);
    void accept_http2_push(connection_type& connection, const http2_push_promise_event& promise);
    [[nodiscard]] http_client_response_state* accept_http3_push(std::size_t slot, http3_client_connection& connection,
        std::uint64_t request_id, const http3_message_head& request) noexcept;

    class lease_type final {
    public:
        lease_type(http_client_pool& pool, std::size_t index) noexcept
            : pool_(pool),
              index_(index) {}
        ~lease_type();
        [[nodiscard]] connection_type& connection() noexcept {
            return pool_.connections_[index_ % pool_.connections_.size()];
        }
        void discard() noexcept {
            discard_ = true;
        }

    private:
        http_client_pool& pool_;
        std::size_t index_;
        bool discard_{false};
    };

    [[nodiscard]] task<std::size_t> acquire(const ruvia::operation_timeout& timeout, stop_token stop_token);
    void release(std::size_t index) noexcept;
    void close(connection_type& connection) noexcept;
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;
    void cancel_operation(std::size_t index, std::uint64_t generation, client_abort_reason reason) noexcept;
    [[nodiscard]] task<void> ensure_connected(connection_type& connection,
        const ruvia::operation_timeout& timeout, const ruvia::operation_timeout& acquire_timeout,
        stop_token stop_token);
    [[nodiscard]] task<void> initialize_http2(
        connection_type& connection, const ruvia::operation_timeout& timeout);
    [[nodiscard]] task<void> run_http2_reader(connection_type& connection, std::uint64_t generation);
    [[nodiscard]] task<void> run_http2_writer(connection_type& connection, std::uint64_t generation);
    [[nodiscard]] task<void> execute_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    [[nodiscard]] task<void> execute_request_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    [[nodiscard]] task<void> execute_http1_tunnel(connection_type& connection, http_client_response_state& state, const operation_timeout& timeout);
    [[nodiscard]] task<void> execute_http1_response(connection_type& connection, const http_client_request_storage& request,
        const operation_timeout& timeout, http_client_response& response, http1_client_response_parser& parser);
    [[nodiscard]] task<void> write_http1_upload(connection_type& connection, http_client_response_state& state,
        const operation_timeout& timeout, http1_request_content_writer writer, http1_client_response_parser& parser,
        std::exception_ptr& failure);
    [[nodiscard]] task<void> write_upload_bytes(connection_type& connection, std::string_view bytes, const operation_timeout& timeout);
    [[nodiscard]] task<void> execute_http1(connection_type& connection,
        const http_client_request_storage& request, const ruvia::operation_timeout& timeout,
        http_client_response& response);
    [[nodiscard]] task<void> execute_http2(connection_type& connection,
        const http_client_request_storage& request, const ruvia::operation_timeout& timeout,
        stop_token stop_token, http_client_response& response);
    [[nodiscard]] task<void> execute_http3(std::size_t connection_index,
        const http_client_request_storage& request, const ruvia::operation_timeout& timeout,
        stop_token stop_token, http_client_response& response);
    [[nodiscard]] http_client_request_storage make_http3_request(
        const http_client_request_storage& request);
    void drain_http2_events(connection_type& connection);
    void retain_http2_advertisement(connection_type& connection, const http2_event& event);
    void fail_http2_session(connection_type& connection, std::uint64_t generation,
        std::error_code transport_error,
        const std::exception_ptr& failure = std::exception_ptr{}) noexcept;
    void finish_http2_session_task(connection_type& connection, std::uint64_t generation) noexcept;
    void submit_http2_reset(connection_type& connection, std::uint32_t stream_id) noexcept;
    void cancel_http2_stream(
        connection_type& connection, std::uint64_t request_id, client_abort_reason reason) noexcept;
    void abandon_response(http_client_response_state& state) noexcept;
    void reprioritize(http_client_response_state& state, http_priority priority);
    void release_response_data(http_client_response_state& state) noexcept;
    void remove_http2_pending(connection_type& connection, http2_pending_stream_type& pending) noexcept;
    [[nodiscard]] task<void> wait_for_http2_session_stop(
        connection_type& connection, const ruvia::operation_timeout& timeout, stop_token stop_token);
    asio::io_context& io_context_;
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    http_client_config_storage config_;
    client_request_policy policy_;
    http_client_advertisement_queue advertisements_;
    std::pmr::list<http_client_push> pushes_;
    std::size_t active_pushes_{};
    std::size_t received_pushes_{};
    std::size_t rejected_pushes_{};
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_memory_domain::owner response_memory_{};
    client_wire_counters wire_counters_;
    asio::ssl::context tls_context_;
    std::pmr::vector<connection_type> connections_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<http_client_cancellation_target> cancellation_target_;
    task_scope background_tasks_;
    client_quic_connections quic_;
    std::size_t requests_buffered_{0};
    std::size_t requests_in_flight_{0};
    std::size_t completed_requests_{0};
    std::size_t failed_requests_{0};
    bool background_joined_{false};
};

}  // namespace ruvia::detail
