#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

// Owner-affine UDP operations. The socket owns metadata and callback storage,
// never packet bytes. Every accepted operation's payload borrow lasts through
// its completion callback, including cancellation and Windows overlapped
// retirement. Each direction admits a bounded number of concurrent operations
// whose metadata is reserved at construction; completions arrive in any order.
// POSIX drains readiness in batches (recvmmsg/sendmmsg on Linux) and posts at
// most one deferred send flush per batch; Windows keeps one overlapped
// operation per admitted slot.
class http3_udp_socket final {
public:
    struct operation_limits final {
        std::size_t receive_{};
        std::size_t send_{};
    };

    struct receive_view final {
        std::span<const std::byte> bytes_;
        asio::ip::udp::endpoint peer_;
        asio::ip::udp::endpoint local_destination_;
    };
    using receive_completion = void (*)(void*, std::error_code, receive_view) noexcept;

    struct send_view final {
        asio::ip::udp::endpoint source_;
        asio::ip::udp::endpoint peer_;
        std::span<const std::byte> bytes_;
    };
    using send_completion = void (*)(void*, std::error_code, std::size_t) noexcept;

    // Both limits must be positive and representable as 32-bit slot indices.
    http3_udp_socket(asio::io_context& network_io, asio::ip::udp::endpoint bind_endpoint,
        operation_limits limits);
    ~http3_udp_socket();
    http3_udp_socket(const http3_udp_socket&) = delete;
    http3_udp_socket& operator=(const http3_udp_socket&) = delete;
    http3_udp_socket(http3_udp_socket&&) = delete;
    http3_udp_socket& operator=(http3_udp_socket&&) = delete;

    void prepare();
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    // false accepts no operation and promises no callback; that includes every
    // slot of the direction being busy. The caller retains writable storage
    // until the accepted operation's callback retires. Callbacks never run
    // inside these calls.
    [[nodiscard]] bool async_receive(std::span<std::byte> bytes, void* context,
        receive_completion completion) noexcept;
    [[nodiscard]] bool async_send(send_view view, void* context,
        send_completion completion) noexcept;
    // Keep this owner and every accepted operation's storage alive until done().
    void request_stop() noexcept;
    [[nodiscard]] bool done() const noexcept;

private:
    class impl;
    std::unique_ptr<impl, pmr_object_deleter<impl>> impl_;
};

}  // namespace ruvia::detail
