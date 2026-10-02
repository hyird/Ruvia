#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/HttpDatagramStream.h"

namespace ruvia {
// One owned RFC 9298 UDP payload. The underlying capsule pins its memory until
// this result is destroyed, independently of later receives or client shutdown.
class HttpUdpDatagram final {
public:
    HttpUdpDatagram(const HttpUdpDatagram&) = delete;
    HttpUdpDatagram& operator=(const HttpUdpDatagram&) = delete;
    HttpUdpDatagram(HttpUdpDatagram&&) noexcept = default;
    HttpUdpDatagram& operator=(HttpUdpDatagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept {
        return datagram_.payload();
    }
    std::span<const std::byte> payload() const&& = delete;

    [[nodiscard]] HttpDatagramTransport transport() const noexcept {
        return datagram_.transport();
    }

private:
    friend class HttpUdpTunnel;
    explicit HttpUdpDatagram(HttpDatagram datagram) noexcept
        : datagram_(std::move(datagram)) {}
    HttpDatagram datagram_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown Context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
class HttpUdpTunnel final {
public:
    explicit HttpUdpTunnel(HttpDatagramStream stream) noexcept
        : stream_(std::move(stream)) {}
    explicit HttpUdpTunnel(HttpCapsuleStream stream)
        : stream_(std::move(stream)) {}
    HttpUdpTunnel(const HttpUdpTunnel&) = delete;
    HttpUdpTunnel& operator=(const HttpUdpTunnel&) = delete;
    HttpUdpTunnel(HttpUdpTunnel&&) noexcept = default;
    HttpUdpTunnel& operator=(HttpUdpTunnel&&) noexcept = default;
    [[nodiscard]] ScopedOperation<std::optional<HttpUdpDatagram>> read() &;
    ScopedOperation<std::optional<HttpUdpDatagram>> read() && = delete;
    [[nodiscard]] ScopedOperation<void> send(std::span<const std::byte> payload) &;
    ScopedOperation<void> send(std::span<const std::byte>) && = delete;
    [[nodiscard]] ScopedOperation<void> send(std::string_view payload) &;
    ScopedOperation<void> send(std::string_view) && = delete;
    [[nodiscard]] ScopedOperation<void> finish() & {
        return stream_.finish();
    }
    ScopedOperation<void> finish() && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    [[nodiscard]] static Task<std::optional<HttpUdpDatagram>> readOwned(detail::CapsuleStatePin pin);
    HttpDatagramStream stream_;
};
}  // namespace ruvia
