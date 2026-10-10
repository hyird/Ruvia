#pragma once
#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/http_client_push_config.h"

#include "client/http_client_request_storage.h"
#include "client/http_client_response_state.h"
#include "http3/http3_client_body_budget.h"
#include "http3/http3_client_receive_driver.h"
#include "http3/http3_client_request_driver.h"
#include "http3/http3_client_response_delivery.h"
#include "http3/http3_quic_client_endpoint_resolver.h"
#include "http3/http3_quic_client_socket_session.h"

namespace ruvia::detail {

struct http3_client_origin_observer final {
    void* context_{};
    std::size_t connection_slot_{};
    void (*receive_)(void*, std::size_t, const http_origin_advertisement&){};
};

class http3_client_connection;
// A pool accepts promises into its independent response-memory domain. receive
// returns one retained state reference for the connection; finished balances
// the pool's active-push admission. Neither callback may drive the protocol.
struct http3_client_push_observer final {
    void* context_{};
    std::size_t connection_slot_{};
    http_client_push_config config_{};
    http_client_response_state* (*receive_)(void*, std::size_t, http3_client_connection&, std::uint64_t, const http3_message_head&){};
    void (*finished_)(void*) noexcept {};
};

// One address-stable connection backend owned by its HTTP client pool. The
// pool's task_scope owns its sole DNS/QUIC driver task; request waiters never
// drive sockets. Incremental response delivery binds an address-stable public
// response state while this owner retains the corresponding request node.
// Pool/TLS/worker resources and this object must survive request waiters and
// the driver's join. All methods, including stop and destruction, are worker-
// affine; cross-thread cancellation must post through the worker endpoint.
// The driver ends every QUIC attempt with CONNECTION_CLOSE carrying H3_NO_ERROR,
// the detected RFC 9114 protocol error, or H3_INTERNAL_ERROR for a local
// failure, then a closing period bounded by three PTOs and one second.
// request_stop() still sends the close but ends that wait at once. Admission
// stops before the final closing period begins.
class http3_client_connection final {
public:
    using time_point_type = std::chrono::steady_clock::time_point;
    using request_id_type = std::uint64_t;
    enum class outcome_type : std::uint8_t {
        pending,
        complete,
        cancelled,
        deadline,
        connect_failed,
        transport_error,
        protocol_error,
        // Peer reports unprocessed, or local admission stopped before opening a stream.
        request_rejected,
        response_too_large,
        result_budget_exceeded,
        connection_draining,
        queue_full,
        invalid_request,
    };
    struct response_type final {
        explicit response_type(std::pmr::memory_resource* resource)
            : headers_(resource),
              trailers_(resource),
              body_(resource) {}
        outcome_type outcome_{outcome_type::pending};
        std::uint16_t status_{};
        // Empty unless the peer's final response head supplied this value.
        std::optional<http_response_body_plan> response_body_plan_{};
        std::optional<std::uint64_t> peer_reset_error_code_{};
        std::pmr::vector<http_header> headers_;
        std::pmr::vector<http_header> trailers_;
        std::pmr::string body_;
    };
    struct submission_type final {
        outcome_type outcome_{outcome_type::pending};
        request_id_type id_{};
    };
    struct rejected_request_type final {
        http_client_request_storage request_;
        std::optional<time_point_type> deadline_{};
    };
    struct lifecycle_notification_type final {
        void* context_{};
        void (*notify_)(void*) noexcept {};
    };

