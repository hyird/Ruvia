#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include <openssl/bio.h>
#include <openssl/err.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

#include "test_harness.h"

namespace {
using Bridge = ruvia::detail::Http3QuicDatagramBridge;
using Address = ruvia::detail::Http3QuicDatagramAddress;
using Outbound = ruvia::detail::Http3QuicOutboundDatagram;

Address ipv4(std::uint8_t last, std::uint16_t port) {
    Address result;
    result.family = Address::Family::kIPv4;
    result.address[0] = 127;
    result.address[3] = last;
    result.port = port;
    return result;
}

Address wildcardIpv4(std::uint16_t port) {
    Address result;
    result.family = Address::Family::kIPv4;
    result.port = port;
    return result;
}

Address ipv6(std::uint8_t last, std::uint16_t port) {
    Address result;
    result.family = Address::Family::kIPv6;
    result.address[0] = 0x20;
    result.address[1] = 0x01;
    result.address[15] = last;
    result.port = port;
    return result;
}

Address wildcardIpv6(std::uint16_t port) {
    Address result;
    result.family = Address::Family::kIPv6;
    result.port = port;
    return result;
}

Address ipv6LinkLocal(std::uint16_t port) {
    Address result;
    result.family = Address::Family::kIPv6;
    result.address[0] = 0xfe;
    result.address[1] = 0x80;
    result.address[15] = 1;
    result.port = port;
    return result;
}

struct BioOwner final {
    ~BioOwner() {
        BIO_free(bio);
    }
    BIO* bio{};
};

struct BioAddressOwner final {
    ~BioAddressOwner() {
        BIO_ADDR_free(address);
    }
    BIO_ADDR* address{BIO_ADDR_new()};
};

std::uint16_t networkPort(std::uint16_t port) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(port);
    }
    return port;
}

bool makeBioAddress(const Address& source, BIO_ADDR* target) {
    if (target == nullptr) {
        return false;
    }
    const bool ipv4Address = source.family == Address::Family::kIPv4;
    const std::size_t length = ipv4Address ? 4 : 16;
    return BIO_ADDR_rawmake(target, ipv4Address ? AF_INET : AF_INET6, source.address.data(),
               length, networkPort(source.port)) == 1;
}

bool readBioAddress(const BIO_ADDR* source, Address& target) {
    if (source == nullptr) {
        return false;
    }
    const int family = BIO_ADDR_family(source);
    const std::size_t expected = family == AF_INET ? 4 : family == AF_INET6 ? 16
                                                                            : 0;
    if (expected == 0) {
        return false;
    }
    Address parsed;
    parsed.family = family == AF_INET ? Address::Family::kIPv4 : Address::Family::kIPv6;
    std::size_t length = parsed.address.size();
    if (BIO_ADDR_rawaddress(source, parsed.address.data(), &length) != 1 || length != expected) {
        return false;
    }
    parsed.port = networkPort(static_cast<std::uint16_t>(BIO_ADDR_rawport(source)));
    target = parsed;
    return true;
}

bool sameAddress(const Address& actual, const Address& expected) {
    const std::size_t length = expected.family == Address::Family::kIPv4 ? 4 : 16;
    return actual.family == expected.family && actual.port == expected.port &&
           std::equal(expected.address.begin(), expected.address.begin() + length,
               actual.address.begin());
}

bool send(BIO* bio, std::span<const std::byte> bytes, BIO_ADDR* peer, BIO_ADDR* local = nullptr) {
    BIO_MSG message{};
    message.data = const_cast<std::byte*>(bytes.data());
    message.data_len = bytes.size();
    message.peer = peer;
    message.local = local;
    std::size_t processed = 0;
    return BIO_sendmmsg(bio, &message, sizeof(message), 1, 0, &processed) == 1 && processed == 1;
}

bool receive(BIO* bio, std::span<std::byte> buffer, BIO_ADDR* local, BIO_ADDR* peer,
    std::size_t& received) {
    BIO_ADDR_clear(local);
    BIO_ADDR_clear(peer);
    BIO_MSG message{};
    message.data = buffer.data();
    message.data_len = buffer.size();
    message.local = local;
    message.peer = peer;
    std::size_t processed = 0;
    const int result = BIO_recvmmsg(bio, &message, sizeof(message), 1, 0, &processed);
    received = message.data_len;
    return result == 1 && processed == 1;
}

}  // namespace

