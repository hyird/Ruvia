#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_server_request_admission.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/quic_types.h"

#include "http3/http3_critical_stream_driver.h"
#include "http3/http3_server_stream_output.h"

namespace ruvia::detail {

class http3_worker;
class http3_server_connection;

struct http3_connection_driver_config final {
    std::size_t max_requests_per_connection_{};
    std::optional<std::chrono::milliseconds> request_header_timeout_;
    std::optional<std::chrono::milliseconds> request_body_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::chrono::milliseconds drain_timeout_{};
    std::chrono::milliseconds handshake_timeout_{};
    http3_settings local_settings_{};
};

// The single worker-local owner of one HTTP/3 server connection slot: its QUIC
// connection, critical streams, response output and http3_server_connection.
// QUIC stream reads feed the HTTP connection directly; handlers write their
// HEADERS/DATA straight to QUIC through output(). A slot is reused after its
// QUIC connection retired and its HTTP connection was joined.
class http3_connection_driver final {
public:
    using clock_type = std::chrono::steady_clock;

    http3_connection_driver(http3_worker& worker, http3_connection_driver_config config);
    ~http3_connection_driver();
    http3_connection_driver(const http3_connection_driver&) = delete;
    http3_connection_driver& operator=(const http3_connection_driver&) = delete;
    http3_connection_driver(http3_connection_driver&&) = delete;
    http3_connection_driver& operator=(http3_connection_driver&&) = delete;

    // Transport port used by this slot's http3_server_connection. Worker-affine;
    // every operation is synchronous and never re-enters the HTTP connection.
    [[nodiscard]] http3_server_stream_output& output() noexcept {
        return *output_;
    }
    // Resets a response/push stream and stops its request input.
    void reset_stream(std::uint64_t stream_id, http3_connection_error_code code) noexcept;
    // Opens a push stream bound to push_id; nullopt when unavailable or stopping.
    [[nodiscard]] std::optional<std::uint64_t> open_push_stream(std::uint64_t push_id) noexcept;
    // Starts closing the QUIC connection with an HTTP/3 application error.
    void close_connection(http3_connection_error_code code) noexcept;
    // Sends one encoded HTTP Datagram; throws when the connection cannot send.
    void send_datagram(std::span<const std::byte> datagram);
    // Schedules this connection on its worker.
    void wake() noexcept;

    // Worker side.
    [[nodiscard]] bool vacant() const noexcept {
        return !token_ && !joining_;
    }
    [[nodiscard]] std::optional<ruvia::quic_connection_token> token() const noexcept {
        return token_;
    }
    // Admits one Initial into this vacant slot. Only would_block keeps the offer.
    // now is the protocol turn's timestamp: QUIC timestamps must not go back.
    [[nodiscard]] ruvia::quic_operation_status admit(const ruvia::quic_initial_offer& offer, clock_type::time_point now);
    // One bounded turn of connection work: handshake/binding, critical output,
    // stream input, handler publication, datagrams, timeouts and retirement.
    // Returns true when another turn is required without a new event.
    [[nodiscard]] bool pump(std::span<std::byte> scratch, clock_type::time_point now);
    // QUIC accepted a datagram for this connection (ACKs, credit, stream data).
    void transport_activity() noexcept;
    void transport_failure() noexcept;
    void request_stop() noexcept;
    // Earliest local deadline (handshake, drain, request phase or write timeout).
    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept;
    // Joins the retired HTTP connection and frees the slot.
    [[nodiscard]] task<void> join_retired();

    // Worker ready-list and output-credit wait membership.
    bool queued_{};
    bool send_blocked_{};

private:
    struct stream_state final {
        enum class receive_phase : std::uint8_t { headers,
            body };

        std::uint64_t id_{};
        clock_type::time_point last_input_activity_{};
        receive_phase input_phase_{receive_phase::headers};
        bool received_early_data_{};
        bool request_stream_{};
        bool input_terminal_{};
        bool input_fin_{};
        bool input_reset_{};

