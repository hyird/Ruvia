#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_connection.h"

#include "context/context_services.h"
#include "http3/http3_buffered_request_dispatch.h"
#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_server_body_budget.h"
#include "http3/http3_server_stream_input.h"
#include "http3/http3_stream_buffer.h"

namespace ruvia::detail {

class route_table;
struct http_server_options;
class http3_ready_scheduler;

struct http3_server_datagram_output final {
    void* context_{};
    void (*send_)(void*, std::uint64_t, std::span<const std::byte>){};
};
struct http3_server_connection_config final {
    std::uint64_t epoch_{};
    std::uint64_t connection_generation_{};
    http3_sans_io_session_limits session_{};
    std::size_t max_tracked_streams_{32};
    connection_scanner* connection_scanner_{};
    asio::any_io_executor executor_{};
    http3_server_datagram_output datagram_output_{};
};

// Worker-affine, transport-independent owner for one HTTP/3 server connection.
// Ordinary request bodies are buffered; explicit stream routes and websocket
// CONNECT streams use bounded input. It accepts one routed block/control at a time, never
// drains or stops either shared buffer, and never waits for capacity.
// All borrowed owners, including context_services' worker/token/capability and
// connection-metadata borrows, must outlive this object and its joined tasks.
// Stop sources observed during publication must request stop on this worker:
// stop_token callbacks are synchronous and this owner intentionally has no
// cross-thread queue or buffer side channel.
// The optional shared body budget must also outlive this owner and every request
// lease it starts; join all children before retiring either owner. Destruction
// also requires transport intents to be handed off, physical retirement to be
// confirmed, or full-connection retirement responsibility to be taken over.
class http3_server_connection final {
    struct request_entry_type;
    struct rejection_entry_type;
    struct request_index_slot_type;

public:
    using dispatch_type = http3_buffered_request_dispatch;
    using input_type = http3_server_stream_input;
    using session = http3_sans_io_session_engine;

    enum class transport_intent_kind_type : std::uint8_t { stream_reset,
        open_push_stream,
        connection_close };

    struct push_stream_open_result_type final {
        enum class status_type : std::uint8_t { opened,
            unavailable,
            stopped };
        status_type status_{status_type::unavailable};
        std::uint64_t stream_id_{};
    };

    enum class transport_close_reason_type : std::uint8_t {
        none,
        local_stop,
        request_retirement_failure,
        entry_retirement_failure,
        unexpected_request_state,
        request_construction_failure,
        handler_failure,
        publish_cancellation,
        publish_failure,
        connection_protocol_error,
        request_index_capacity_exhausted,
        input_capacity_exhausted,
        final_size_error,
        input_stopped,
        session_not_ready,
        dispatch_start_failure,
        transport_intent_capacity_exhausted,
        transport_intent_sequence_exhausted,
    };

    struct transport_intent_token_type final {
        transport_intent_kind_type kind_{transport_intent_kind_type::stream_reset};
        http3_stream_id id_{};
        std::uint64_t sequence_{};

        friend bool operator==(const transport_intent_token_type& left,
            const transport_intent_token_type& right) noexcept {
            return left.kind_ == right.kind_ && left.id_.epoch_ == right.id_.epoch_ &&
                   left.id_.connection_generation_ == right.id_.connection_generation_ &&
                   left.id_.stream_id_ == right.id_.stream_id_ && left.id_.push_id_ == right.id_.push_id_ && left.sequence_ == right.sequence_;
        }
    };

    struct transport_intent_type final {
        transport_intent_token_type token_{};
        // Exact QUIC RESET_STREAM code. This is not buffer control.value,
        // which remains reserved for a FIN's cumulative byte count.
        http3_connection_error_code stream_reset_error_code_{
            http3_connection_error_code::request_cancelled};
        transport_close_reason_type close_reason_{transport_close_reason_type::none};
        // Suggested HTTP/3 application error code for connection close; absent
        // when the typed reason alone determines transport policy.
        std::optional<http3_connection_error_code> connection_error_code_{};
    };