    // receive_body_budget may borrow a pool-owned worker-affine owner shared by
    // successive connections. It must outlive this connection and its receive
    // leases; null selects the connection-local budget, which is detached with
    // the response before this connection can be destroyed.
    http3_client_connection(asio::io_context& io, const worker_handle& worker_value,
        task_scope& pool_tasks, http3_quic_client_tls_context& tls, http_origin_view origin,
        std::chrono::milliseconds connect_timeout,
        std::pmr::memory_resource* resource, std::size_t max_requests = 32,
        std::size_t max_response_bytes = 16 * 1024 * 1024,
        std::chrono::milliseconds idle_timeout = std::chrono::seconds(30),
        http3_client_body_budget* receive_body_budget = nullptr,
        std::optional<std::chrono::milliseconds> write_timeout = std::chrono::seconds(30));
    http3_client_connection(asio::io_context& io, const worker_handle& worker_value,
        task_scope& pool_tasks, http3_quic_client_tls_context& tls, http_origin_view origin,
        std::chrono::milliseconds connect_timeout, std::pmr::memory_resource* resource,
        std::size_t max_requests, std::size_t max_response_bytes,
        std::chrono::milliseconds idle_timeout, http3_client_body_budget* receive_body_budget,
        std::optional<std::chrono::milliseconds> write_timeout,
        lifecycle_notification_type lifecycle_notification, http3_qpack_config qpack = {},
        http3_client_origin_observer origin_observer = {}, http3_client_push_observer push_observer = {},
        ruvia::quic_version initial_version = ruvia::quic_version::v1,
        bool enable_early_data = false);
    ~http3_client_connection();
    http3_client_connection(const http3_client_connection&) = delete;
    http3_client_connection& operator=(const http3_client_connection&) = delete;
    http3_client_connection(http3_client_connection&&) = delete;
    http3_client_connection& operator=(http3_client_connection&&) = delete;

    // Request storage becomes owned before this returns. An accepted request
    // remains address stable, including a pending SSL_write WANT range, until
    // completion or cancellation and explicit release(id).
    [[nodiscard]] submission_type submit(http_client_request_storage request,
        std::optional<time_point_type> deadline = {});
    // Internal response-state delivery reuses the same request node and sole
    // connection driver. The state is borrowed until its consumer releases it
    // and all request waiters have left; its owner must keep this connection
    // alive for that entire interval.
    [[nodiscard]] submission_type submit(http_client_request_storage request,
        http_client_response_state& response, std::optional<time_point_type> deadline = {});
    // Uses the original absolute deadline rather than granting a new timeout.
    [[nodiscard]] submission_type submit(rejected_request_type request);
    void start();
    void start_if_needed();
    void cancel(request_id_type id) noexcept;
    [[nodiscard]] bool reprioritize(request_id_type id, http_priority priority);
    [[nodiscard]] task<void> wait(request_id_type id);
    [[nodiscard]] const response_type* result(request_id_type id) const noexcept;
    [[nodiscard]] bool release(request_id_type id) noexcept;
    // Detaches a terminal, retired incremental response without modifying its
    // response data. A nonempty body lease may survive only through a budget
    // shared by successive connections on this worker. The caller must retain
    // the response state, its PMR/result-budget owners, and any external budget;
    // this does not transfer ownership or permit cross-worker/pool teardown.
    // Unprocessed requests remain available to their retry/handoff path.
    [[nodiscard]] bool release_response_request(request_id_type id) noexcept;
    void abandon_response(request_id_type id) noexcept;
    void consumer_released(request_id_type id) noexcept;
    [[nodiscard]] http_datagram_session_config datagram_config(request_id_type id) const;
    [[nodiscard]] bool send_datagram(request_id_type id, std::span<const std::byte> wire);
    // Consumes a peer-rejected or locally unstarted terminal request after all waiters leave.
    // For a bound public response, detaches its empty response state without publishing the
    // rejection so the pool can retry once; any observed response data makes handoff unsafe.
    // Stream retirement already completed before terminal publication. Returned storage keeps
    // its worker allocator, whose owner must outlive it, and preserves even an expired deadline.
    [[nodiscard]] std::optional<rejected_request_type> take_rejected_request(request_id_type id);
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(
        const asio::ip::udp::endpoint& local_endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    [[nodiscard]] std::optional<ruvia::quic_path_migration> active_path_migration() const noexcept;
    [[nodiscard]] std::uint64_t quic_generation() const noexcept {
        return quic_generation_;
    }
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);
    void request_stop() noexcept;
    [[nodiscard]] bool running() const noexcept {
        return running_;
    }
    [[nodiscard]] bool accepting() const noexcept {
        return !stopping_ && !draining_ && !terminal_ &&
               requests_.size() < max_requests_;
    }
    [[nodiscard]] bool terminal() const noexcept {
        return terminal_;
    }
    [[nodiscard]] std::size_t retained_requests() const noexcept {
        return requests_.size();
    }
    [[nodiscard]] std::size_t retained_result_body_bytes() const noexcept {
        return retained_result_body_bytes_;
    }

private:
    struct request_type final {
        request_type(request_id_type value, http3_client_request_write&& write, const worker_handle& worker_value,
            std::pmr::memory_resource* resource, std::optional<time_point_type> absolute_deadline,
            bool replay_safe)
            : id_(value),
              writer_(std::move(write)),
              signal_(worker_value),
              response_(resource),
              deadline_(absolute_deadline),
              replay_safe_(replay_safe) {}
        request_type(request_id_type value, http3_client_request_write&& write, const worker_handle& worker_value,
            std::pmr::memory_resource* resource, std::optional<time_point_type> absolute_deadline,
            bool replay_safe, http_client_response_state& response_state,
            http3_client_body_budget& body_budget)
            : request_type(value, std::move(write), worker_value, resource, absolute_deadline, replay_safe) {
            response_state_ = &response_state;
            delivery_.emplace(response_state, &body_budget);
        }
        request_id_type id_;
        http3_client_request_driver writer_;
        worker_signal signal_;
        response_type response_;
        std::optional<time_point_type> deadline_;
        std::optional<time_point_type> write_deadline_;
        std::optional<time_point_type> continue_deadline_;
        std::optional<http_priority> pending_priority_update_;
        std::size_t waiters_{};
        http_client_response_state* response_state_{};
        std::optional<http3_client_response_delivery> delivery_{};
        bool consumer_released_{};
        bool response_parser_registered_{};
        bool stream_retired_{};
        bool cancel_requested_{};
        bool replay_safe_{};
        bool early_data_eligible_{};
    };
    struct push_type final {
        request_id_type id_{};
        std::uint64_t push_id_{};
        std::optional<std::uint64_t> stream_id_{};
        std::optional<time_point_type> deadline_{};
        http_client_response_state* state_{};
        std::optional<http3_client_response_delivery> delivery_{};
        bool cancel_requested_{};
        bool peer_cancelled_{};
    };
    struct peer_push_stream_type final {
        std::uint64_t push_id_{};
        std::uint64_t stream_id_{};
        std::optional<time_point_type> deadline_{};
    };
    using push_list_type = std::pmr::list<push_type>;
    static constexpr std::size_t max_remembered_pushes = 128;
    using request_list_type = std::pmr::list<request_type>;
    using session_owner_type = std::unique_ptr<http3_quic_client_socket_session,
        pmr_object_deleter<http3_quic_client_socket_session>>;
    // Protocol state of one QUIC connection attempt. Every endpoint attempt
    // starts from a fresh HTTP/3/QPACK engine, receive buffers, critical-stream
    // output, peer streams and push-ID space; a retry re-arms its requests
    // instead of carrying their streams into the next QUIC connection. Request
    // records, results and push consumers remain connection state.
    struct attempt_type final {
        attempt_type(std::pmr::memory_resource* resource, http3_client_body_budget& body_budget,
            const http3_client_sans_io_response_limits& limits);
        attempt_type(const attempt_type&) = delete;
        attempt_type& operator=(const attempt_type&) = delete;

