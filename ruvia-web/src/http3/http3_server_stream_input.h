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

namespace ruvia {
class worker_memory;
}

namespace ruvia::detail {

// Worker-affine receive adapter for one QUIC connection. The connection owner
// feeds each stream's DATA, FIN and RESET here in QUIC order; byte counts
// include HTTP/3 frame bytes, not just request body. Terminal stream entries
// stay as tombstones until this connection is retired. Streaming consumers are
// paced by the connection owner before accept_data(): a QPACK-blocked stream
// retains its unparsed suffix and accepts no more input until it resumes.
class http3_server_stream_input final {
    enum class stream_phase_type : std::uint8_t { open,
        finished,
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
        deferred_qpack,
        finished,
        reset,
        local_cancelled,
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

    struct resumed_input_type final {
        std::uint64_t stream_id_{};
        result_type result_{};
    };

    // max_tracked_streams bounds active entries plus retained tombstones. The
    // worker-PMR lookup table is allocated at construction; worker must outlive
    // this input and the associated session. Bind before the first session feed;
    // thereafter route all receive, cancellation and stop operations through
    // this adapter. Session queries, leases and completed-response release
    // remain direct operations.
    http3_server_stream_input(http3_sans_io_session_engine& session_value, worker_memory& worker_value,
        std::size_t max_tracked_streams);
    ~http3_server_stream_input() = default;
    http3_server_stream_input(const http3_server_stream_input&) = delete;
    http3_server_stream_input& operator=(const http3_server_stream_input&) = delete;
    http3_server_stream_input(http3_server_stream_input&&) = delete;
    http3_server_stream_input& operator=(http3_server_stream_input&&) = delete;

    // A QPACK-blocked stream holds further QUIC input until it resumes.
    [[nodiscard]] bool qpack_blocked(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool received_early_data(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::optional<resumed_input_type> resume_qpack() noexcept;

    // Request-stream bytes. Tracking-capacity exhaustion or an inconsistent
    // stream state stops this input/session; the owner must close the connection.
    [[nodiscard]] result_type accept_data(std::uint64_t stream_id, bool received_early_data,
        std::span<const std::byte> bytes) noexcept;
    // Peer unidirectional input is consumed synchronously by the same protocol
    // owner. The HTTP parser owns any incomplete stream-type, control-frame or
    // QPACK instruction fragments; bytes are borrowed only for this call.
    [[nodiscard]] result_type accept_peer_stream_data(std::uint64_t stream_id,
        std::span<const std::byte> bytes) noexcept;
    // Peer FIN after every DATA byte of the stream.
    [[nodiscard]] result_type accept_fin(std::uint64_t stream_id) noexcept;
    // The stream's receive side ended without FIN (peer RESET_STREAM or a
    // transport-side termination). Retires the session stream as reset.
    [[nodiscard]] result_type accept_reset(std::uint64_t stream_id) noexcept;

    // After transport termination, route local cancellation through this input
    // (not directly through the session) so later DATA cannot recreate state.
    // May precede HEADERS; only request stream IDs are accepted. No peer RESET
    // is fabricated, and a dispatch lease continues to pin its storage.
    [[nodiscard]] result_type cancel_request(std::uint64_t stream_id) noexcept;

    // Stop delivery and retire the session. Idempotent; all subsequent input
    // is rejected, and existing per-stream tombstones are retained.
    void stop() noexcept;

    [[nodiscard]] std::size_t observed_request_stream_count() const noexcept {
        return observed_request_stream_count_;
    }
    [[nodiscard]] std::size_t active_request_stream_count() const noexcept {
        return active_request_stream_count_;
    }
    [[nodiscard]] bool stopped() const noexcept {
        return stopped_;
    }

private:
    [[nodiscard]] result_type accept_bytes(std::uint64_t stream_id, bool received_early_data,
        std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] result_type invariant_failure() noexcept;
    [[nodiscard]] result_type feed_session(std::uint64_t stream_id, std::string_view bytes,
        bool fin, stream_state_type& state) noexcept;
    [[nodiscard]] result_type protocol_failure(stream_state_type& state,
        const http3_connection_result& result) noexcept;
    [[nodiscard]] static std::size_t table_capacity(std::size_t max_tracked_streams);
    [[nodiscard]] const stream_slot_type* find(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] stream_state_type* find_or_create(std::uint64_t stream_id,
        status_type& failure) noexcept;
    void close_streams() noexcept;
    void finish_request_stream(stream_state_type& state) noexcept;
    void clear_qpack(stream_state_type& state) noexcept;

    http3_sans_io_session_engine& session_;
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