    // The external channel lifecycle owner uses this typed takeover only after
    // it has accepted responsibility for closing the full connection at global
    // stop. This is not a transport-retired confirmation. Both identity fields
    // must match the owner whose pending close is being transferred.
    struct transport_retirement_takeover_type final {
        std::uint64_t epoch_{};
        std::uint64_t connection_generation_{};
    };
    // A confirmation means the transport owner has already physically retired
    // this connection. It is distinct from handing off an intent or peer receipt.
    struct transport_retirement_confirmation_type final {
        std::uint64_t epoch_{};
        std::uint64_t connection_generation_{};
    };

    enum class event_status_type : std::uint8_t {
        accepted,
        dispatched,
        rejected,
        stream_cancelled,
        input_rejected,
        protocol_error,
        connection_closed,
        session_not_ready,
        request_index_full,
        dispatch_start_failed,
        admission_closed,
        wrong_worker,
    };

    struct event_result_type final {
        event_status_type status_{event_status_type::input_rejected};
        input_type::result_type input_{};
        session::rejection_type rejection_{session::rejection_type::none};
        // The caller still owns transport policy. True means it must close this
        // connection; no peer RESET or "unprocessed request" evidence is implied.
        bool connection_close_required_{};
    };

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

    struct request_info_type final {
        request_status_type status_{request_status_type::unknown};
        session::rejection_type rejection_{session::rejection_type::none};
        std::optional<dispatch_type::run_status_type> run_status_{};
    };

    enum class publish_status_type : std::uint8_t {
        no_ready_request,
        attempted,
        wrong_worker,
    };

    enum class work_lane_type : std::uint8_t { data,
        control,
        local };

    struct work_lanes_type final {
        bool data_{};
        bool control_{};
        bool local_{};

        [[nodiscard]] constexpr bool contains(work_lane_type lane) const noexcept {
            switch (lane) {
                case work_lane_type::data:
                    return data_;
                case work_lane_type::control:
                    return control_;
                case work_lane_type::local:
                    return local_;
            }
            return false;
        }
    };

    struct work_state_type final {
        work_lanes_type runnable_{};
        work_lanes_type blocked_{};
        std::size_t runnable_count_{};
        std::size_t blocked_count_{};
        bool wrong_worker_{};
    };

    struct worker_activation_type final {
        work_state_type work_{};
        std::optional<transport_intent_token_type> transport_intent_{};
        bool input_capacity_available_{false};
    };

    // Borrowed typed activation endpoint. The endpoint is called only on this
    // owner's worker and must only mark/enqueue the supplied snapshot; it must
    // not synchronously publish, destroy, or re-enter the owner.
    struct activation_ref_type final {
        using activate_type = void (*)(void* context, std::uint64_t epoch,
            std::uint64_t connection_generation, std::uint64_t slot_generation,
            const worker_activation_type& activation) noexcept;

        void* context_{};
        activate_type activate_{};
        std::uint64_t slot_generation_{};

        [[nodiscard]] bool valid() const noexcept {
            return context_ != nullptr && activate_ != nullptr && slot_generation_ != 0;
        }
    };

    struct publish_attempt_type final {
        publish_status_type status_{publish_status_type::no_ready_request};
        std::uint64_t stream_id_{};
        std::optional<ruvia::http3_critical_stream_output::stream_kind> critical_kind_{};
        // When status is attempted this is the exact result returned by the
        // selected dispatch, including the buffer backpressure lane.
        dispatch_type::publish_result_type publication_{};
    };

    http3_server_connection(const route_table& routes_value, worker_memory& worker_value,
        context_services services, const http_server_options& options,
        http3_stream_buffer& outbound, activation_ref_type activation,
        http3_server_connection_config config = {});
    // Shares a worker-owned buffered-body budget with other connections. The
    // budget must outlive this connection and every request lease it starts.
    http3_server_connection(const route_table& routes_value, worker_memory& worker_value,
        context_services services, const http_server_options& options,
        http3_stream_buffer& outbound, activation_ref_type activation,
        http3_server_body_budget& body_budget,
        http3_server_connection_config config = {});
    ~http3_server_connection();
    http3_server_connection(const http3_server_connection&) = delete;
    http3_server_connection& operator=(const http3_server_connection&) = delete;
    http3_server_connection(http3_server_connection&&) = delete;
    http3_server_connection& operator=(http3_server_connection&&) = delete;

