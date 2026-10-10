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
#include "ruvia/http/http3_server_request_admission.h"
#include "ruvia/http/http3_stream_frames.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_datagram.h"

#include "http3/http3_connection_state.h"
#include "http3/http3_critical_stream_driver.h"
#include "http3/http3_quic_wire_owner.h"
#include "http3/http3_server_stream_output.h"
#include "http3/http3_stream_buffer.h"

namespace ruvia::detail {

struct http3_connection_driver_config final {
    std::size_t max_requests_per_connection_{};
    std::uint32_t buffer_capacity_{};
    std::optional<std::chrono::milliseconds> request_header_timeout_;
    std::optional<std::chrono::milliseconds> request_body_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::chrono::milliseconds drain_timeout_{};
    std::chrono::milliseconds handshake_timeout_{};
    http3_settings local_settings_{};
};

// One generation's network/resource authority. Shared lifecycle facts stay in
// state; handler attachment does not imply prepared protocol output. The driver
// owns every transport borrow until output retirement has returned its leases.
class http3_connection_driver final {
public:
    http3_connection_driver(std::pmr::memory_resource* resource,
        http3_connection_state& state_value, http3_stream_buffer& request_buffer,
        http3_quic_wire_owner& wire, http3_connection_driver_config config);
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
    [[nodiscard]] http3_connection_state::intent_execution_result execute_intent(const http3_server_connection::transport_intent_type& intent) noexcept;

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
            : frame_tracker_(nullptr, pmr_object_deleter<http3_stream_frames>{resource}) {}

        enum class receive_phase : std::uint8_t { headers,
            body };

        std::uint64_t id_{};
        std::uint64_t received_bytes_{};
        std::optional<http3_stream_control> pending_control_;
        std::unique_ptr<http3_stream_frames, pmr_object_deleter<http3_stream_frames>> frame_tracker_;
        std::chrono::steady_clock::time_point last_input_activity_{};
        std::optional<std::uint64_t> tunnel_established_barrier_{};
        receive_phase input_phase_{receive_phase::headers};
        bool received_early_data_{};
        bool request_stream_{};
        bool input_terminal_{};
        bool input_fin_{};
        bool input_reset_{};
        bool write_timeout_notified_{};
        bool tunnel_established_{};

        [[nodiscard]] tunnel_established_result accept_tunnel_established(
            const http3_stream_control& control,
            http3_connection_identity identity,
            std::uint64_t accepted_wire_bytes) noexcept {
            if (control.kind_ != http3_stream_control::kind::tunnel_established ||
                control.id_.epoch_ != identity.epoch_ ||
                control.id_.connection_generation_ != identity.connection_generation_ ||
                control.id_.stream_id_ != id_ || !request_stream_ || control.value_ == 0 ||
                control.value_ > http3_var_int_max) {
                return tunnel_established_result::protocol_failure;
            }
            if (input_reset_ || (input_terminal_ && !input_fin_)) {
                return tunnel_established_result::ignored_terminal;
            }
            if (input_phase_ != receive_phase::body || tunnel_established_ ||
                tunnel_established_barrier_) {
                return tunnel_established_result::protocol_failure;
            }
            tunnel_established_barrier_ = control.value_;
            (void)confirm_tunnel_established(accepted_wire_bytes);
            return tunnel_established_result::accepted;
        }

        [[nodiscard]] bool confirm_tunnel_established(
            std::uint64_t accepted_wire_bytes) noexcept {
            if (!tunnel_established_barrier_ ||
                accepted_wire_bytes < *tunnel_established_barrier_) {
                return false;
            }
            tunnel_established_barrier_.reset();
            tunnel_established_ = true;
            return true;
        }

        [[nodiscard]] bool body_timeout_applies() const noexcept {
            return request_stream_ && input_phase_ == receive_phase::body &&
                   !input_fin_ && !input_reset_ && !input_terminal_ && !tunnel_established_;
        }
    };

    struct push_stream final {
        std::uint64_t stream_id_{};
        std::uint64_t push_id_{};
    };
    struct close_code final {
        ruvia::quic_close_kind kind_{ruvia::quic_close_kind::application};
        std::uint64_t value_{};
    };
    [[nodiscard]] bool has_generation() const noexcept {
        return identity_.epoch_ != 0;
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
    [[nodiscard]] http3_datagram_receive_status plan_datagram_receive(const http3_datagram_view& datagram) const noexcept;
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
    void close_connection(http3_connection_error_code reason) noexcept;
    // Refuses an admitted connection before any HTTP/3 state exists.
    void refuse_connection() noexcept;
    void start_close(close_code code) noexcept;

    std::pmr::memory_resource* resource_;
    http3_connection_state* state_;
    http3_stream_buffer* request_buffer_;
    http3_quic_wire_owner* wire_;
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
    std::unique_ptr<http3_server_stream_output,
        pmr_object_deleter<http3_server_stream_output>>
        output_;
    std::unique_ptr<http3_critical_stream_driver,
        pmr_object_deleter<http3_critical_stream_driver>>
        critical_;
    std::optional<http3_server_request_admission_planner> admission_planner_;
    std::optional<std::chrono::steady_clock::time_point> drain_deadline_;
    std::size_t admitted_request_count_{};
    std::size_t peer_unidirectional_stream_count_{};
    std::size_t rejected_request_count_{};
    bool goaway_queued_{};
    bool goaway_bytes_accepted_{};
    bool graceful_close_started_{};
    bool graceful_close_abandoned_{};
    std::optional<close_code> close_code_;
    // close_started_ denotes the rapid, no-flush forced path only.
    bool close_started_{};

    bool stopping_{};
    bool executor_installed_{};
};

}  // namespace ruvia::detail
