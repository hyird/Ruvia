#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::detail {

// Owner-affine UDP operations. The socket owns metadata and callback storage,
// never packet bytes. Both payload borrows last through the completion callback,
// including cancellation and Windows overlapped retirement.
class http3_udp_socket final {
public:
    static constexpr std::size_t datagram_buffer_size = 65536;

    struct receive_view final {
        std::span<const std::byte> bytes;
        asio::ip::udp::endpoint peer;
        asio::ip::udp::endpoint local_destination;
    };
    using receive_completion = void (*)(void*, std::error_code, receive_view) noexcept;

    struct send_view final {
        asio::ip::udp::endpoint source;
        asio::ip::udp::endpoint peer;
        std::span<const std::byte> bytes;
    };
    using send_completion = void (*)(void*, std::error_code, std::size_t) noexcept;

    http3_udp_socket(asio::io_context& network_io, asio::ip::udp::endpoint bind_endpoint);
    ~http3_udp_socket();
    http3_udp_socket(const http3_udp_socket&) = delete;
    http3_udp_socket& operator=(const http3_udp_socket&) = delete;
    http3_udp_socket(http3_udp_socket&&) = delete;
    http3_udp_socket& operator=(http3_udp_socket&&) = delete;

    void prepare();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    // false accepts no operation and promises no callback. The caller retains
    // writable storage until the accepted operation's callback retires.
    [[nodiscard]] bool async_receive(std::span<std::byte> bytes, void* context,
        receive_completion completion) noexcept;
    [[nodiscard]] bool async_send(send_view view, void* context,
        send_completion completion) noexcept;
    // Keep this owner and every accepted operation's storage alive until done().
    void request_stop() noexcept;
    [[nodiscard]] bool done() const noexcept;

private:
    class impl;
    std::unique_ptr<impl, PmrObjectDeleter<impl>> impl_;
};

}  // namespace ruvia::detail
