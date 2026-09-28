#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::detail {

// A single-thread-affine server network UDP socket. It owns one fixed receive slot and
// one fixed send slot; it does not own a thread, QUIC transport, or packet queue.
class Http3UdpSocket final {
public:
    static constexpr std::size_t kDatagramBufferSize = 65536;

    struct ReceiveView final {
        std::span<const std::byte> bytes;
        asio::ip::udp::endpoint peer;
        asio::ip::udp::endpoint localDestination;
    };

    using ReceiveCompletion = void (*)(void*, std::error_code, ReceiveView) noexcept;

    struct SendView final {
        asio::ip::udp::endpoint source;
        asio::ip::udp::endpoint peer;
        std::span<const std::byte> bytes;
    };

    using SendCompletion = void (*)(void*, std::error_code, std::size_t) noexcept;

    Http3UdpSocket(asio::io_context& networkIo,
        asio::ip::udp::endpoint bindEndpoint);
    ~Http3UdpSocket();

    Http3UdpSocket(const Http3UdpSocket&) = delete;
    Http3UdpSocket& operator=(const Http3UdpSocket&) = delete;
    Http3UdpSocket(Http3UdpSocket&&) = delete;
    Http3UdpSocket& operator=(Http3UdpSocket&&) = delete;

    // Synchronously opens, configures pktinfo, and binds on the server network owner.
    // Throws std::system_error (or std::logic_error) if preparation fails.
    void prepare();
    [[nodiscard]] std::uint16_t boundPort() const noexcept;

    // false means no operation was accepted and no callback will run. Received
    // bytes remain borrowed through the next receive start or requestStop(); a
    // callback may rearm, which immediately starts that next receive lifetime.
    [[nodiscard]] bool asyncReceive(void* context,
        ReceiveCompletion completion) noexcept;

    // Endpoints are copied by value. The caller-owned payload stays borrowed
    // until completion; source must be concrete and use this socket's bound port.
    [[nodiscard]] bool asyncSend(SendView view, void* context,
        SendCompletion completion) noexcept;

    // Owner-thread only and nonblocking. Pending completions are drained by the
    // borrowed io_context; callbacks must not destroy this owner. Keep it alive
    // until done() becomes true.
    void requestStop() noexcept;
    [[nodiscard]] bool done() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl, PmrObjectDeleter<Impl>> impl_;
};

}  // namespace ruvia::detail
