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

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/Http3ServerRequestAdmission.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"
#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"
#include "ruvia/web/detail/http3/http3_connection_state.h"
#include "ruvia/web/detail/http3/http3_stream_buffer.h"

namespace ruvia::detail {

struct http3_connection_driver_config final {
    std::size_t max_requests_per_connection{};
    std::uint32_t buffer_capacity{};
    std::optional<std::chrono::milliseconds> request_header_timeout;
    std::optional<std::chrono::milliseconds> request_body_timeout;
    std::optional<std::chrono::milliseconds> write_timeout;
    std::chrono::milliseconds drain_timeout{};
    std::chrono::milliseconds handshake_timeout{};
    Http3Settings local_settings{};
};

// One generation's network/resource authority. Shared lifecycle facts stay in
// state; handler attachment does not imply prepared protocol output. The driver
// owns every transport borrow until output retirement has returned its leases.
class http3_connection_driver final {
public:
    http3_connection_driver(std::pmr::memory_resource* resource,
        http3_connection_state& state, http3_stream_buffer& request_buffer,
        Http3QuicWireOwner& wire, http3_connection_driver_config config);
    http3_connection_driver(const http3_connection_driver&) = delete;
    http3_connection_driver& operator=(const http3_connection_driver&) = delete;
    // Only cold, uninstalled storage may move. install_executor pins this address
    // until destruction; generations reset their resources in place.
    http3_connection_driver(http3_connection_driver&& other) noexcept;
    http3_connection_driver& operator=(http3_connection_driver&&) = delete;

    void install_executor() noexcept;
    [[nodiscard]] bool matches(http3_connection_identity identity) const noexcept;
    [[nodiscard]] std::optional<ruvia::quic_connection_token> transport_token() const noexcept {
        return transport_id_;
    }
    [[nodiscard]] bool pump_admission(std::pmr::vector<ruvia::quic_initial_offer>& offers);
    [[nodiscard]] bool pump_local(bool transport_activity);
    [[nodiscard]] bool retire(bool response_drained);
    void request_stop() noexcept;
    void transport_failure() noexcept;
    void observe_transport(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] ruvia::quic_packet_result write_packet(std::span<std::byte> bytes, std::chrono::steady_clock::time_point now);
    [[nodiscard]] bool accept_response_control(const http3_stream_control& control) noexcept;
    [[nodiscard]] bool accept_response_data(http3_stream_buffer::borrowed_block& block) noexcept;
    [[nodiscard]] http3_connection_state::intent_execution_result execute_intent(const Http3ServerConnection::TransportIntent& intent) noexcept;

private:
    friend struct http3_connection_driver_test_access;
    explicit http3_connection_driver(std::pmr::memory_resource* resource);
    enum class tunnel_established_result : std::uint8_t {
        accepted,
        ignored_terminal,
        protocol_failure,
    };

    struct stream_state final {
        explicit stream_state(std::pmr::memory_resource* resource)
            : frame_tracker(nullptr, PmrObjectDeleter<Http3StreamFrames>{resource}) {}

        enum class receive_phase : std::uint8_t { headers,
            body };

        std::uint64_t id{};
        std::uint64_t received_bytes{};
        std::optional<http3_stream_control> pending_control;
        std::unique_ptr<Http3StreamFrames, PmrObjectDeleter<Http3StreamFrames>> frame_tracker;
        std::chrono::steady_clock::time_point last_input_activity{};
        std::optional<std::uint64_t> tunnel_established_barrier{};
        receive_phase input_phase{receive_phase::headers};
        bool received_early_data{};
        bool request_stream{};
        bool input_terminal{};
        bool input_fin{};
        bool input_reset{};
        bool write_timeout_notified{};
        bool tunnel_established{};

        [[nodiscard]] tunnel_established_result accept_tunnel_established(
            const http3_stream_control& control,
            http3_connection_identity identity,
            std::uint64_t accepted_wire_bytes) noexcept {
            if (control.kind != http3_stream_control::kind::tunnel_established ||
                control.id.epoch != identity.epoch ||
                control.id.connection_generation != identity.connection_generation ||
                control.id.stream_id != id || !request_stream || control.value == 0 ||
                control.value > kHttp3VarIntMax) {
                return tunnel_established_result::protocol_failure;
            }
            if (input_reset || (input_terminal && !input_fin)) {
                return tunnel_established_result::ignored_terminal;
            }
            if (input_phase != receive_phase::body || tunnel_established ||
                tunnel_established_barrier) {
                return tunnel_established_result::protocol_failure;
            }
            tunnel_established_barrier = control.value;
            (void)confirm_tunnel_established(accepted_wire_bytes);
            return tunnel_established_result::accepted;
        }

