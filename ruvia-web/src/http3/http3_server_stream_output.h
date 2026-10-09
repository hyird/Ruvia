#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <thread>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/quic_connection.h"

#include "http3/http3_stream_buffer.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

struct http3_server_stream_output_config final {
    // Bounds active stream slots plus retained tombstones; entries are not recycled.
    std::size_t max_tracked_streams_{32};
    // Bound retained borrowed_block nodes; size to the local buffer's block capacity.
    std::size_t max_queued_blocks_{32};
    // Bounds service operations per turn; a scan visits at most the occupied slots.
    std::size_t max_drive_work_items_{16};
    // Per-response-stream write inactivity timeout; nullopt disables it.
    std::optional<std::chrono::milliseconds> write_timeout_{};
};

// Worker-affine response egress for the wire half that already routed
// buffer messages for one QUIC connection. It never drains the shared buffer.
// Accepted blocks remain borrowed until all bytes are accepted by SSL or the
// corresponding stream/connection SSL owner has been retired. The transport and
// worker-owned memory resource must outlive this object; destruction with live streams or
// borrowed blocks is a contract violation, so stop() explicitly before teardown.
class http3_server_stream_output final {
public:
    using stream_id_type = std::uint64_t;
    using transport_error_type = ruvia::quic_operation_status;
    using stream_write_type = ruvia::quic_stream_write_result;
    struct stream_termination_type final {
        transport_error_type send_{transport_error_type::would_block};
        transport_error_type close_{transport_error_type::would_block};
    };

    enum class status_type : std::uint8_t {
        accepted,
        backpressured,
        writable,
        fin_deferred,
        duplicate_fin,
        finished,
        reset,
        cancelled,
        connection_closed,
        foreign_epoch,
        stale_connection,
        invalid_input,
        invalid_stream_id,
        closed_stream,
        capacity_exhausted,
        final_size_error,
        transport_error,
        stopped,
        unsafe_to_release,
        idle,
        progress,
    };

    // finished records accepted local FIN plus wrapper retirement, not peer delivery/ACK.
    enum class stream_state_type : std::uint8_t {
        open,
        fin_pending,
        stopping,
        finished,
        reset,
        cancelled,
        failed,
        connection_closed,
    };

    struct result_type final {
        status_type status_{status_type::accepted};
        transport_error_type transport_error_{transport_error_type::accepted};
        transport_error_type write_status_{transport_error_type::accepted};
        stream_termination_type termination_{};
    };

    struct drive_result_type final {
        status_type status_{status_type::idle};
        // Budgeted service operations, including retryable SSL write attempts.
        std::size_t operations_{};
        std::size_t scanned_slots_{};
        std::size_t write_calls_{};
        std::size_t accepted_bytes_{};
        std::size_t would_block_writes_{};
        std::size_t finished_streams_{};
        std::size_t timed_out_streams_{};
        stream_id_type last_stream_id_{};
        stream_id_type error_stream_id_{};
        transport_error_type write_status_{transport_error_type::accepted};
        transport_error_type transport_error_{transport_error_type::accepted};
        stream_termination_type termination_{};
        // True for accepted bytes, consumed blocks, or completed/retired stream work.
        bool made_progress_{};
        // The current bounded scan has more slots, or a completed scan with
        // progress/activity needs another pass. An inactive WANT-only scan parks.
        bool needs_reschedule_{};
    };

    struct stream_info_type final {
        stream_id_type stream_id_{};
        stream_state_type state_{stream_state_type::open};
        std::uint64_t received_wire_bytes_{};
        std::uint64_t accepted_wire_bytes_{};
        std::uint64_t queued_wire_bytes_{};
        std::optional<std::uint64_t> final_wire_bytes_{};
        std::size_t queued_blocks_{};
        transport_error_type last_write_status_{transport_error_type::accepted};
        transport_error_type finish_error_{transport_error_type::accepted};
        // SSL_stream_conclude accepted local FIN; receive completion is tracked
        // by the transport until read_stream observes peer EOF or RESET.
        bool send_fin_accepted_{};
        bool timed_out_{};
        stream_termination_type termination_{};
        std::optional<std::uint64_t> push_id_{};
    };

    http3_server_stream_output(ruvia::quic_connection& connection,
        std::pmr::memory_resource* resource, std::uint64_t epoch,
        std::uint64_t connection_generation, http3_server_stream_output_config config = {});
    http3_server_stream_output(ruvia::quic_connection& connection, worker_memory& worker_value,
        std::uint64_t epoch, std::uint64_t connection_generation,
        http3_server_stream_output_config config = {});
    ~http3_server_stream_output();
    http3_server_stream_output(const http3_server_stream_output&) = delete;
    http3_server_stream_output& operator=(const http3_server_stream_output&) = delete;
    http3_server_stream_output(http3_server_stream_output&&) = delete;
    http3_server_stream_output& operator=(http3_server_stream_output&&) = delete;

    // On accepted ownership moves into the bounded FIFO. On backpressured,
    // identity mismatch, or other non-acceptance the caller still owns block and
    // must retain or explicitly release it. IDs outside ordinary client bidi
    // and explicitly bound push streams fail the connection closed. Mismatched
    // identities are detected before any transport operation.
    [[nodiscard]] result_type accept_data(http3_stream_buffer::borrowed_block& block);

