#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <openssl/bio.h>

namespace ruvia::detail {

// An IPv4/IPv6 UDP address. Port is host byte order; IPv4 uses the first 4 address bytes.
struct Http3QuicDatagramAddress final {
    enum class Family : std::uint8_t { kIPv4,
        kIPv6 };
    Family family{Family::kIPv4};
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port{};
    std::uint32_t scopeId{};
};

// Converts a concrete address into an OpenSSL BIO_ADDR, including host-order
// UDP port normalization. The caller owns target; wildcard, scoped and zero-port
// addresses are rejected.
[[nodiscard]] bool makeHttp3QuicBioAddress(const Http3QuicDatagramAddress& source,
    BIO_ADDR* target) noexcept;

struct Http3QuicOutboundDatagram final {
    std::span<const std::byte> bytes;
    Http3QuicDatagramAddress destination;
    // Present when the BIO specified the source address for this datagram.
    bool hasSource{};
    Http3QuicDatagramAddress source{};
};

// Single-threaded adapter between UDP datagrams and an OpenSSL QUIC datagram BIO.
// The server listener or client SSL owning the returned BIO must be destroyed
// before this bridge.
class Http3QuicDatagramBridge final {
public:
    enum class InjectResult : std::uint8_t { kAccepted,
        kFull,
        kFatal };
    enum class OutboundResult : std::uint8_t { kReady,
        kEmpty,
        kBusy,
        kFatal };

    explicit Http3QuicDatagramBridge(Http3QuicDatagramAddress boundAddress);
    ~Http3QuicDatagramBridge();
    Http3QuicDatagramBridge(const Http3QuicDatagramBridge&) = delete;
    Http3QuicDatagramBridge& operator=(const Http3QuicDatagramBridge&) = delete;
    Http3QuicDatagramBridge(Http3QuicDatagramBridge&&) = delete;
    Http3QuicDatagramBridge& operator=(Http3QuicDatagramBridge&&) = delete;

    // Transfers the SSL-side BIO to SSL_set_bio(ssl, bio, bio), once.
    [[nodiscard]] BIO* releaseSslBio() noexcept;

    // peer identifies the actual sender. For a concrete bind, the bound address is
    // used as the packet's local destination; a wildcard bind requires the explicit
    // destination overload because pktinfo is not yet wired through.
    [[nodiscard]] InjectResult inject(std::span<const std::byte> datagram,
        const Http3QuicDatagramAddress& peer) noexcept;
    // localDestination is consumed synchronously and copied into preallocated BIO
    // address storage; it must match the bind family and port (and a concrete bind's
    // address).
    [[nodiscard]] InjectResult inject(std::span<const std::byte> datagram,
        const Http3QuicDatagramAddress& peer,
        Http3QuicDatagramAddress localDestination) noexcept;

    // At most one packet may be outstanding. Call completeOutbound only after the
    // caller's asynchronous send operation has finished using bytes/destination/source.
    [[nodiscard]] OutboundResult takeOutbound(Http3QuicOutboundDatagram& outbound) noexcept;
    void completeOutbound() noexcept;
    [[nodiscard]] bool outboundQuiescent() const noexcept;
    // Owner-thread metadata for the datagram most recently accepted by inject().
    // A QUIC listener uses the generation to associate a newly queued connection
    // with the packet source without borrowing caller storage.
    [[nodiscard]] std::uint64_t injectionGeneration() const noexcept {
        return injectionGeneration_;
    }
    [[nodiscard]] const Http3QuicDatagramAddress& lastInjectedPeer() const noexcept {
        return lastInjectedPeer_;
    }

private:
    static constexpr std::size_t kMaxDatagramSize = 65536;
    static constexpr std::size_t kBioQueueSize = 128 * 1024;
    BIO* appBio_{};
    BIO* sslBio_{};
    Http3QuicDatagramAddress boundAddress_{};
    BIO_ADDR* injectSource_{};
    BIO_ADDR* injectDestination_{};
    BIO_ADDR* outboundLocal_{};
    BIO_ADDR* outboundPeer_{};
    std::array<std::byte, kMaxDatagramSize> buffer_{};
    std::size_t outboundSize_{};
    Http3QuicDatagramAddress outboundDestination_{};
    Http3QuicDatagramAddress outboundSource_{};
    Http3QuicDatagramAddress lastInjectedPeer_{};
    std::uint64_t injectionGeneration_{};
    bool outboundHasSource_{};
    bool outboundInFlight_{};
};

}  // namespace ruvia::detail