RUVIA_TEST(http3QuicDatagramBridgeConfiguresAddressCapabilities) {
    Bridge bridge(ipv4(1, 4433));
    BioOwner ssl{bridge.releaseSslBio()};
    RUVIA_CHECK(ssl.bio != nullptr);
    if (ssl.bio == nullptr) {
        return;
    }

    constexpr std::uint32_t expectedCaps = BIO_DGRAM_CAP_HANDLES_SRC_ADDR |
                                           BIO_DGRAM_CAP_HANDLES_DST_ADDR |
                                           BIO_DGRAM_CAP_PROVIDES_SRC_ADDR |
                                           BIO_DGRAM_CAP_PROVIDES_DST_ADDR;
    RUVIA_CHECK(BIO_dgram_get_caps(ssl.bio) == expectedCaps);
    RUVIA_CHECK(BIO_dgram_get_effective_caps(ssl.bio) == expectedCaps);
    RUVIA_CHECK(BIO_dgram_get_local_addr_cap(ssl.bio) == 1);

    int enabled = 0;
    RUVIA_CHECK(BIO_dgram_get_local_addr_enable(ssl.bio, &enabled) == 1);
    RUVIA_CHECK(enabled == 1);
}

RUVIA_TEST(http3QuicDatagramBridgeInjectsPeerAndBoundLocalDestination) {
    const Address bound = ipv4(1, 4433);
    Bridge bridge(bound);
    BioOwner ssl{bridge.releaseSslBio()};
    RUVIA_CHECK(ssl.bio != nullptr);
    const Address peerA = ipv4(2, 0x1234);
    const Address peerB = ipv4(3, 0x5678);
    const std::array<std::byte, 2> packetA{std::byte{0x42}, std::byte{0x43}};
    const std::array<std::byte, 3> packetB{std::byte{0x51}, std::byte{0x52}, std::byte{0x53}};
    const Address wrongLocal = ipv4(9, bound.port);

    RUVIA_CHECK(bridge.inject(packetA, peerA, wrongLocal) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packetA, peerA) == Bridge::InjectResult::kAccepted);
    RUVIA_CHECK(bridge.inject(packetB, peerB) == Bridge::InjectResult::kAccepted);

    std::array<std::byte, 8> buffer{};
    BioAddressOwner sourceOwner;
    BioAddressOwner destinationOwner;
    RUVIA_CHECK(sourceOwner.address != nullptr);
    RUVIA_CHECK(destinationOwner.address != nullptr);
    for (const auto& expected : {std::span<const std::byte>(packetA),
             std::span<const std::byte>(packetB)}) {
        std::size_t received = 0;
        RUVIA_CHECK(receive(ssl.bio, buffer, destinationOwner.address, sourceOwner.address,
            received));
        const Address& expectedPeer = expected.data() == packetA.data() ? peerA : peerB;
        Address actualPeer;
        Address actualDestination;
        RUVIA_CHECK(readBioAddress(sourceOwner.address, actualPeer));
        RUVIA_CHECK(readBioAddress(destinationOwner.address, actualDestination));
        RUVIA_CHECK(sameAddress(actualPeer, expectedPeer));
        RUVIA_CHECK(sameAddress(actualDestination, bound));
        RUVIA_CHECK(received == expected.size());
        RUVIA_CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
    }
}

