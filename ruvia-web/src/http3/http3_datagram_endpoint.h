#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/buffer_pool.h"

#include "http3/http3_datagram_channel.h"
#include "http3/http3_udp_socket.h"

namespace ruvia::detail {

class http3_datagram_endpoint_types {
public:
    using udp = asio::ip::udp;
    using datagram = http3_datagram_channel::datagram;
    enum class notification_kind : std::uint8_t { input_available,
        output_drained,
        stopping,
        endpoint_retired };
    struct notification final {
        void* context_{};
        void (*notify_)(void*, notification_kind) noexcept {};
    };
    enum class pump_result : std::uint8_t { idle,
        pending,
        stopped,
        error };
    enum class stop_status : std::uint8_t { pending,
        done,
        error };
    struct received_datagram final {
        std::span<const std::byte> bytes_;
        udp::endpoint peer_;
        udp::endpoint local_destination_;
    };
};

// Owner-affine native socket; borrowed io_context, pool and callback context must
// outlive native callback retirement. Destruction requires finalized callbacks.
// send_owned_datagram moves only on admission: rejection or backpressure leaves
// the caller's lease intact. An admitted lease survives stop until its callback.
class http3_acceptor_datagram_endpoint final {
public:
    using udp = http3_datagram_endpoint_types::udp;
    using datagram = http3_datagram_endpoint_types::datagram;
    using notification_kind = http3_datagram_endpoint_types::notification_kind;
    using notification = http3_datagram_endpoint_types::notification;
    using pump_result = http3_datagram_endpoint_types::pump_result;
    using stop_status = http3_datagram_endpoint_types::stop_status;

    http3_acceptor_datagram_endpoint(asio::io_context&, udp::endpoint, notification, buffer_pool&);
    ~http3_acceptor_datagram_endpoint();
    http3_acceptor_datagram_endpoint(const http3_acceptor_datagram_endpoint&) = delete;
    http3_acceptor_datagram_endpoint& operator=(const http3_acceptor_datagram_endpoint&) = delete;
    void prepare();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] pump_result start() noexcept;
    [[nodiscard]] std::optional<datagram> take_receive() noexcept;
    void poll_receive() noexcept;
    [[nodiscard]] pump_result send_owned_datagram(datagram&&) noexcept;
    [[nodiscard]] bool outbound_capacity() const noexcept;
    [[nodiscard]] bool outbound_pending() const noexcept;
    [[nodiscard]] std::size_t outbound_count() const noexcept;
    [[nodiscard]] bool outbound_quiescent() const noexcept;
    void request_stop() noexcept;
    [[nodiscard]] bool endpoint_retired() const noexcept;
    [[nodiscard]] stop_status status() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    static void receive_completion(void*, std::error_code, http3_udp_socket::receive_view) noexcept;
    static void send_completion(void*, std::error_code, std::size_t) noexcept;
    void require_owner_thread() const noexcept;
    [[nodiscard]] bool valid_packet(std::span<const std::byte>, const udp::endpoint&, const udp::endpoint&) noexcept;
    [[nodiscard]] bool arm_receive() noexcept;
    void handle_receive(std::error_code, http3_udp_socket::receive_view) noexcept;
    void handle_send(std::error_code, std::size_t) noexcept;
    void notify(notification_kind) noexcept;
    void fail(std::error_code) noexcept;
    udp::endpoint bind_endpoint_;
    udp::endpoint bound_endpoint_;
    std::thread::id owner_thread_;
    buffer_pool& pool_;
    http3_udp_socket socket_;
    notification notification_;
    std::optional<buffer_lease> receive_lease_;
    std::optional<datagram> held_receive_;
    std::optional<datagram> pending_send_;
    std::error_code error_;
    std::size_t callback_depth_{};
    bool prepared_{};
    bool started_{};
    bool receive_armed_{};
    bool stopping_{};
    bool stopping_notified_{};
};

// Owner-affine channel borrow, never a socket or pool owner. The channel outlives
// retire_channel(); callback context outlives endpoint destruction. Input bytes
// expire at consume/stop; a packet reservation expires at commit/cancel/stop.
// endpoint_retired means channel I/O loans retired, not peer QUIC acknowledgement.
class http3_worker_datagram_endpoint final {
public:
    using udp = http3_datagram_endpoint_types::udp;
    using datagram = http3_datagram_endpoint_types::datagram;
    using notification_kind = http3_datagram_endpoint_types::notification_kind;
    using notification = http3_datagram_endpoint_types::notification;
    using pump_result = http3_datagram_endpoint_types::pump_result;
    using stop_status = http3_datagram_endpoint_types::stop_status;
    using received_datagram = http3_datagram_endpoint_types::received_datagram;

    http3_worker_datagram_endpoint(http3_datagram_channel&, udp::endpoint, notification);
    ~http3_worker_datagram_endpoint();
    http3_worker_datagram_endpoint(const http3_worker_datagram_endpoint&) = delete;
    http3_worker_datagram_endpoint& operator=(const http3_worker_datagram_endpoint&) = delete;
    void prepare();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] pump_result start() noexcept;
    [[nodiscard]] std::optional<received_datagram> receive_slot() const noexcept;
    [[nodiscard]] pump_result consume_receive() noexcept;
    [[nodiscard]] std::span<std::byte> packet_buffer() noexcept;
    void cancel_packet() noexcept;
    [[nodiscard]] pump_result send_datagram(std::span<const std::byte>, const udp::endpoint&, const udp::endpoint&) noexcept;
    [[nodiscard]] bool outbound_capacity() const noexcept;
    [[nodiscard]] bool outbound_pending() const noexcept;
    [[nodiscard]] std::size_t outbound_count() const noexcept;
    [[nodiscard]] bool outbound_quiescent() const noexcept;
    void poll_channel() noexcept;
    void request_stop() noexcept;
    // Release the channel dependency only after all owner-affine I/O is retired,
    // before the worker's final ACK lets Acceptor destroy channel storage.
    void retire_channel() noexcept;
    [[nodiscard]] bool endpoint_retired() const noexcept;
    [[nodiscard]] stop_status status() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    void require_owner_thread() const noexcept;
    [[nodiscard]] bool valid_packet(std::span<const std::byte>, const udp::endpoint&, const udp::endpoint&) noexcept;
    void notify(notification_kind) noexcept;
    void fail(std::error_code) noexcept;
    udp::endpoint bind_endpoint_;
    udp::endpoint bound_endpoint_;
    std::thread::id owner_thread_;
    http3_datagram_channel* channel_;
    notification notification_;
    std::error_code error_;
    std::size_t callback_depth_{};
    std::size_t observed_output_count_{};
    bool prepared_{};
    bool started_{};
    bool stopping_{};
    bool stopping_notified_{};
};

}  // namespace ruvia::detail
