#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http_datagram.h"

#include "context/context_services.h"
#include "http3/http3_body_budget.h"
#include "http3/http3_buffered_request_dispatch.h"
#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_server_stream_input.h"
#include "http3/http3_server_stream_output.h"
#include "server/connection_scanner.h"

namespace ruvia::detail {

class route_table;
class http3_connection_driver;
struct http_server_options;

struct http3_server_connection_config final {
    http3_sans_io_session_limits session_{};
    std::size_t max_tracked_streams_{32};
    connection_scanner* connection_scanner_{};
    asio::any_io_executor executor_{};
};

// Worker-affine HTTP owner of one HTTP/3 server connection, owned by its
// http3_connection_driver. QUIC stream input is fed here in order; request
// handlers publish HEADERS/DATA straight from their own buffers to the
// driver's QUIC output, whose stream flow control is the only backpressure.
// Ordinary request bodies are buffered; explicit stream routes and websocket
// CONNECT streams use bounded input. Every transport operation is a
// synchronous call on the driver, which never re-enters this owner.
// All borrowed owners, including context_services' worker/token/capability and
// connection-metadata borrows and the shared body budget, must outlive this
// object and its joined tasks. Stop sources observed during publication must
// request stop on this worker: stop_token callbacks are synchronous.
class http3_server_connection final {
    struct request_entry_type;
    struct rejection_entry_type;
    struct request_index_slot_type;

public:
    using dispatch_type = http3_buffered_request_dispatch;
    using input_type = http3_server_stream_input;
    using session_type = http3_sans_io_session_engine;

    http3_server_connection(const route_table& routes_value, worker_memory& worker_value,
        context_services services, const http_server_options& options, http3_connection_driver& transport,
        http3_body_budget& body_budget, http3_server_connection_config config);
    ~http3_server_connection();
    http3_server_connection(const http3_server_connection&) = delete;
    http3_server_connection& operator=(const http3_server_connection&) = delete;
    http3_server_connection(http3_server_connection&&) = delete;
    http3_server_connection& operator=(http3_server_connection&&) = delete;