RUVIA_TEST(http3QuicDatagramBridgePreservesWildcardLocalDestinationPerDatagram) {
    constexpr std::uint16_t port = 4433;
    Bridge bridge(wildcardIpv4(port));
    BioOwner ssl{bridge.releaseSslBio()};
    RUVIA_CHECK(ssl.bio != nullptr);

    const Address peer = ipv4(2, 0x2345);
    const std::array<std::byte, 2> packetA{std::byte{0xa1}, std::byte{0xa2}};
    const std::array<std::byte, 3> packetB{std::byte{0xb1}, std::byte{0xb2}, std::byte{0xb3}};
    const Address destinationA = ipv4(7, port);
    const Address destinationB = ipv4(8, port);

    RUVIA_CHECK(bridge.inject(packetA, peer) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packetA, peer, wildcardIpv4(port)) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packetA, peer, ipv4(7, 0)) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packetA, peer, ipv6(7, port)) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packetA, peer, ipv4(7, port + 1)) == Bridge::InjectResult::kFatal);
    Address scopedDestination = destinationA;
    scopedDestination.scopeId = 1;
    RUVIA_CHECK(bridge.inject(packetA, peer, scopedDestination) == Bridge::InjectResult::kFatal);

    RUVIA_CHECK(bridge.inject(packetA, peer, destinationA) == Bridge::InjectResult::kAccepted);
    RUVIA_CHECK(bridge.inject(packetB, peer, destinationB) == Bridge::InjectResult::kAccepted);

    std::array<std::byte, 8> buffer{};
    BioAddressOwner sourceOwner;
    BioAddressOwner destinationOwner;
    const std::array expectedPackets{std::span<const std::byte>(packetA),
        std::span<const std::byte>(packetB)};
    const std::array expectedDestinations{destinationA, destinationB};
    for (std::size_t i = 0; i < expectedPackets.size(); ++i) {
        std::size_t received = 0;
        RUVIA_CHECK(receive(ssl.bio, buffer, destinationOwner.address, sourceOwner.address,
            received));
        Address actualPeer;
        Address actualDestination;
        RUVIA_CHECK(readBioAddress(sourceOwner.address, actualPeer));
        RUVIA_CHECK(readBioAddress(destinationOwner.address, actualDestination));
        RUVIA_CHECK(sameAddress(actualPeer, peer));
        RUVIA_CHECK(sameAddress(actualDestination, expectedDestinations[i]));
        RUVIA_CHECK(received == expectedPackets[i].size());
        RUVIA_CHECK(std::equal(expectedPackets[i].begin(), expectedPackets[i].end(), buffer.begin()));
    }

    BIO_MSG emptyMessage{};
    emptyMessage.data = buffer.data();
    emptyMessage.data_len = buffer.size();
    std::size_t processed = 0;
    RUVIA_CHECK(BIO_recvmmsg(ssl.bio, &emptyMessage, sizeof(emptyMessage), 1, 0, &processed) == 0);
    RUVIA_CHECK(processed == 0);
    ERR_clear_error();
}

RUVIA_TEST(http3QuicDatagramBridgeAcceptsWildcardIpv6Bind) {
    constexpr std::uint16_t port = 4433;
    Bridge bridge(wildcardIpv6(port));
    BioOwner ssl{bridge.releaseSslBio()};
    const Address peer = ipv6(2, 0x2345);
    const Address local = ipv6(3, port);
    const std::array<std::byte, 1> packet{std::byte{0x66}};
    Address scopedLocal = local;
    scopedLocal.scopeId = 1;
    RUVIA_CHECK(bridge.inject(packet, peer, scopedLocal) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packet, peer, ipv6LinkLocal(port)) == Bridge::InjectResult::kFatal);
    RUVIA_CHECK(bridge.inject(packet, peer, local) == Bridge::InjectResult::kAccepted);

    std::array<std::byte, 8> buffer{};
    BioAddressOwner sourceOwner;
    BioAddressOwner destinationOwner;
    std::size_t received = 0;
    RUVIA_CHECK(receive(ssl.bio, buffer, destinationOwner.address, sourceOwner.address, received));
    Address actualPeer;
    Address actualLocal;
    RUVIA_CHECK(readBioAddress(sourceOwner.address, actualPeer));
    RUVIA_CHECK(readBioAddress(destinationOwner.address, actualLocal));
    RUVIA_CHECK(sameAddress(actualPeer, peer));
    RUVIA_CHECK(sameAddress(actualLocal, local));
    RUVIA_CHECK(received == packet.size());
    RUVIA_CHECK(std::equal(packet.begin(), packet.end(), buffer.begin()));
}

RUVIA_TEST(http3QuicDatagramBridgeBuildsConcretePeerBioAddresses) {
    BioAddressOwner owner;
    RUVIA_CHECK(owner.address != nullptr);
    if (owner.address == nullptr) {
        return;
    }

    auto check = [&](const Address& expected, int family, std::size_t bytes) {
        RUVIA_CHECK(ruvia::detail::makeHttp3QuicBioAddress(expected, owner.address));
        RUVIA_CHECK(BIO_ADDR_family(owner.address) == family);
        std::array<std::uint8_t, 16> actual{};
        std::size_t length = actual.size();
        RUVIA_CHECK(BIO_ADDR_rawaddress(owner.address, actual.data(), &length) == 1);
        RUVIA_CHECK(length == bytes);
        RUVIA_CHECK(std::equal(expected.address.begin(), expected.address.begin() + bytes,
            actual.begin()));
        auto port = BIO_ADDR_rawport(owner.address);
        if constexpr (std::endian::native == std::endian::little) {
            port = std::byteswap(port);
        }
        RUVIA_CHECK(port == expected.port);
    };
    check(ipv4(2, 0x1234), AF_INET, 4);
    check(ipv6(1, 0x4567), AF_INET6, 16);
    auto invalid = ipv4(2, 0);
    RUVIA_CHECK(!ruvia::detail::makeHttp3QuicBioAddress(invalid, owner.address));
}