    // The caller routes exactly one borrowed block/control here and releases a
    // block after return. A validated request FIN automatically starts one
    // buffered dispatch when the session is ready.
    [[nodiscard]] event_result_type accept_data(const http3_stream_buffer::borrowed_block& block) &;
    [[nodiscard]] bool resume_qpack_input() noexcept;
    [[nodiscard]] bool can_accept_input(std::uint64_t stream_id, std::size_t wire_bytes) const noexcept;
    event_result_type accept_data(const http3_stream_buffer::borrowed_block&) && = delete;
    // Worker-local peer unidirectional bytes bypass request DATA storage but
    // still enter the authoritative input/session parser and error policy.
    [[nodiscard]] event_result_type accept_peer_stream_data(http3_stream_id id, std::span<const std::byte> bytes) &;
    [[nodiscard]] event_result_type accept_control(const http3_stream_control& control) &;
    event_result_type accept_control(const http3_stream_control&) && = delete;

    // A worker-affine snapshot of the active intrusive queues. DATA/CONTROL
    // blocked entries are disjoint; local work never waits for buffer capacity.
    [[nodiscard]] work_state_type work_state() const noexcept;

    // Makes at most one nonblocking publish_step attempt from an eligible
    // runnable lane. Backpressure parks the request on the exact buffer lane;
    // it is not retried until reactivate_blocked() is called for that lane.
    [[nodiscard]] publish_attempt_type publish_one(work_lanes_type eligible_lanes) & noexcept;
    publish_attempt_type publish_one(work_lanes_type) && = delete;

    // Explicit capacity notification: reactivates only requests parked on the
    // named DATA/CONTROL lanes. The caller is already handling the capacity
    // event; no independent transport or buffer operation is performed.
    [[nodiscard]] std::size_t reactivate_blocked(work_lanes_type lanes) & noexcept;
    std::size_t reactivate_blocked(work_lanes_type) && = delete;