    // Bind only a server UNI stream actually opened by this wire half.
    // The binding is immutable and must precede handler response publication.
    [[nodiscard]] result_type register_push_stream(stream_id_type stream_id, std::uint64_t push_id);
    [[nodiscard]] result_type accept_critical_data(http3_stream_buffer::borrowed_block& block, stream_id_type stream_id);
    [[nodiscard]] result_type accept_control(const http3_stream_control& control);
    [[nodiscard]] result_type cancel_stream(stream_id_type stream_id,
        std::uint64_t error_code = static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));

    // Performs at most max_drive_work_items_ service operations and scans at most
    // one table per call. A write item is one SSL attempt; WANT counts as an
    // operation but not progress, so idle may accompany nonzero operations.
    // progress reflects real progress only. A FIN item submits the send FIN
    // and attempts direction-complete retirement. Persistent scan state requests
    // another turn while slots remain; productive or activity-requested scans
    // get one more pass, while a full no-progress scan parks. WANT retains the
    // exact borrowed address, size and contents while later streams get a turn.
    [[nodiscard]] drive_result_type drive();

    // Reopens a parked scan or requests a fresh pass after owner-thread UDP,
    // timer, QUIC, or stream-read activity. New accepted data, FIN, and stream
    // retirement do so automatically.
    void notify_transport_activity();

    // Marks all live streams terminal before resetting/closing their SSL wrappers.
    // If an individual wrapper cannot be proven retired, closes the whole bound
    // connection before returning its borrowed blocks. Repeated calls are safe;
    // a failed close may be retried. unsafe_to_release forbids destruction; queued
    // blocks remain borrowed until a later stop succeeds.
    [[nodiscard]] result_type stop();

    [[nodiscard]] std::optional<stream_info_type> stream_info(stream_id_type stream_id) const;
    // tracked_stream_count includes retained terminal tombstones. live_stream_count
    // counts response writes begun but not yet locally FINished; pending_stream_count
    // counts live streams with queued data or a locally-ready FIN.
    [[nodiscard]] std::size_t tracked_stream_count() const;
    [[nodiscard]] std::size_t live_stream_count() const;
    [[nodiscard]] std::size_t pending_stream_count() const;
    [[nodiscard]] std::size_t queued_block_count() const;
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] bool connection_retired() const;

private:
    [[nodiscard]] result_type accept_addressed_data(http3_stream_buffer::borrowed_block& block, stream_id_type stream_id, std::uint64_t epoch, std::uint64_t generation);
    static constexpr std::uint32_t no_node = UINT32_MAX;

    enum class identity_status_type : std::uint8_t { match,
        foreign_epoch,
        stale_connection };

    struct stream_slot_type final {
        stream_info_type info_{};
        std::optional<std::chrono::steady_clock::time_point> last_write_activity_{};
        std::uint32_t head_{no_node};
        std::uint32_t tail_{no_node};
        // Stable occupied-slot ring, including tombstones; no second allocation.
        std::size_t next_tracked_slot_{};
        bool occupied_{};
    };

    struct block_node_type final {
        http3_stream_buffer::borrowed_block block_{};
        std::uint32_t next_{no_node};
        std::size_t offset_{};
    };

    void require_owner_thread() const;
    [[nodiscard]] identity_status_type identity_status(const http3_stream_id& id) const noexcept;
    [[nodiscard]] bool valid_response_stream_id(stream_id_type stream_id) const noexcept;
    [[nodiscard]] stream_slot_type* find_stream(stream_id_type stream_id) noexcept;
    [[nodiscard]] const stream_slot_type* find_stream(stream_id_type stream_id) const noexcept;
    [[nodiscard]] stream_slot_type* find_or_create_stream(stream_id_type stream_id);
    [[nodiscard]] bool is_terminal(stream_state_type state) const noexcept;
    [[nodiscard]] bool connection_identity_gone();
    [[nodiscard]] bool close_connection_and_release();
    [[nodiscard]] bool retire_stream(stream_slot_type& slot, stream_state_type terminal_state,
        std::uint64_t error_code);
    void release_node(std::uint32_t index) noexcept;
    void release_stream_queue(stream_slot_type& slot) noexcept;
    void release_all_queues() noexcept;
    void mark_connection_closed() noexcept;
    [[nodiscard]] result_type fail_connection(status_type status,
        transport_error_type error = transport_error_type::closing);
    [[nodiscard]] result_type handle_write_failure(stream_slot_type& slot,
        transport_error_type write_status);
    [[nodiscard]] bool finish_stream(stream_slot_type& slot, transport_error_type& error);
    [[nodiscard]] bool write_timed_out(const stream_slot_type& slot,
        std::chrono::steady_clock::time_point now) const noexcept;
    void request_scan() noexcept;
    [[nodiscard]] static std::size_t table_capacity(std::size_t max_tracked_streams);

    ruvia::quic_connection& connection_;
    const std::uint64_t epoch_;
    const std::uint64_t connection_generation_;
    const std::size_t max_tracked_streams_;
    const std::size_t max_queued_blocks_;
    const std::size_t max_drive_work_items_;
    const std::optional<std::chrono::milliseconds> write_timeout_;
    const std::thread::id owner_thread_;
    std::pmr::vector<stream_slot_type> streams_;
    std::pmr::vector<block_node_type> nodes_;
    std::uint32_t free_node_{no_node};
    std::size_t tracked_stream_count_{};
    std::size_t queued_block_count_{};
    std::size_t first_tracked_slot_{};
    std::size_t last_tracked_slot_{};
    std::size_t round_robin_slot_{};
    std::size_t round_remaining_slots_{};
    bool round_made_progress_{};
    bool scan_again_{};
    bool stopped_{};
    bool connection_retired_{};
    bool retirement_failed_{};
    bool stop_complete_{};
};

}  // namespace ruvia::detail
