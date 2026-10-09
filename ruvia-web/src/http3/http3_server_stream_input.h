#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_stream_buffer.h"

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

// Worker-affine receive adapter for one QUIC connection. The buffer owner
// routes each block/control here individually; this class never drains shared
// queues. DATA byte counts include HTTP/3 frame bytes, not just request body.
// Terminal stream entries stay as tombstones until this connection is retired.
// Streaming consumers are paced by the connection owner before accept_data();
// it retains bounded buffer blocks while the worker body backlog is full.
class http3_server_stream_input final {
    enum class stream_phase_type : std::uint8_t { open,
        finished,
        reset_pending,
        reset,
        cancelled,
        failed,
        connection_closed };

    struct stream_state_type final {
        explicit stream_state_type(std::pmr::memory_resource* resource)
            : pending_bytes_(resource) {}
        std::pmr::string pending_bytes_;
        bool qpack_blocked_{};
        bool pending_fin_{};
        std::uint64_t wire_bytes_{};
        std::optional<std::uint64_t> final_size_;
        std::optional<std::uint64_t> reset_published_bytes_;
        http3_connection_error_code reset_error_code_{
            http3_connection_error_code::request_cancelled};
        stream_phase_type phase_{stream_phase_type::open};
        bool request_stream_{};
        bool request_active_{};
        bool received_early_data_{};
    };

    struct stream_slot_type final {
        explicit stream_slot_type(std::pmr::memory_resource* resource)
            : state_(resource) {}
        std::uint64_t stream_id_{};
        stream_state_type state_;
        bool occupied_{};
    };

public:
    enum class status_type : std::uint8_t {
        fed,
        deferred_fin,
        deferred_qpack,
        deferred_reset,
        finished,
        reset,
        connection_closed,
        local_cancelled,
        ignored_control,
        foreign_epoch,
        stale_connection,
        closed_stream,
        duplicate_fin,
        final_size_error,
        capacity_exhausted,
        invalid_input,
        stopped,
        protocol_error,
    };

    // finished means ordered receive FIN was accepted, not route admission.
    // The owner still inspects session rejection/readiness before dispatch.
    struct result_type final {
        status_type status_{status_type::fed};
        http3_connection_result protocol_{};
    };

    // max_tracked_streams bounds active entries plus retained tombstones. The
    // worker-PMR lookup table is allocated at construction; worker must outlive
    // this input and the associated session. epoch/generation identify this
    // one connection. Bind before the first session feed; thereafter route all
    // receive, cancellation and stop operations through this adapter. Session
    // queries, leases and completed-response release remain direct operations.
    http3_server_stream_input(http3_sans_io_session_engine& session_value, worker_memory& worker_value,
        std::uint64_t epoch, std::uint64_t connection_generation,
        std::size_t max_tracked_streams);
    ~http3_server_stream_input() = default;
    http3_server_stream_input(const http3_server_stream_input&) = delete;
    http3_server_stream_input& operator=(const http3_server_stream_input&) = delete;
    http3_server_stream_input(http3_server_stream_input&&) = delete;
    http3_server_stream_input& operator=(http3_server_stream_input&&) = delete;

    struct resumed_input_type final {
        std::uint64_t stream_id_{};
        result_type result_{};
    };
    [[nodiscard]] bool can_accept_input(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool received_early_data(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::optional<resumed_input_type> resume_qpack() noexcept;

    // Call once for each routed buffer block. A mismatched identity is
    // reported without touching input or session state. The borrowed block is
    // not retained and can be released as soon as this call returns. Final-size
    // inconsistency or tracking-capacity exhaustion stops this input/session;
    // the transport owner must close that connection, not retry the message.
    [[nodiscard]] result_type accept_data(const http3_stream_buffer::borrowed_block& block) noexcept;
    // Peer unidirectional input is consumed synchronously by the same protocol
    // owner, independently of request DATA credits. The HTTP parser owns any
    // incomplete stream-type, control-frame or QPACK instruction fragments.
    // bytes is borrowed only for this call and never retained by this adapter.
    [[nodiscard]] result_type accept_peer_stream_data(http3_stream_id id, std::span<const std::byte> bytes) noexcept;
    // stream_fin.value is the final cumulative wire-byte count. It may precede
    // queued DATA. Peer RESET.value is the transport owner's cumulative successfully
    // published DATA-byte count, not QUIC Final Size; RESET is deferred until
    // that barrier is consumed. Local cancellation/connection-close controls
    // never synthesize a peer RESET. Callers may route controls before DATA.
    [[nodiscard]] result_type accept_control(const http3_stream_control& control) noexcept;

    // After transport termination, route local cancellation through this input
    // (not directly through the session) so queued DATA cannot recreate state.
    // May precede HEADERS; only request stream IDs are accepted. No peer RESET
    // is fabricated, and a dispatch lease continues to pin its storage.
    [[nodiscard]] result_type cancel_request(std::uint64_t stream_id) noexcept;

    // Stop delivery and retire the session. Idempotent; all subsequent matching
    // input is rejected, and existing per-stream tombstones are retained.
    void stop() noexcept;

    [[nodiscard]] std::size_t tracked_stream_count() const noexcept;
    [[nodiscard]] std::size_t observed_request_stream_count() const noexcept;
    [[nodiscard]] std::size_t active_request_stream_count() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;

private:
    [[nodiscard]] status_type identity_status(const http3_stream_id& id) const noexcept;
    [[nodiscard]] result_type accept_bytes(http3_stream_id id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] result_type accept_fin(const http3_stream_control& control) noexcept;
    [[nodiscard]] result_type apply_peer_reset(std::uint64_t stream_id, stream_state_type& state) noexcept;
    [[nodiscard]] result_type final_size_failure() noexcept;
    [[nodiscard]] result_type feed_session(std::uint64_t stream_id, std::string_view bytes,
        bool fin, stream_state_type& state) noexcept;
    [[nodiscard]] static std::size_t table_capacity(std::size_t max_tracked_streams);
    [[nodiscard]] stream_state_type* find_or_create(std::uint64_t stream_id,
        status_type& failure) noexcept;
    void close_for_connection_error() noexcept;
    void finish_request_stream(stream_state_type& state) noexcept;
    void clear_qpack(stream_state_type& state) noexcept;

    http3_sans_io_session_engine& session_;
    const std::uint64_t epoch_;
    const std::uint64_t connection_generation_;
    const std::size_t max_tracked_streams_;
    std::pmr::vector<stream_slot_type> streams_;
    std::size_t tracked_stream_count_{};
    std::size_t observed_request_stream_count_{};
    std::size_t active_request_stream_count_{};
    std::size_t blocked_qpack_count_{};
    std::size_t next_qpack_resume_{};
    bool stopped_{false};
};

}  // namespace ruvia::detail