        http3_client_sans_io_session_engine engine_;
        http3_client_receive_driver receiver_;
        std::array<std::pmr::string, 3> critical_output_;
        std::array<std::size_t, 3> critical_output_offset_{};
        std::array<std::optional<time_point_type>, 3> critical_write_deadlines_{};
        std::pmr::vector<std::uint64_t> peer_streams_;
        std::pmr::vector<peer_push_stream_type> peer_push_streams_;
        std::bitset<max_remembered_pushes> seen_pushes_{};
        std::bitset<max_remembered_pushes> settled_pushes_{};
        std::bitset<max_remembered_pushes> cancelled_pushes_{};
        std::uint64_t authorized_push_id_{};
        std::size_t pending_push_credits_{};
        // Null once this attempt's QUIC transport and socket are closed; the
        // engine then retires remaining parser state without peer input.
        session_owner_type session_;
    };
    using attempt_owner_type = std::unique_ptr<attempt_type, pmr_object_deleter<attempt_type>>;

    static void on_push_event(void* context, const http3_connection_event& event);
    static void on_origin_event(void* context, const http3_connection_event& event);
    [[nodiscard]] push_list_type::iterator find_push(request_id_type id) noexcept;
    [[nodiscard]] push_list_type::iterator find_push_by_stream(std::uint64_t stream_id) noexcept;
    void finish_push(push_list_type::iterator push, outcome_type outcome);
    void settle_push(std::uint64_t push_id) noexcept;
    [[nodiscard]] bool sweep_pushes();
    [[nodiscard]] bool flush_push_control();
    void require_owner_thread() const;
    static void on_receive_body_budget_released(void* context) noexcept;
    static void on_response_event(void* context, const http3_connection_event& event);
    void wake_receive_driver() noexcept;
    [[nodiscard]] http3_quic_client_socket_session* live_session() const noexcept {
        return attempt_ ? attempt_->session_.get() : nullptr;
    }
    [[nodiscard]] attempt_type& attempt();
    [[nodiscard]] request_list_type::iterator find(request_id_type id) noexcept;
    [[nodiscard]] request_list_type::const_iterator find(request_id_type id) const noexcept;
    [[nodiscard]] submission_type submit_impl(http_client_request_storage request,
        std::optional<time_point_type> deadline, http_client_response_state* response);
    [[nodiscard]] task<void> drive();
    [[nodiscard]] task<bool> drive_endpoint(const asio::ip::udp::endpoint& peer, time_point_type connect_deadline);
    void begin_attempt(const asio::ip::udp::endpoint& peer);
    [[nodiscard]] task<void> close_attempt(time_point_type latest);
    void rearm_attempt_requests();
    void rearm_early_request(request_type& request);
    [[noreturn]] void fail_connection(outcome_type outcome, std::uint64_t close_code, const char* message);
    void cache_path_migration() noexcept;
    [[nodiscard]] bool sweep(bool requests_may_start, bool early_data_only = false);
    [[nodiscard]] bool receive_peer_streams();
    [[nodiscard]] bool replay_rejected_early_streams();
    [[nodiscard]] bool receive_requests();
    [[nodiscard]] bool receive_datagrams();
    [[nodiscard]] bool drive_request_writers(bool early_data_only = false);
    [[nodiscard]] bool drive_critical_output();
    void finish_request(request_type& request, outcome_type outcome);
    void maybe_release_response_request(request_type& request) noexcept;
    void reap_released_response_requests() noexcept;
    void finish_all(outcome_type outcome) noexcept;
    [[nodiscard]] bool retire_request(request_type& request, bool graceful = false) noexcept;
    [[nodiscard]] std::optional<time_point_type> next_deadline(
        std::optional<time_point_type> connect_deadline) const noexcept;