    // Input in QUIC order per stream. A validated request FIN starts one
    // dispatch; fatal input closes the transport through the driver.
    void accept_data(std::uint64_t stream_id, bool received_early_data, std::span<const std::byte> bytes) noexcept;
    // Client unidirectional streams (control, QPACK, unknown types).
    void accept_peer_stream_data(std::uint64_t stream_id, std::span<const std::byte> bytes) noexcept;
    void accept_fin(std::uint64_t stream_id) noexcept;
    // Peer RESET_STREAM, request timeout, write timeout or peer STOP_SENDING:
    // the stream is terminal at the transport and its handler is cancelled.
    void accept_reset(std::uint64_t stream_id, http3_connection_error_code code) noexcept;
    // Wire bytes acceptable now; 0 holds QUIC input (flow control is the
    // backpressure) and SIZE_MAX is unpaced. The driver is woken when a held
    // stream's capacity reopens.
    [[nodiscard]] std::size_t input_capacity(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool resume_qpack_input() noexcept;
    // A decoded HTTP Datagram the driver planned for delivery.
    void receive_datagram(const http3_datagram_view& datagram) noexcept;

    // Runs up to budget publish steps over the runnable lanes. True when
    // runnable work remains.
    [[nodiscard]] bool publish(std::size_t budget) noexcept;
    // QUIC made progress: reactivates publishers parked on a blocked or closed
    // stream. A closed stream's handler is cancelled through accept_reset.
    void transport_activity() noexcept;

    // Idempotent; stops input and cancels every handler. True on the first
    // transition. Transport close policy belongs to the driver.
    [[nodiscard]] bool request_stop() noexcept;
    // The QUIC connection is gone: stops input and cancels every handler.
    void transport_retired() noexcept;
    // After request_stop() or transport_retired(); joins every task.
    [[nodiscard]] task<void> join();
    // Graceful GOAWAY drain predicate: every admitted request stream, handler,
    // rejection and publication is terminal. Persistent peer unidirectional
    // streams are excluded.
    [[nodiscard]] bool drain_ready(std::size_t expected_admitted_requests) const noexcept;
    [[nodiscard]] session_type& session() noexcept {
        return session_;
    }

private:
    friend class http3_buffered_request_dispatch;

    enum class queue_kind_type : std::uint8_t {
        none,
        data_runnable,
        control_runnable,
        local_runnable,
        data_blocked,
        control_blocked,
    };

    enum class work_lane_type : std::uint8_t { data,
        control,
        local };

    enum class request_status_type : std::uint8_t {
        unknown,
        admitting,
        running,
        ready_to_publish,
        publishing,
        published,
        rejected,
        cancelled,
        failed,
        protocol_error,
    };

    struct intrusive_queue_type final {
        request_index_slot_type* head_{};
        request_index_slot_type* tail_{};
    };

    struct pending_interim_response_type final {
        http3_headers_frame frame_;
        std::size_t offset_{};
    };

    struct request_index_slot_type final {
        std::uint64_t stream_id_{};
        std::optional<std::uint64_t> push_id_{};
        bool push_cancelled_{};
        std::optional<http3_push_stream_prefix> push_prefix_{};
        request_entry_type* entry_{};
        std::optional<pending_interim_response_type> interim_response_{};
        // A 100 (Continue) head was produced for this stream.
        bool interim_sent_{};
        bool request_started_{};
        rejection_entry_type* rejection_entry_{};
        request_index_slot_type* queue_previous_{};
        request_index_slot_type* queue_next_{};
        queue_kind_type queue_{queue_kind_type::none};
        request_status_type status_{request_status_type::unknown};
        session_type::rejection_type rejection_{session_type::rejection_type::none};
        bool occupied_{};
    };

    // Stable request records and active-entry accounting share one owner.
    class request_index final {
    public:
        request_index(std::pmr::memory_resource* resource, std::size_t capacity);
        [[nodiscard]] request_index_slot_type* find_or_create(std::uint64_t stream_id, bool& created) noexcept;
        [[nodiscard]] request_index_slot_type* find(std::uint64_t stream_id) noexcept;
        [[nodiscard]] const request_index_slot_type* find(std::uint64_t stream_id) const noexcept;
        void attach(request_index_slot_type& slot, request_entry_type& entry) noexcept;
        void attach_rejection(request_index_slot_type& slot, rejection_entry_type& entry) noexcept;
        void retire(request_index_slot_type& slot, request_entry_type& entry) noexcept;
        void retire_rejection(request_index_slot_type& slot, rejection_entry_type& entry) noexcept;
        [[nodiscard]] std::span<request_index_slot_type> slots() noexcept {
            return slots_;
        }
        [[nodiscard]] std::span<const request_index_slot_type> slots() const noexcept {
            return slots_;
        }
        [[nodiscard]] std::size_t tracked() const noexcept {
            return tracked_;
        }
        [[nodiscard]] std::size_t active() const noexcept {
            return active_;
        }
        [[nodiscard]] std::size_t rejections() const noexcept {
            return rejections_;
        }

    private:
        const std::size_t capacity_;
        std::pmr::vector<request_index_slot_type> slots_;
        std::size_t tracked_{};
        std::size_t active_{};
        std::size_t rejections_{};
    };

    // Owns links, counts and round-robin lane selection. The connection
    // decides publication eligibility before handing a record to it.
    class output_scheduler final {
    public:
        [[nodiscard]] bool enqueue(request_index_slot_type& slot, queue_kind_type kind) noexcept;
        void remove(request_index_slot_type& slot) noexcept;
        [[nodiscard]] request_index_slot_type* select() noexcept;
        [[nodiscard]] const intrusive_queue_type& queue(queue_kind_type kind) const noexcept;
        [[nodiscard]] std::size_t ready() const noexcept {
            return ready_;
        }
        [[nodiscard]] std::size_t blocked() const noexcept {
            return blocked_;
        }

    private:
        [[nodiscard]] intrusive_queue_type& queue(queue_kind_type kind) noexcept;
        intrusive_queue_type data_runnable_{};
        intrusive_queue_type control_runnable_{};
        intrusive_queue_type local_runnable_{};
        intrusive_queue_type data_blocked_{};
        intrusive_queue_type control_blocked_{};
        std::size_t ready_{};
        std::size_t blocked_{};
        work_lane_type next_lane_{work_lane_type::data};
    };

    struct entry_retirement_type;
    struct request_retirement_type;

    // Request lifecycle (http3_server_connection.cpp).
    [[nodiscard]] task<void> run_request(std::uint64_t stream_id);
    void handle_input_result(std::uint64_t stream_id, input_type::result_type result);
    [[nodiscard]] bool admit_finished_request(std::uint64_t stream_id);
    void start_rejection(std::uint64_t stream_id, session_type::rejection_type rejection, bool receive_finished);
    void retire_rejected_receive(rejection_entry_type& entry, request_status_type status) noexcept;
    void cancel_stream(request_index_slot_type& slot) noexcept;
    [[nodiscard]] bool queue_continue_response(std::uint64_t stream_id);
    void publication_stopped(request_entry_type& entry) noexcept;
    void cancel_deadline_entry(request_entry_type& entry) noexcept;
    void cancel_entry(request_entry_type& entry, request_status_type status) noexcept;
    void cancel_rejection_output(rejection_entry_type& entry, request_status_type status) noexcept;
    void finish_rejection(rejection_entry_type& entry) noexcept;
    void finish_entry(request_entry_type& entry) noexcept;
    void stop_entries() noexcept;
    void retire_request_input(std::uint64_t stream_id) noexcept;
    void reset_stream(request_index_slot_type& slot,
        http3_connection_error_code code = http3_connection_error_code::request_cancelled) noexcept;
    void close_connection(http3_connection_error_code code) noexcept;
    void notify_activation() noexcept;
    [[nodiscard]] http3_server_stream_output& output() noexcept;
    [[nodiscard]] bool on_worker() const noexcept;

    // Publication (connection_components.cpp).
    void publish_slot(request_index_slot_type& slot) noexcept;
    void publish_entry(request_entry_type& entry) noexcept;
    void publish_rejection(request_index_slot_type& slot, rejection_entry_type& entry) noexcept;
    void publish_interim_response(request_index_slot_type& slot) noexcept;
    void enqueue_runnable(request_index_slot_type& slot, queue_kind_type kind, bool notify) noexcept;
    void enqueue_for_demand(request_index_slot_type& slot, bool notify) noexcept;
    void enqueue_blocked(request_index_slot_type& slot, queue_kind_type kind) noexcept;
    [[nodiscard]] static std::size_t index_capacity(std::size_t max_tracked_streams);

    // Server push (http3_server_push.cpp).
    [[nodiscard]] task<bool> push_request(std::uint64_t parent_stream_id, http_push_request_view request);
    void observe_push_cancellation(std::uint64_t push_id) noexcept;
    void process_push_cancellations() noexcept;
    void cancel_push(request_index_slot_type& slot) noexcept;

    // Tunnels and HTTP Datagrams (http3_tunnel_dispatch.cpp).
    [[nodiscard]] bool attach_tunnel_scanner(std::uint64_t stream_id, connection_scanner::entry_type& entry) noexcept;
    void tunnel_output_ready(std::uint64_t stream_id) noexcept;
    void abort_tunnel(std::uint64_t stream_id) noexcept;
    void send_datagram(std::span<const std::byte> datagram);

    const context_services services_;
    worker_memory& worker_;
    const route_table& routes_;
    const http_server_options& options_;
    const http_request_admission_policy admission_policy_;
    http3_connection_driver& transport_;
    const std::size_t max_tracked_streams_;
    connection_scanner* connection_scanner_{};
    asio::any_io_executor executor_;
    session_type session_;
    input_type input_;
    task_scope tasks_;
    request_index requests_;
    output_scheduler output_;
    std::uint64_t next_push_id_{};
    bool admission_closed_{};
    bool stop_requested_{};
    bool close_requested_{};
    bool transport_retired_{};
    bool ever_spawned_{};
    bool join_started_{};
    bool join_completed_{};
};

}  // namespace ruvia::detail
