#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/http_capsule_stream.h"
namespace ruvia {
enum class http_datagram_send_policy : unsigned char { automatic,
    capsule,
    quic };
struct http_datagram_config final {
    http_capsule_config capsules_{};
    http_datagram_send_policy send_policy_{http_datagram_send_policy::automatic};
};
// Owned opaque HTTP Datagram payload, valid across later reads and client
// shutdown. Destroy results on their worker before the worker retires.
class http_datagram final {
public:
    http_datagram(const http_datagram&) = delete;
    http_datagram& operator=(const http_datagram&) = delete;
    http_datagram(http_datagram&&) noexcept = default;
    http_datagram& operator=(http_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept {
        auto bytes_value = capsule_.payload();
        bytes_value.remove_prefix(std::min(offset_, bytes_value.size()));
        return std::as_bytes(std::span(bytes_value.data(), bytes_value.size()));
    }
    std::span<const std::byte> payload() const&& = delete;
    [[nodiscard]] http_datagram_transport transport() const noexcept {
        return transport_;
    }

private:
    friend class http_datagram_stream;
    http_datagram(http_capsule capsule, std::size_t offset, http_datagram_transport transport) noexcept
        : capsule_(std::move(capsule)),
          offset_(offset),
          transport_(transport) {}
    http_capsule capsule_;
    std::size_t offset_{};
    http_datagram_transport transport_{};
};
// RFC 9297 channel over a negotiated tunnel. Receives both DATAGRAM capsules
// and native QUIC DATAGRAMs on one lane. Automatic sending chooses QUIC when
// negotiated and within its packet bound, otherwise reliable capsules. QUIC
// sends are best effort; a full bounded queue drops the packet. Empty payloads
// are data and an empty optional is EOF. Use exclusively with the tunnel.
class http_datagram_stream final {
public:
    // An invalid policy throws std::invalid_argument before the stream is moved.
    explicit http_datagram_stream(http_capsule_stream&& stream, http_datagram_send_policy policy = http_datagram_send_policy::automatic);
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] scoped_operation<std::optional<http_datagram>> read() &;
    scoped_operation<std::optional<http_datagram>> read() && = delete;
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
    friend class http_udp_tunnel;
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp);
    [[nodiscard]] scoped_operation<void> send_payload(std::string_view payload, bool udp);
    http_capsule_stream stream_;
};
}  // namespace ruvia
