#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {

struct http3_server_stream_output_config final {
    // Bounds response/push stream records, including retained terminal records.
    std::size_t max_tracked_streams_{32};
    // Per-response-stream write inactivity timeout; nullopt disables it.
    std::optional<std::chrono::milliseconds> write_timeout_{};
};

// Worker-affine QUIC response egress for one HTTP/3 server connection. Writers
// publish HEADERS/DATA straight from their own buffers: QUIC copies the accepted
// prefix before write() returns, and its stream flow control is the only
// backpressure. A writer that sees blocked or closed parks until its connection
// owner reactivates it after transport activity or cancels the stream.
// The connection owner detaches this output before retiring the QUIC connection;
// every later operation reports closed.
class http3_server_stream_output final {
public:
    using stream_id_type = std::uint64_t;

    enum class status_type : std::uint8_t {
        accepted,
        // Nothing was accepted; retry after transport activity.
        blocked,
        // The stream or connection is terminal; the owner cancels the writer.
        closed,
    };

    struct write_result_type final {
        status_type status_{status_type::closed};
        std::size_t accepted_{};
    };

    enum class stream_state_type : std::uint8_t {
        open,
        finished,
        cancelled,
        failed,
    };

    struct stream_info_type final {
        stream_state_type state_{stream_state_type::open};
        std::uint64_t accepted_wire_bytes_{};
        std::optional<std::uint64_t> push_id_{};
        bool send_fin_accepted_{};
        bool tunnel_established_{};
    };

    http3_server_stream_output(ruvia::quic_connection& connection,
        std::pmr::memory_resource* resource, http3_server_stream_output_config config);
    ~http3_server_stream_output() = default;
    http3_server_stream_output(const http3_server_stream_output&) = delete;
    http3_server_stream_output& operator=(const http3_server_stream_output&) = delete;
    http3_server_stream_output(http3_server_stream_output&&) = delete;
    http3_server_stream_output& operator=(http3_server_stream_output&&) = delete;

    // Writer side. Only client request streams and push streams opened here are
    // writable; accepted_ may be shorter than bytes.
    [[nodiscard]] write_result_type write(stream_id_type stream_id, std::span<const std::byte> bytes);
    // Local FIN once every response byte was accepted.
    [[nodiscard]] status_type finish(stream_id_type stream_id);
    // A CONNECT stream's 2xx head has been accepted: HTTP Datagrams and tunnel
    // input are now permitted on it.
    [[nodiscard]] status_type establish_tunnel(stream_id_type stream_id);
    [[nodiscard]] bool closed() const noexcept {
        return connection_ == nullptr;
    }

    // Owner side.
    [[nodiscard]] std::optional<stream_id_type> open_push_stream(std::uint64_t push_id);
    // Terminates both directions of a request stream (or resets a push stream).
    // After a local FIN only the receive side is stopped.
    void cancel_stream(stream_id_type stream_id, std::uint64_t error_code);
    // Cancels streams whose pending output made no progress within the write
    // timeout and streams whose QUIC send side was closed by the peer. Returns
    // each such request stream once so the owner can retire its handler.
    template <typename function_type>
    void take_terminated(std::chrono::steady_clock::time_point now, function_type&& terminated);
    [[nodiscard]] std::optional<stream_info_type> stream_info(stream_id_type stream_id) const noexcept;
    // Response writes begun but not yet locally FINished or terminated.
    [[nodiscard]] std::size_t live_stream_count() const noexcept {
        return live_streams_;
    }
    // An unrecoverable QUIC result; the owner must close the connection.
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    // Earliest write-timeout expiry among streams with pending output.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_write_deadline() const noexcept {
        std::optional<std::chrono::steady_clock::time_point> earliest;
        if (!write_timeout_ || pending_streams_ == 0) {
            return earliest;
        }
        for (const auto& slot : streams_) {
            if (slot.occupied_ && slot.output_pending_ &&
                (!earliest || slot.last_progress_ + *write_timeout_ < *earliest)) {
                earliest = slot.last_progress_ + *write_timeout_;
            }
        }
        return earliest;
    }
    // Terminates every live stream and detaches from QUIC before its retirement.
    void detach() noexcept;

private:
    struct stream_slot_type final {
        stream_id_type stream_id_{};
        stream_info_type info_{};
        // Pending output is the writer's own unaccepted bytes or FIN.
        std::chrono::steady_clock::time_point last_progress_{};
        bool output_pending_{};
        bool started_{};
        bool terminated_unreported_{};
        bool occupied_{};
    };

    [[nodiscard]] stream_slot_type* find(stream_id_type stream_id) noexcept;
    [[nodiscard]] const stream_slot_type* find(stream_id_type stream_id) const noexcept;
    [[nodiscard]] stream_slot_type* find_or_create(stream_id_type stream_id);
    [[nodiscard]] stream_slot_type* writable_slot(stream_id_type stream_id);
    void start(stream_slot_type& slot) noexcept;
    void terminate(stream_slot_type& slot, stream_state_type state, std::uint64_t error_code) noexcept;
    void blocked(stream_slot_type& slot, std::chrono::steady_clock::time_point now) noexcept;
    void progressed(stream_slot_type& slot, std::chrono::steady_clock::time_point now) noexcept;
    void end(stream_slot_type& slot, stream_state_type state) noexcept;
    [[nodiscard]] status_type fail() noexcept;
    [[nodiscard]] static std::size_t table_capacity(std::size_t max_tracked_streams);

    ruvia::quic_connection* connection_;
    const std::size_t max_tracked_streams_;
    const std::optional<std::chrono::milliseconds> write_timeout_;
    std::pmr::vector<stream_slot_type> streams_;
    std::size_t tracked_streams_{};
    std::size_t live_streams_{};
    std::size_t pending_streams_{};
    std::size_t unreported_streams_{};
    bool failed_{};
};

template <typename function_type>
void http3_server_stream_output::take_terminated(
    std::chrono::steady_clock::time_point now, function_type&& terminated) {
    if (pending_streams_ == 0 && unreported_streams_ == 0) {
        return;
    }
    for (auto& slot : streams_) {
        if (!slot.occupied_) {
            continue;
        }
        if (slot.output_pending_ && write_timeout_ && connection_ != nullptr &&
            now >= slot.last_progress_ && now - slot.last_progress_ >= *write_timeout_) {
            terminate(slot, stream_state_type::cancelled,
                static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
        }
        if (slot.terminated_unreported_) {
            slot.terminated_unreported_ = false;
            --unreported_streams_;
            if (!slot.info_.push_id_) {
                terminated(slot.stream_id_);
            }
        }
    }
}

}  // namespace ruvia::detail
