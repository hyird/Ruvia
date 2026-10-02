#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/HttpCapsuleStream.h"
namespace ruvia {
enum class HttpDatagramSendPolicy : unsigned char { kAutomatic,
    kCapsule,
    kQuic };
struct HttpDatagramConfig final {
    HttpCapsuleConfig capsules{};
    HttpDatagramSendPolicy sendPolicy{HttpDatagramSendPolicy::kAutomatic};
};
// Owned opaque HTTP Datagram payload, valid across later reads and client
// shutdown. Destroy results on their worker before the worker retires.
class HttpDatagram final {
public:
    HttpDatagram(const HttpDatagram&) = delete;
    HttpDatagram& operator=(const HttpDatagram&) = delete;
    HttpDatagram(HttpDatagram&&) noexcept = default;
    HttpDatagram& operator=(HttpDatagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept {
        auto bytes = capsule_.payload();
        bytes.remove_prefix(std::min(offset_, bytes.size()));
        return std::as_bytes(std::span(bytes.data(), bytes.size()));
    }
    std::span<const std::byte> payload() const&& = delete;
    [[nodiscard]] HttpDatagramTransport transport() const noexcept {
        return transport_;
    }

private:
    friend class HttpDatagramStream;
    HttpDatagram(HttpCapsule capsule, std::size_t offset, HttpDatagramTransport transport) noexcept
        : capsule_(std::move(capsule)),
          offset_(offset),
          transport_(transport) {}
    HttpCapsule capsule_;
    std::size_t offset_{};
    HttpDatagramTransport transport_{};
};
// RFC 9297 channel over a negotiated tunnel. Receives both DATAGRAM capsules
// and native QUIC DATAGRAMs on one lane. Automatic sending chooses QUIC when
// negotiated and within its packet bound, otherwise reliable capsules. QUIC
// sends are best effort; a full bounded queue drops the packet. Empty payloads
// are data and an empty optional is EOF. Use exclusively with the tunnel.
class HttpDatagramStream final {
public:
    explicit HttpDatagramStream(HttpCapsuleStream stream, HttpDatagramSendPolicy policy = HttpDatagramSendPolicy::kAutomatic);
    HttpDatagramStream(const HttpDatagramStream&) = delete;
    HttpDatagramStream& operator=(const HttpDatagramStream&) = delete;
    HttpDatagramStream(HttpDatagramStream&&) noexcept = default;
    HttpDatagramStream& operator=(HttpDatagramStream&&) noexcept = default;
    [[nodiscard]] ScopedOperation<std::optional<HttpDatagram>> read() &;
    ScopedOperation<std::optional<HttpDatagram>> read() && = delete;
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
    friend class HttpUdpTunnel;
    static Task<std::optional<HttpDatagram>> readOwned(detail::CapsuleStatePin pin, bool udp);
    [[nodiscard]] ScopedOperation<void> sendPayload(std::string_view payload, bool udp);
    HttpCapsuleStream stream_;
};
}  // namespace ruvia