    std::thread::id owner_thread_;
    asio::io_context& io_;
    const worker_handle& worker_;
    task_scope& pool_tasks_;
    http3_quic_client_tls_context& tls_;
    std::pmr::memory_resource* resource_;
    std::pmr::string host_;
    std::pmr::string authority_;
    std::uint16_t port_;
    ruvia::quic_version initial_version_;
    bool enable_early_data_{};
    time_point_type::duration connect_timeout_;
    time_point_type::duration idle_timeout_;
    std::optional<time_point_type::duration> write_timeout_;
    std::size_t max_requests_;
    std::size_t max_response_bytes_;
    http3_client_sans_io_response_limits response_limits_;
    http3_quic_client_endpoint_resolver resolver_;
    // The connection-local budget remains for sans-I/O storage and terminal
    // response bodies. Receive-state leases may instead borrow the pool owner.
    http3_client_body_budget body_budget_;
    http3_client_body_budget* receive_body_budget_{};
    lifecycle_notification_type lifecycle_notification_{};
    http3_client_origin_observer origin_observer_{};
    http3_client_push_observer push_observer_{};
    http3_client_body_budget::wake_registration_type receive_body_budget_wake_;
    request_list_type requests_;
    push_list_type pushes_;
    // Declared after body_budget_, which its engine's reservations return to.
    attempt_owner_type attempt_;
    request_id_type next_request_id_{};
    std::size_t retained_result_body_bytes_{};
    bool running_{};
    std::uint64_t quic_generation_{};
    std::uint64_t next_path_migration_id_{1};
    std::uint64_t quic_connection_migration_id_{};
    std::uint64_t quic_path_migration_generation_{};
    std::optional<ruvia::quic_path_migration> quic_path_migration_{};
    bool starting_{};
    bool stopping_{};
    bool draining_{};
    bool terminal_{};
    outcome_type terminal_failure_{outcome_type::transport_error};
    // RFC 9114 code for the closing CONNECTION_CLOSE; unset means H3_NO_ERROR
    // unless the driver failed unexpectedly.
    std::optional<std::uint64_t> close_error_code_{};
};

}  // namespace ruvia::detail