    // Synchronous and idempotent. Returns true only on the transition that
    // requires the external owner to close the transport. It logically stops
    // input, records a persistent typed close intent, cancels active handlers,
    // and wakes the scheduler; it does not touch the transport or either shared
    // buffer.
    void receive_datagram(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool request_stop() & noexcept;
    bool request_stop() && = delete;

    // Stop/admission must be closed before joining. Even an empty task_scope is
    // actually joined; the returned task is lazy, so discarding it has no effect.
    [[nodiscard]] task<void> join() &;
    task<void> join() && = delete;

    [[nodiscard]] request_info_type request_info(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::size_t active_request_count() const noexcept;
    // An unfinished rejected request retains one bounded, worker-owned receive record.
    [[nodiscard]] std::size_t active_rejection_count() const noexcept;
    [[nodiscard]] std::size_t active_task_count() const noexcept;
    [[nodiscard]] std::size_t ready_request_count() const noexcept;
    [[nodiscard]] std::size_t tracked_request_count() const noexcept;
    [[nodiscard]] std::size_t active_session_stream_count() const noexcept;
    // A worker-side predicate for the admission_sealed generation handshake.
    // It excludes persistent peer-unidirectional streams, and waits for every
    // admitted request stream, active handler, rejection and output publication
    // to become terminal. Terminal request frames may finish their deferred local
    // cleanup during transport retirement; the owner still joins them before reuse.
    [[nodiscard]] bool drain_ready(std::size_t expected_admitted_requests) const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    // Sticky close intent. Callers must not rely only on the event result that
    // first reported the failure, since their wakeup/result may be coalesced.
    [[nodiscard]] bool transport_close_required() const noexcept;

    // Worker-affine intent access. Intents are returned by value, with
    // connection close taking priority over the intrusive per-stream reset
    // chain. Ack means the transport owner has
    // reliably accepted responsibility for the intent; it does not mean a peer
    // observed the operation or that the transport has retired. A close ACK
    // settles only its own token; every reset remains independently owed. Ack
    // never sends a notification; the caller owns the buffer notify obligation
    // from a successful reset-control send. join() alone is not transport
    // retirement or responsibility transfer.
    [[nodiscard]] std::optional<transport_intent_type> peek_transport_intent() const noexcept;
    [[nodiscard]] bool ack_transport_intent(const transport_intent_token_type& token,
        std::optional<push_stream_open_result_type> opened = {}) & noexcept;
    bool ack_transport_intent(const transport_intent_token_type&) && = delete;
    [[nodiscard]] bool take_over_transport_retirement(
        transport_retirement_takeover_type takeover) & noexcept;
    bool take_over_transport_retirement(transport_retirement_takeover_type) && = delete;
    [[nodiscard]] bool confirm_transport_retired(
        transport_retirement_confirmation_type confirmation) & noexcept;
    bool confirm_transport_retired(transport_retirement_confirmation_type) && = delete;
    [[nodiscard]] std::size_t pending_transport_intent_count() const noexcept;
    [[nodiscard]] bool transport_retired() const noexcept;

private:
    friend class http3_ready_scheduler;
    friend struct http3_server_connection_reset_intent_test_access;
    friend struct http3_worker_server_test_access;

    static constexpr std::size_t no_intent_slot = static_cast<std::size_t>(-1);
    static constexpr std::uint64_t reserved_close_intent_sequence =
        static_cast<std::uint64_t>(-1);

    enum class reset_intent_origin_type : std::uint8_t { local_cancellation,
        stream_protocol_error };

    enum class queue_kind_type : std::uint8_t {
        none,
        data_runnable,
        control_runnable,
        local_runnable,
        data_blocked,
        control_blocked,
    };

    struct intrusive_queue_type final {
        request_index_slot_type* head_{};
        request_index_slot_type* tail_{};
    };

    struct pending_interim_response_type final {
        std::pmr::vector<char> frame_;
        std::size_t offset_{};
    };

    struct request_index_slot_type final {
        std::uint64_t stream_id_{};
        std::optional<std::uint64_t> push_id_{};
        bool push_cancelled_{};
        std::optional<std::pmr::vector<char>> push_prefix_{};
        request_entry_type* entry_{};
        std::optional<pending_interim_response_type> interim_response_{};
        std::uint64_t response_prelude_bytes_{};
        bool request_started_{};
        rejection_entry_type* rejection_entry_{};
        request_index_slot_type* queue_previous_{};
        request_index_slot_type* queue_next_{};
        queue_kind_type queue_{queue_kind_type::none};
        request_status_type status_{request_status_type::unknown};
        session::rejection_type rejection_{session::rejection_type::none};
        std::optional<dispatch_type::run_status_type> run_status_{};
        std::size_t reset_intent_previous_{no_intent_slot};
        std::size_t reset_intent_next_{no_intent_slot};
        std::uint64_t reset_intent_sequence_{};
        http3_connection_error_code reset_intent_error_code_{
            http3_connection_error_code::request_cancelled};
        reset_intent_origin_type reset_intent_origin_{reset_intent_origin_type::local_cancellation};
        bool reset_intent_pending_{};
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

    // Owns links, counts and round-robin scheduling. The connection decides
    // publication eligibility before handing a record to this scheduler.
    class output_scheduler final {
    public:
        [[nodiscard]] bool enqueue(request_index_slot_type& slot, queue_kind_type kind) noexcept;
        void remove(request_index_slot_type& slot) noexcept;
        [[nodiscard]] request_index_slot_type* select(work_lanes_type lanes) const noexcept;
        [[nodiscard]] const intrusive_queue_type& queue(queue_kind_type kind) const noexcept;
        [[nodiscard]] std::size_t ready() const noexcept {
            return ready_;
        }
        [[nodiscard]] std::size_t blocked() const noexcept {
            return blocked_;
        }
        void advance() noexcept;

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

    // The intent ledger survives handoff and physical retirement until every
    // borrowed transport token is acknowledged by its exact sequence.
    class transport_retirement final {
    public:
        enum class reset_result { queued,
            unavailable,
            capacity_exhausted,
            sequence_exhausted };
        transport_retirement(request_index& requests, std::size_t capacity, std::uint64_t epoch, std::uint64_t generation) noexcept;
        [[nodiscard]] reset_result enqueue_reset(request_index_slot_type& slot, http3_connection_error_code code, reset_intent_origin_type origin) noexcept;
        [[nodiscard]] std::optional<transport_intent_type> next_intent() const noexcept;
        [[nodiscard]] bool acknowledge(const transport_intent_token_type& token) noexcept;
        void observe_peer_reset(request_index_slot_type& slot) noexcept {
            unlink_reset(slot);
        }
        void require_close(transport_close_reason_type reason, std::optional<http3_connection_error_code> code) noexcept;
        [[nodiscard]] bool take_over() noexcept;
        [[nodiscard]] bool confirm() noexcept;
        [[nodiscard]] bool sequence_available() const noexcept;
        [[nodiscard]] std::uint64_t allocate_sequence() noexcept;
        [[nodiscard]] std::size_t pending() const noexcept {
            return reset_count_ + static_cast<std::size_t>(close_pending_);
        }
        [[nodiscard]] bool close_pending() const noexcept {
            return close_pending_;
        }
        [[nodiscard]] bool close_required() const noexcept {
            return close_required_;
        }
        [[nodiscard]] bool handed_off() const noexcept {
            return handed_off_;
        }
        [[nodiscard]] bool taken_over() const noexcept {
            return taken_over_;
        }
        [[nodiscard]] bool retired() const noexcept {
            return retired_;
        }

    private:
        void unlink_reset(request_index_slot_type& slot) noexcept;
        request_index& requests_;
        const std::size_t capacity_;
        const std::uint64_t epoch_;
        const std::uint64_t generation_;
        std::size_t reset_head_{no_intent_slot};
        std::size_t reset_tail_{no_intent_slot};
        std::size_t reset_count_{};
        std::uint64_t next_sequence_{1};
        transport_close_reason_type close_reason_{transport_close_reason_type::none};
        std::optional<http3_connection_error_code> close_code_{};
        bool close_pending_{};
        bool close_required_{};
        bool handed_off_{};
        bool taken_over_{};
        bool retired_{};
    };

    struct entry_retirement_type;
    struct request_retirement_type;
    struct pending_push_type;

    [[nodiscard]] task<bool> push_request(std::uint64_t parent_stream_id, http_push_request_view request);
    void process_push_cancellations() noexcept;
    void observe_push_cancellation(std::uint64_t push_id) noexcept;
    void retire_request_input(std::uint64_t stream_id) noexcept;

    [[nodiscard]] task<void> run_request(std::uint64_t stream_id);
    [[nodiscard]] event_result_type handle_input_result(std::uint64_t stream_id,
        input_type::result_type result, bool from_control);
    [[nodiscard]] event_result_type admit_finished_request(std::uint64_t stream_id,
        input_type::result_type result);
    [[nodiscard]] event_result_type start_rejection(std::uint64_t stream_id,
        session::rejection_type rejection, input_type::result_type result, bool receive_finished);
    [[nodiscard]] event_result_type retire_rejected_receive(std::uint64_t stream_id,
        input_type::result_type result, bool reset);
    [[nodiscard]] publish_attempt_type publish_rejection(request_index_slot_type& slot,
        rejection_entry_type& entry) noexcept;
    void cancel_rejection_output(rejection_entry_type& entry, request_status_type status) noexcept;
    void finish_rejection(rejection_entry_type& entry) noexcept;
    void enqueue_runnable(request_index_slot_type& slot, queue_kind_type kind, bool notify) noexcept;
    [[nodiscard]] bool queue_continue_response(std::uint64_t stream_id);
    [[nodiscard]] publish_attempt_type publish_interim_response(request_index_slot_type& slot) noexcept;
    void enqueue_for_demand(request_index_slot_type& slot, bool notify) noexcept;
    void enqueue_blocked(request_index_slot_type& slot, dispatch_type::publish_block_reason_type reason) noexcept;
    [[nodiscard]] request_index_slot_type* select_runnable(work_lanes_type eligible_lanes) const noexcept;
    [[nodiscard]] bool reactivate_blocked_one(work_lane_type lane) & noexcept;
    [[nodiscard]] intrusive_queue_type& queue_for(queue_kind_type kind) noexcept;
    [[nodiscard]] const intrusive_queue_type& queue_for(queue_kind_type kind) const noexcept;
    void publication_stopped(request_entry_type& entry) noexcept;
    void cancel_deadline_entry(request_entry_type& entry) noexcept;
    void cancel_entry(request_entry_type& entry, request_status_type status) noexcept;
    void stop_entries(bool input_already_stopped) noexcept;
    void notify_activation() noexcept;
    [[nodiscard]] worker_activation_type activation_snapshot() const noexcept;
    [[nodiscard]] bool detach_activation_after_join() noexcept;
    [[nodiscard]] bool enqueue_reset_intent(request_index_slot_type& slot,
        http3_connection_error_code error_code = http3_connection_error_code::request_cancelled,
        reset_intent_origin_type origin = reset_intent_origin_type::local_cancellation) noexcept;
    void require_connection_close(transport_close_reason_type reason,
        std::optional<http3_connection_error_code> error_code = {}) noexcept;
    void finish_entry(request_entry_type& entry) noexcept;
    void initialize();
    http3_server_datagram_output datagram_output_{};
    [[nodiscard]] bool on_worker() const noexcept;
    [[nodiscard]] bool attach_tunnel_scanner(std::uint64_t stream_id,
        connection_scanner::entry_type& entry) noexcept;
    void tunnel_output_ready(std::uint64_t stream_id) noexcept;
    void abort_tunnel(std::uint64_t stream_id) noexcept;
    static bool attach_tunnel_scanner_thunk(void* context, std::uint64_t stream_id,
        connection_scanner::entry_type& entry) noexcept;
    static void tunnel_output_ready_thunk(void* context, std::uint64_t stream_id) noexcept;
    static void request_input_consumed_thunk(void* context) noexcept;
    static void abort_tunnel_thunk(void* context, std::uint64_t stream_id) noexcept;
    [[nodiscard]] static std::size_t index_capacity(std::size_t max_tracked_streams);

    const context_services services_;
    worker_memory& worker_;
    const route_table& routes_;
    const http_server_options& options_;
    http3_stream_buffer& outbound_;
    activation_ref_type activation_;
    const std::uint64_t epoch_;
    const std::uint64_t connection_generation_;
    const std::size_t max_tracked_streams_;
    connection_scanner* connection_scanner_{};
    asio::any_io_executor executor_;
    session session_;
    input_type input_;
    task_scope tasks_;
    request_index requests_;
    output_scheduler output_;
    transport_retirement retirement_;
    pending_push_type* pending_push_head_{};
    pending_push_type* pending_push_tail_{};
    pending_push_type* push_intent_head_{};
    pending_push_type* push_intent_tail_{};
    std::size_t pending_push_count_{};
    std::size_t pending_push_intent_count_{};
    std::uint64_t next_push_id_{};
    bool critical_output_blocked_{};
    bool prefer_critical_output_{true};
    std::size_t next_critical_output_{};
    bool admission_closed_{};
    bool stop_requested_{};
    bool ever_spawned_{};
    bool join_started_{};
    bool join_completed_{};
};

}  // namespace ruvia::detail