        [[nodiscard]] bool body_timeout_applies(bool tunnel_established) const noexcept {
            return request_stream_ && input_phase_ == receive_phase::body &&
                   !input_fin_ && !input_reset_ && !input_terminal_ && !tunnel_established;
        }
    };

    struct close_code final {
        ruvia::quic_close_kind kind_{ruvia::quic_close_kind::application};
        std::uint64_t value_{};
    };

    [[nodiscard]] ruvia::quic_connection& quic();
    [[nodiscard]] bool serving() const noexcept {
        return connection_ != nullptr && token_.has_value();
    }
    [[nodiscard]] bool closing() const noexcept {
        return close_started_ || graceful_close_started_;
    }
    [[nodiscard]] bool bind(clock_type::time_point now);
    [[nodiscard]] bool serve(std::span<std::byte> scratch, clock_type::time_point now);
    [[nodiscard]] bool accept_new_streams(clock_type::time_point now);
    [[nodiscard]] bool pump_streams(std::span<std::byte> scratch, clock_type::time_point now);
    [[nodiscard]] bool pump_datagrams(std::span<std::byte> scratch);
    [[nodiscard]] bool retire(clock_type::time_point now);
    [[nodiscard]] stream_state* find_stream(std::uint64_t stream_id) noexcept;
    [[nodiscard]] bool tunnel_established(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] std::optional<clock_type::time_point> input_deadline(const stream_state& stream) const noexcept;
    [[nodiscard]] http3_datagram_receive_status plan_datagram_receive(const http3_datagram_view& datagram) noexcept;
    void latch_request_head(stream_state& stream) noexcept;
    void terminate_request_stream(std::uint64_t stream_id, std::uint64_t error_code) noexcept;
    void stop_input(stream_state& stream) noexcept;
    void deliver_reset(std::uint64_t stream_id, http3_connection_error_code code) noexcept;
    [[nodiscard]] bool announce_goaway() noexcept;
    [[nodiscard]] bool reject_request_stream(std::uint64_t stream_id);
    void refuse_connection() noexcept;
    void start_close(close_code code) noexcept;
    void release_quic() noexcept;
    void release_slot() noexcept;

    http3_worker& worker_;
    const http3_connection_driver_config config_;
    std::optional<ruvia::quic_connection_token> token_;
    std::optional<clock_type::time_point> handshake_deadline_;
    std::optional<clock_type::time_point> drain_deadline_;
    // Connection metadata borrowed by the HTTP connection's context services.
    std::pmr::string remote_address_;
    std::uint16_t remote_port_{};
    std::pmr::vector<stream_state> streams_;
    std::size_t next_input_stream_index_{};
    std::unique_ptr<http3_server_stream_output, pmr_object_deleter<http3_server_stream_output>> output_;
    std::optional<http3_critical_stream_driver> critical_;
    std::optional<http3_server_request_admission_planner> admission_planner_;
    // Declared after output_ and the stream table, which it borrows.
    std::unique_ptr<http3_server_connection, pmr_object_deleter<http3_server_connection>> connection_;
    std::optional<close_code> close_code_;
    std::size_t admitted_request_count_{};
    std::size_t peer_unidirectional_stream_count_{};
    std::size_t rejected_request_count_{};
    bool admission_sealed_{};
    bool goaway_queued_{};
    bool goaway_bytes_accepted_{};
    bool graceful_close_started_{};
    bool graceful_close_abandoned_{};
    // close_started_ denotes the rapid, no-flush forced path only.
    bool close_started_{};
    // A CONNECTION_CLOSE was produced; retirement waits one flush turn.
    bool close_flush_pending_{};
    bool connection_stop_requested_{};
    bool transport_activity_{};
    bool stopping_{};
    bool joining_{};
};

}  // namespace ruvia::detail
