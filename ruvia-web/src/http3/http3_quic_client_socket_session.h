#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http3_settings.h"

#include "http3/http3_critical_stream_driver.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_datagram_bridge.h"

namespace ruvia::detail {

// Worker-affine UDP socket and QUIC client adapter; the caller owns all waits.
class http3_quic_client_socket_session final {
public:
    static constexpr std::size_t datagram_buffer_size = 65536;
    static constexpr std::size_t batch_size = 32;

    enum class pump_status_type : unsigned char { active,
        would_block,
        closed,
        fatal };
    enum class wake_reason_type : unsigned char {
        readable,
        writable,
        quic_event,
        application,
        deadline,
        stopped,
        candidate_failure,
        fatal,
    };
    struct pump_result_type final {
        pump_status_type status_{pump_status_type::active};
        std::size_t received_{};
        std::size_t sent_{};
        bool input_backpressured_{};
        bool output_backpressured_{};
        bool critical_streams_ready_{};
        bool critical_output_progress_{};
        std::optional<std::chrono::steady_clock::duration> event_timeout_;
    };

    http3_quic_client_socket_session(asio::io_context& io,
        const asio::ip::udp::endpoint& peer, std::string_view tls_hostname,
        http3_quic_client_tls_context& tls,
        http3_settings settings = {.qpack_max_table_capacity_ = 4096, .qpack_blocked_streams_ = 16},
        ruvia::quic_version initial_version = ruvia::quic_version::v1,
        bool enable_early_data = false,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_quic_client_socket_session();

    http3_quic_client_socket_session(const http3_quic_client_socket_session&) = delete;
    http3_quic_client_socket_session& operator=(const http3_quic_client_socket_session&) = delete;
    http3_quic_client_socket_session(http3_quic_client_socket_session&&) = delete;
    http3_quic_client_socket_session& operator=(http3_quic_client_socket_session&&) = delete;

    [[nodiscard]] pump_result_type pump();
    [[nodiscard]] task<wake_reason_type> wait_for_activity(pump_result_type pump,
        std::optional<std::chrono::steady_clock::time_point> absolute_deadline = {});
    // Closes QUIC with reason (an earlier close reason is retained), sends
    // CONNECTION_CLOSE, and keeps the socket for the RFC 9000 closing period,
    // ended no later than latest. The period is skipped when the peer never
    // sent a datagram, since it has no state to answer from. A stop request
    // ends only the wait: CONNECTION_CLOSE is still offered once without
    // blocking. Always releases the transport and sockets before completing;
    // a send failure only ends this best-effort period early.
    [[nodiscard]] task<void> shutdown(ruvia::quic_close_reason_view reason,
        std::chrono::steady_clock::time_point latest);
    void request_stop() noexcept;
    void notify_work() noexcept;
    [[nodiscard]] bool consume_work_notification() noexcept;
    void remember_resumption_ticket(std::optional<ruvia::http3_settings> settings);
    [[nodiscard]] std::size_t take_rejected_early_streams(
        std::span<std::uint64_t> output) noexcept;
    [[nodiscard]] bool early_data_enabled() const noexcept {
        return early_data_enabled_;
    }
    [[nodiscard]] ruvia::quic_connection& transport() noexcept {
        return transport_.connection();
    }
    [[nodiscard]] const ruvia::quic_connection& transport() const noexcept {
        return transport_.connection();
    }
    [[nodiscard]] const asio::ip::udp::endpoint& local_endpoint() const noexcept {
        return local_endpoint_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& peer_endpoint() const noexcept {
        return peer_;
    }
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(
        const asio::ip::udp::endpoint& local_endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    [[nodiscard]] std::optional<ruvia::quic_path_migration> active_path_migration() const noexcept;
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);
    [[nodiscard]] ruvia::quic_stream_write_result write_critical_stream(
        ruvia::http3_critical_stream_output::stream_kind kind, std::span<const char> bytes);
    void close() noexcept;

private:
    enum class packet_route_type : unsigned char { send,
        skip,
        invalid };

    [[nodiscard]] static asio::ip::udp::socket make_socket(asio::io_context& io,
        const asio::ip::udp::endpoint& peer);
    [[nodiscard]] static asio::ip::udp::endpoint concrete_local_endpoint(
        const asio::ip::udp::socket& socket);
    [[nodiscard]] static asio::ip::udp::socket make_candidate_socket(asio::io_context& io,
        const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& peer);
    void settle_migration() noexcept;
    void fail_candidate_migration() noexcept;
    void reset_rejected_early_streams();
    void require_owner_thread() const;
    [[nodiscard]] bool send_pending(pump_result_type& result_value);
    [[nodiscard]] bool receive_datagrams(pump_result_type& result_value);
    [[nodiscard]] packet_route_type route_packet(const ruvia::quic_packet_result& output);
    [[nodiscard]] bool drive_closing_period();
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> closing_period_end(
        std::chrono::steady_clock::time_point latest) const noexcept;

    std::thread::id owner_thread_;
    asio::ip::udp::endpoint peer_;
    asio::io_context& io_;
    asio::ip::udp::socket socket_;
    std::optional<asio::ip::udp::socket> candidate_socket_{};
    std::optional<asio::ip::udp::endpoint> candidate_local_endpoint_{};
    std::optional<asio::ip::udp::endpoint> failed_migration_local_endpoint_{};
    std::optional<std::uint64_t> migration_id_{};
    std::optional<ruvia::quic_path_migration> last_migration_{};
    bool pending_candidate_{};
    asio::steady_timer event_timer_;
    asio::steady_timer work_timer_;
    asio::ip::udp::endpoint local_endpoint_;
    http3_quic_client_transport transport_;
    http3_settings settings_;
    std::pmr::vector<std::uint64_t> rejected_early_streams_;
    bool early_data_enabled_{};
    std::optional<http3_critical_stream_driver> critical_streams_;
    std::array<std::byte, datagram_buffer_size> receive_buffer_{};
    std::array<std::byte, datagram_buffer_size> packet_buffer_{};
    std::size_t pending_packet_size_{};
    bool stopping_{};
    bool active_wait_{};
    bool work_pending_{};
    bool peer_reached_{};
    bool closed_{};
};

}  // namespace ruvia::detail