RUVIA_TEST(http3QuicDatagramBridgeOutboundAddressAndBorrowedBytesLifecycle) {
    Bridge bridge(ipv4(1, 4433));
    BioOwner ssl{bridge.releaseSslBio()};
    const Address destination = ipv4(9, 0x2345);
    const Address source = ipv4(7, 0x3456);
    BioAddressOwner destinationOwner;
    BioAddressOwner sourceOwner;
    BIO_ADDR* const peer = destinationOwner.address;
    BIO_ADDR* const local = sourceOwner.address;
    RUVIA_CHECK(peer != nullptr);
    RUVIA_CHECK(local != nullptr);
    RUVIA_CHECK(makeBioAddress(destination, peer));
    RUVIA_CHECK(makeBioAddress(source, local));

    const std::array<std::byte, 3> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    RUVIA_CHECK(send(ssl.bio, bytes, peer, local));

    Outbound outbound;
    RUVIA_CHECK(bridge.takeOutbound(outbound) == Bridge::OutboundResult::kReady);
    RUVIA_CHECK(outbound.hasSource);
    RUVIA_CHECK(sameAddress(outbound.destination, destination));
    RUVIA_CHECK(sameAddress(outbound.source, source));
    RUVIA_CHECK(outbound.bytes.size() == bytes.size());
    RUVIA_CHECK(std::equal(bytes.begin(), bytes.end(), outbound.bytes.begin()));
    RUVIA_CHECK(bridge.takeOutbound(outbound) == Bridge::OutboundResult::kBusy);
    RUVIA_CHECK(outbound.hasSource);
    RUVIA_CHECK(sameAddress(outbound.destination, destination));
    RUVIA_CHECK(sameAddress(outbound.source, source));
    RUVIA_CHECK(outbound.bytes.size() == bytes.size());
    RUVIA_CHECK(std::equal(bytes.begin(), bytes.end(), outbound.bytes.begin()));
    bridge.completeOutbound();

    RUVIA_CHECK(send(ssl.bio, bytes, peer));
    RUVIA_CHECK(bridge.takeOutbound(outbound) == Bridge::OutboundResult::kReady);
    RUVIA_CHECK(!outbound.hasSource);
    RUVIA_CHECK(sameAddress(outbound.destination, destination));
    RUVIA_CHECK(outbound.bytes.size() == bytes.size());
    RUVIA_CHECK(std::equal(bytes.begin(), bytes.end(), outbound.bytes.begin()));
    bridge.completeOutbound();
    RUVIA_CHECK(bridge.takeOutbound(outbound) == Bridge::OutboundResult::kEmpty);
}

RUVIA_TEST(http3QuicDatagramBridgeQueueFullRecoversAfterConsumption) {
    Bridge bridge(ipv4(1, 4433));
    BioOwner ssl{bridge.releaseSslBio()};
    const Address peer = ipv4(2, 0x4321);
    const std::array<std::byte, 1> packet{std::byte{0x5a}};
    std::size_t accepted = 0;
    for (;;) {
        const auto result = bridge.inject(packet, peer);
        if (result == Bridge::InjectResult::kFull) {
            break;
        }
        RUVIA_CHECK(result == Bridge::InjectResult::kAccepted);
        ++accepted;
        RUVIA_CHECK(accepted < 200000);
    }
    RUVIA_CHECK(accepted != 0);

    BIO_MSG message{};
    std::array<std::byte, 8> buffer{};
    message.data = buffer.data();
    message.data_len = buffer.size();
    std::size_t processed = 0;
    RUVIA_CHECK(BIO_recvmmsg(ssl.bio, &message, sizeof(message), 1, 0, &processed) == 1);
    RUVIA_CHECK(processed == 1);
    RUVIA_CHECK(bridge.inject(packet, peer) == Bridge::InjectResult::kAccepted);
}

RUVIA_TEST(http3QuicDatagramBridgeRejectsLinkLocalAndReportsBrokenBioFatal) {
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { Bridge invalid(ipv6LinkLocal(4433)); }));

    Bridge bridge(ipv4(1, 4433));
    BioOwner ssl{bridge.releaseSslBio()};
    BIO_free(ssl.bio);
    ssl.bio = nullptr;
    Outbound outbound;
    RUVIA_CHECK(bridge.takeOutbound(outbound) == Bridge::OutboundResult::kFatal);
}