        [[nodiscard]] bool confirm_tunnel_established(
            std::uint64_t accepted_wire_bytes) noexcept {
            if (!tunnel_established_barrier ||
                accepted_wire_bytes < *tunnel_established_barrier) {
                return false;
            }
            tunnel_established_barrier.reset();
            tunnel_established = true;
            return true;
        }

        [[nodiscard]] bool body_timeout_applies() const noexcept {
            return request_stream && input_phase == receive_phase::body &&
                   !input_fin && !input_reset && !input_terminal && !tunnel_established;
        }
    };

    struct push_stream final {
        std::uint64_t stream_id{};
        std::uint64_t push_id{};
    };
    [[nodiscard]] bool has_generation() const noexcept {
        return identity_.epoch != 0;
    }
    [[nodiscard]] bool bound() const noexcept {
        return state_->binding().has_value();
    }
    [[nodiscard]] bool protocol_ready() const noexcept {
        return output_ != nullptr;
    }
    void release_generation() noexcept;
    [[nodiscard]] bool admit(std::pmr::vector<ruvia::quic_initial_offer>& offers);
    [[nodiscard]] bool retire_unbound() noexcept;
    [[nodiscard]] bool revoke_reservation() noexcept;
    [[nodiscard]] bool prepare_protocol() noexcept;
    [[nodiscard]] bool pump_input();
    [[nodiscard]] bool pump_output(bool transport_activity);
    [[nodiscard]] bool pump_datagrams();
    [[nodiscard]] tunnel_established_result accept_tunnel_established(const http3_stream_control& control, std::uint64_t accepted_wire_bytes) noexcept;
    [[nodiscard]] bool confirm_tunnel_established(std::uint64_t stream_id, std::uint64_t accepted_wire_bytes) noexcept;
    void note_peer_fin(std::uint64_t stream_id) noexcept;
    void note_input_reset(std::uint64_t stream_id) noexcept;
    void complete_input_terminal(std::uint64_t stream_id) noexcept;
    void terminate_request_stream(std::uint64_t stream_id, std::uint64_t error_code);
    void stop_request_input(std::uint64_t stream_id) noexcept;
    [[nodiscard]] bool announce_goaway() noexcept;
    [[nodiscard]] bool seal_admission() noexcept;
    [[nodiscard]] bool reject_request_stream(std::uint64_t stream_id);
    void close_connection(Http3ConnectionErrorCode reason) noexcept;

    std::pmr::memory_resource* resource_;
    http3_connection_state* state_;
    http3_stream_buffer* request_buffer_;
    Http3QuicWireOwner* wire_;
    http3_connection_driver_config config_;
    http3_connection_identity identity_{};
    std::optional<ruvia::quic_connection_token> transport_id_;
    std::optional<std::chrono::steady_clock::time_point> handshake_deadline_{};
    // Bind borrows worker-owned storage. It remains immutable until both
    // same-worker protocol halves finalize this generation.
    std::pmr::string remote_address_;
    std::pmr::vector<stream_state> streams_;
    std::pmr::vector<push_stream> push_streams_;
    std::size_t next_input_stream_index_{};
    std::size_t next_tunnel_handshake_stream_index_{};
    std::size_t tunnel_handshake_scan_remaining_{};
    std::size_t pending_tunnel_handshakes_{};
    bool tunnel_handshake_scan_dirty_{};
    std::unique_ptr<Http3ServerStreamOutput,
        PmrObjectDeleter<Http3ServerStreamOutput>>
        output_;
    std::unique_ptr<Http3CriticalStreamDriver,
        PmrObjectDeleter<Http3CriticalStreamDriver>>
        critical_;
    std::optional<Http3ServerRequestAdmissionPlanner> admission_planner_;
    std::optional<std::chrono::steady_clock::time_point> drain_deadline_;
    std::size_t admitted_request_count_{};
    std::size_t peer_unidirectional_stream_count_{};
    std::size_t rejected_request_count_{};
    bool goaway_queued_{};
    bool goaway_bytes_accepted_{};
    bool graceful_close_started_{};
    bool graceful_close_abandoned_{};
    std::optional<Http3ConnectionErrorCode> close_error_code_;
    // close_started_ denotes the rapid, no-flush forced path only.
    bool close_started_{};

    bool stopping_{};
    bool executor_installed_{};
};

}  // namespace ruvia::detail
