#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3UdpSocket.h"

namespace ruvia::detail {

class http3_datagram_channel;

// Owner-thread datagram boundary. The standalone backend owns UDP; production
// borrows an acceptor-owned channel. Both feed the same authoritative QUIC pump.
class Http3DatagramEndpoint final {
public:
    using udp = asio::ip::udp;

    enum class notification_kind : std::uint8_t { input_available,
        output_drained,
        stopping,
        io_retired };
    struct notification final {
        void* context{};
        void (*notify)(void*, notification_kind) noexcept {};
    };

    enum class pump_result : std::uint8_t { idle,
        pending,
        stopped,
        error };
    enum class stop_status : std::uint8_t { pending,
        done,
        error };

    struct received_datagram final {
        std::span<const std::byte> bytes;
        udp::endpoint peer;
        udp::endpoint local_destination;
    };

    Http3DatagramEndpoint(asio::io_context& network_io, udp::endpoint bind_endpoint,
        notification notification);
    Http3DatagramEndpoint(http3_datagram_channel& channel, udp::endpoint local_endpoint,
        notification notification);
    ~Http3DatagramEndpoint();

    Http3DatagramEndpoint(const Http3DatagramEndpoint&) = delete;
    Http3DatagramEndpoint& operator=(const Http3DatagramEndpoint&) = delete;
    Http3DatagramEndpoint(Http3DatagramEndpoint&&) = delete;
    Http3DatagramEndpoint& operator=(Http3DatagramEndpoint&&) = delete;

    void prepare();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] pump_result start() noexcept;
    [[nodiscard]] std::optional<received_datagram> receive_slot() const noexcept;
    [[nodiscard]] pump_result consume_receive() noexcept;
    [[nodiscard]] pump_result send_datagram(std::span<const std::byte> bytes,
        const udp::endpoint& source, const udp::endpoint& peer) noexcept;
    // The direct backend borrows bytes until output_drained/stopping and socket
    // callback retirement. Used by the acceptor while holding a channel loan.
    [[nodiscard]] pump_result send_borrowed_datagram(std::span<const std::byte> bytes,
        const udp::endpoint& source, const udp::endpoint& peer) noexcept;
    [[nodiscard]] std::span<std::byte> packet_buffer(std::span<std::byte> fallback) noexcept;
    [[nodiscard]] bool send_in_flight() const noexcept;
    [[nodiscard]] bool outbound_quiescent() const noexcept;

    // Owner-thread dispatch after a native channel wake.
    void poll_forwarded() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] bool socket_done() const noexcept;
    [[nodiscard]] stop_status status() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    static udp::endpoint checked_bind_endpoint(udp::endpoint endpoint);
    static notification checked_notification(notification notification);
    static void receive_completion(void* context, std::error_code error,
        Http3UdpSocket::ReceiveView view) noexcept;
    static void send_completion(void* context, std::error_code error,
        std::size_t size) noexcept;

    void require_owner_thread() const noexcept;
    [[nodiscard]] pump_result send_packet(std::span<const std::byte> bytes,
        const udp::endpoint& source, const udp::endpoint& peer, bool copy) noexcept;
    [[nodiscard]] bool arm_receive() noexcept;
    void handle_receive(std::error_code error, Http3UdpSocket::ReceiveView view) noexcept;
    void handle_send(std::error_code error, std::size_t size) noexcept;
    void notify(notification_kind kind) noexcept;
    void fail(std::error_code error) noexcept;

    udp::endpoint bind_endpoint_;
    udp::endpoint bound_endpoint_;
    std::thread::id owner_thread_;
    std::optional<Http3UdpSocket> socket_;
    http3_datagram_channel* channel_{};
    notification notification_;
    Http3UdpSocket::ReceiveView held_receive_;
    std::pmr::vector<std::byte> send_buffer_;
    std::error_code error_;
    std::size_t callback_depth_{};
    std::size_t send_size_{};
    bool prepared_{};
    bool forwarded_{};
    bool started_{};
    bool receive_armed_{};
    bool has_held_receive_{};
    bool send_in_flight_{};
    bool stopping_{};
    bool stopping_notified_{};
};

}  // namespace ruvia::detail
