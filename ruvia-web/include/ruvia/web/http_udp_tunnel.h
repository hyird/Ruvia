#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/http_datagram_stream.h"

namespace ruvia {
// One owned RFC 9298 UDP payload. The underlying capsule pins its memory until
// this result is destroyed, independently of later receives or client shutdown.
class http_udp_datagram final {
public:
    http_udp_datagram(const http_udp_datagram&) = delete;
    http_udp_datagram& operator=(const http_udp_datagram&) = delete;
    http_udp_datagram(http_udp_datagram&&) noexcept = default;
    http_udp_datagram& operator=(http_udp_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept {
        return datagram_.payload();
    }
    std::span<const std::byte> payload() const&& = delete;

    [[nodiscard]] http_datagram_transport transport() const noexcept {
        return datagram_.transport();
    }

private:
    friend class http_udp_tunnel;
    explicit http_udp_datagram(http_datagram datagram) noexcept
        : datagram_(std::move(datagram)) {}
    http_datagram datagram_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
class http_udp_tunnel final {
public:
    explicit http_udp_tunnel(http_datagram_stream stream) noexcept
        : stream_(std::move(stream)) {}
    explicit http_udp_tunnel(http_capsule_stream stream)
        : stream_(std::move(stream)) {}
    http_udp_tunnel(const http_udp_tunnel&) = delete;
    http_udp_tunnel& operator=(const http_udp_tunnel&) = delete;
    http_udp_tunnel(http_udp_tunnel&&) noexcept = default;
    http_udp_tunnel& operator=(http_udp_tunnel&&) noexcept = default;
    [[nodiscard]] scoped_operation<std::optional<http_udp_datagram>> read() &;
    scoped_operation<std::optional<http_udp_datagram>> read() && = delete;
    [[nodiscard]] scoped_operation<void> send(std::span<const std::byte> payload) &;
    scoped_operation<void> send(std::span<const std::byte>) && = delete;
    [[nodiscard]] scoped_operation<void> send(std::string_view payload) &;
    scoped_operation<void> send(std::string_view) && = delete;
    [[nodiscard]] scoped_operation<void> finish() & {
        return stream_.finish();
    }
    scoped_operation<void> finish() && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    [[nodiscard]] static task<std::optional<http_udp_datagram>> read_owned(detail::capsule_state_pin pin);
    http_datagram_stream stream_;
};
}  // namespace ruvia
