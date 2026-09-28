#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/bio.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

#include "test_harness.h"

namespace {
using Endpoint = ruvia::detail::Http3DatagramEndpoint;
using Address = ruvia::detail::Http3QuicDatagramAddress;
using Udp = asio::ip::udp;
using namespace std::chrono_literals;

struct NotificationState final {
    int inputAvailable{};
    int outputDrained{};
    int stopping{};
    Endpoint* endpoint{};
    bool repeatStop{};
};

void onNotification(void* context, Endpoint::NotificationKind kind) noexcept {
    auto& state = *static_cast<NotificationState*>(context);
    switch (kind) {
        case Endpoint::NotificationKind::kInputAvailable:
            ++state.inputAvailable;
            break;
        case Endpoint::NotificationKind::kOutputDrained:
            ++state.outputDrained;
            break;
        case Endpoint::NotificationKind::kStopping:
            ++state.stopping;
            if (state.repeatStop && state.endpoint != nullptr) {
                state.endpoint->requestStop();
            }
            break;
    }
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

struct DatagramState final {
    int calls{};
    asio::error_code error;
    std::size_t size{};
    Udp::endpoint peer;
    std::array<std::byte, 128> bytes{};
};

struct SendState final {
    int calls{};
    asio::error_code error;
    std::size_t size{};
};

template <class Predicate>
bool runUntil(asio::io_context& io, Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(5ms);
    }
    return predicate();
}

struct EndpointDrain final {
    asio::io_context& io;
    Endpoint& endpoint;

    ~EndpointDrain() {
        endpoint.requestStop();
        if (!runUntil(io, [&] { return endpoint.socketDone(); }) ||
            endpoint.stopStatus() == Endpoint::StopStatus::kPending) {
            std::terminate();
        }
    }
};

bool sameBioAddress(const BIO_ADDR* actual, const Address& expected) {
    BioAddressOwner expectedOwner;
    if (actual == nullptr || expectedOwner.address == nullptr ||
        !ruvia::detail::makeHttp3QuicBioAddress(expected, expectedOwner.address)) {
        return false;
    }
    if (BIO_ADDR_family(actual) != BIO_ADDR_family(expectedOwner.address) ||
        BIO_ADDR_rawport(actual) != BIO_ADDR_rawport(expectedOwner.address)) {
        return false;
    }
    std::array<std::uint8_t, 16> actualBytes{};
    std::array<std::uint8_t, 16> expectedBytes{};
    std::size_t actualSize = actualBytes.size();
    std::size_t expectedSize = expectedBytes.size();
    return BIO_ADDR_rawaddress(actual, actualBytes.data(), &actualSize) == 1 &&
           BIO_ADDR_rawaddress(expectedOwner.address, expectedBytes.data(), &expectedSize) == 1 &&
           actualSize == expectedSize &&
           std::equal(actualBytes.begin(), actualBytes.begin() + static_cast<std::ptrdiff_t>(actualSize),
               expectedBytes.begin());
}

bool receiveBio(BIO* bio, std::span<std::byte> buffer, BIO_ADDR* local, BIO_ADDR* peer,
    std::size_t& size) {
    if (bio == nullptr || local == nullptr || peer == nullptr) {
        return false;
    }
    BIO_ADDR_clear(local);
    BIO_ADDR_clear(peer);
    BIO_MSG message{};
    message.data = buffer.data();
    message.data_len = buffer.size();
    message.local = local;
    message.peer = peer;
    std::size_t processed = 0;
    const int result = BIO_recvmmsg(bio, &message, sizeof(message), 1, 0, &processed);
    size = message.data_len;
    return result == 1 && processed == 1;
}

bool sendBio(BIO* bio, std::span<const std::byte> bytes,
    const Udp::endpoint& peerEndpoint, std::optional<Udp::endpoint> localEndpoint = std::nullopt) {
    const auto peerAddress = ruvia::detail::toHttp3QuicDatagramAddress(peerEndpoint);
    if (!peerAddress) {
        return false;
    }
    BioAddressOwner peerOwner;
    BioAddressOwner localOwner;
    if (peerOwner.address == nullptr || localOwner.address == nullptr ||
        !ruvia::detail::makeHttp3QuicBioAddress(*peerAddress, peerOwner.address)) {
        return false;
    }

    BIO_ADDR* local = nullptr;
    if (localEndpoint) {
        const auto localAddress = ruvia::detail::toHttp3QuicDatagramAddress(*localEndpoint);
        if (!localAddress || !ruvia::detail::makeHttp3QuicBioAddress(*localAddress, localOwner.address)) {
            return false;
        }
        local = localOwner.address;
    }

    BIO_MSG message{};
    message.data = const_cast<std::byte*>(bytes.data());
    message.data_len = bytes.size();
    message.peer = peerOwner.address;
    message.local = local;
    std::size_t processed = 0;
    return BIO_sendmmsg(bio, &message, sizeof(message), 1, 0, &processed) == 1 && processed == 1;
}

}  // namespace

RUVIA_TEST(http3NetworkDatagramEndpointLoopbackReceivePreservesBioPacketMetadata) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::Notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    RUVIA_CHECK(endpoint.boundPort() != 0);

    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        SendState sent;
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        std::array<std::byte, 128> bioBytes{};
        const std::array<std::byte, 5> packet{
            std::byte{0x13}, std::byte{0x24}, std::byte{0x35}, std::byte{0x46}, std::byte{0x57}};

        RUVIA_CHECK(endpoint.start() == Endpoint::PumpResult::kPending);
        const std::array<std::byte, 0> emptyPacket{};
        peer.async_send_to(asio::buffer(emptyPacket),
            Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort()),
            [&sent](const asio::error_code& error, std::size_t size) noexcept {
                sent.error = error;
                sent.size = size;
                ++sent.calls;
            });
        RUVIA_CHECK(runUntil(io, [&] { return sent.calls == 1; }));
        RUVIA_CHECK(!sent.error);
        RUVIA_CHECK_EQ(sent.size, std::size_t{0});
        peer.async_send_to(asio::buffer(packet), Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort()),
            [&sent](const asio::error_code& error, std::size_t size) noexcept {
                sent.error = error;
                sent.size = size;
                ++sent.calls;
            });
        RUVIA_CHECK(runUntil(io, [&] {
            return sent.calls == 2 && notifications.inputAvailable == 1;
        }));

        RUVIA_CHECK(!sent.error);
        RUVIA_CHECK_EQ(sent.size, packet.size());
        BioAddressOwner actualLocal;
        BioAddressOwner actualPeer;
        std::size_t receivedSize = 0;
        RUVIA_CHECK(actualLocal.address != nullptr);
        RUVIA_CHECK(actualPeer.address != nullptr);
        RUVIA_CHECK(receiveBio(ssl.bio, bioBytes, actualLocal.address,
            actualPeer.address, receivedSize));
        const auto expectedPeer = ruvia::detail::toHttp3QuicDatagramAddress(peer.local_endpoint());
        const auto expectedLocal = ruvia::detail::toHttp3QuicDatagramAddress(
            Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort()));
        RUVIA_CHECK(expectedPeer.has_value());
        RUVIA_CHECK(expectedLocal.has_value());
        if (expectedPeer && expectedLocal) {
            RUVIA_CHECK(sameBioAddress(actualPeer.address, *expectedPeer));
            RUVIA_CHECK(sameBioAddress(actualLocal.address, *expectedLocal));
        }
        RUVIA_CHECK_EQ(receivedSize, packet.size());
        RUVIA_CHECK(std::equal(packet.begin(), packet.end(), bioBytes.begin()));
        RUVIA_CHECK_EQ(notifications.outputDrained, 0);

        endpoint.requestStop();
        RUVIA_CHECK(runUntil(io, [&] { return endpoint.socketDone(); }));
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kDone);
    RUVIA_CHECK_EQ(notifications.inputAvailable, 1);
    RUVIA_CHECK_EQ(notifications.outputDrained, 0);
    RUVIA_CHECK_EQ(notifications.stopping, 1);
    RUVIA_CHECK(!endpoint.error());
}

RUVIA_TEST(http3NetworkDatagramEndpointLoopbackSendKeepsBridgeBytesUntilCompletion) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::any(), 0),
        Endpoint::Notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();

    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto source =
            Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort());
        std::array<DatagramState, 2> received{};
        for (std::size_t i = 0; i < received.size(); ++i) {
            peer.async_receive_from(asio::buffer(received[i].bytes), received[i].peer,
                [&received, i](const asio::error_code& error, std::size_t size) noexcept {
                    received[i].error = error;
                    received[i].size = size;
                    ++received[i].calls;
                });
        }
        const std::array<std::byte, 6> packet{
            std::byte{0x61}, std::byte{0x62}, std::byte{0x63},
            std::byte{0x71}, std::byte{0x72}, std::byte{0x73}};
        const std::array<std::byte, 4> secondPacket{
            std::byte{0x81}, std::byte{0x82}, std::byte{0x83}, std::byte{0x84}};
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kIdle);
        RUVIA_CHECK(!endpoint.sendInFlight());
        RUVIA_CHECK_EQ(notifications.outputDrained, 0);
        RUVIA_CHECK(sendBio(ssl.bio, packet, peer.local_endpoint(), source));
        RUVIA_CHECK(sendBio(ssl.bio, secondPacket, peer.local_endpoint(), source));
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kPending);
        RUVIA_CHECK(endpoint.sendInFlight());
        ruvia::detail::Http3QuicOutboundDatagram outstanding;
        RUVIA_CHECK(lease.bridge().takeOutbound(outstanding) ==
                    ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kBusy);

        // Exercise the receive slot while the send bytes are borrowed from the
        // bridge's fixed storage.
        RUVIA_CHECK(endpoint.start() == Endpoint::PumpResult::kPending);
        RUVIA_CHECK(runUntil(io, [&] {
            return received[0].calls == 1 && received[1].calls == 1 &&
                   notifications.outputDrained == 1;
        }));
        RUVIA_CHECK(!endpoint.sendInFlight());
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kIdle);
        RUVIA_CHECK_EQ(notifications.outputDrained, 1);
        RUVIA_CHECK(!received[0].error);
        RUVIA_CHECK(!received[1].error);
        RUVIA_CHECK_EQ(received[0].size, packet.size());
        RUVIA_CHECK_EQ(received[1].size, secondPacket.size());
        RUVIA_CHECK(std::equal(packet.begin(), packet.end(), received[0].bytes.begin()));
        RUVIA_CHECK(std::equal(secondPacket.begin(), secondPacket.end(), received[1].bytes.begin()));
        RUVIA_CHECK(received[0].peer == Udp::endpoint(
                                            asio::ip::address_v4::loopback(), endpoint.boundPort()));
        RUVIA_CHECK(received[1].peer == received[0].peer);
        RUVIA_CHECK(lease.bridge().takeOutbound(outstanding) ==
                    ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty);
        RUVIA_CHECK_EQ(notifications.inputAvailable, 0);

        endpoint.requestStop();
        RUVIA_CHECK(runUntil(io, [&] { return endpoint.socketDone(); }));
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kDone);
}

RUVIA_TEST(http3NetworkDatagramEndpointHoldsBioFullReceiveUntilExplicitRetry) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::Notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();

    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        SendState sent;
        const auto local = ruvia::detail::toHttp3QuicDatagramAddress(
            Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort()));
        const auto remote = ruvia::detail::toHttp3QuicDatagramAddress(peer.local_endpoint());
        RUVIA_CHECK(local.has_value());
        RUVIA_CHECK(remote.has_value());
        if (!local || !remote) {
            return;
        }

        std::array<std::byte, 4096> filler{};
        filler.fill(std::byte{0xee});
        std::size_t queued = 0;
        for (;;) {
            const auto result = lease.bridge().inject(filler, *remote, *local);
            if (result == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kFull) {
                break;
            }
            RUVIA_CHECK(result == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted);
            if (result != ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted) {
                return;
            }
            ++queued;
            RUVIA_CHECK(queued < 128);
            if (queued >= 128) {
                return;
            }
        }
        RUVIA_CHECK(queued != 0);
        const std::array<std::byte, 1> tailFiller{std::byte{0x7e}};
        std::size_t queuedTail = 0;
        for (;;) {
            const auto result = lease.bridge().inject(tailFiller, *remote, *local);
            if (result == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kFull) {
                break;
            }
            RUVIA_CHECK(result == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted);
            if (result != ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted) {
                return;
            }
            ++queuedTail;
            RUVIA_CHECK(queuedTail < 8192);
            if (queuedTail >= 8192) {
                return;
            }
        }

        const std::array<std::byte, 4> packet{
            std::byte{0x91}, std::byte{0x82}, std::byte{0x73}, std::byte{0x64}};
        RUVIA_CHECK(endpoint.start() == Endpoint::PumpResult::kPending);
        peer.async_send_to(asio::buffer(packet), Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort()),
            [&sent](const asio::error_code& error, std::size_t size) noexcept {
                sent.error = error;
                sent.size = size;
                ++sent.calls;
            });
        RUVIA_CHECK(runUntil(io, [&] {
            return sent.calls == 1 && notifications.inputAvailable == 1;
        }));
        RUVIA_CHECK(!sent.error);
        RUVIA_CHECK_EQ(notifications.inputAvailable, 1);
        RUVIA_CHECK(endpoint.retryHeldReceive() == Endpoint::PumpResult::kBackpressured);
        RUVIA_CHECK(endpoint.retryHeldReceive() == Endpoint::PumpResult::kBackpressured);
        RUVIA_CHECK_EQ(notifications.inputAvailable, 1);

        std::array<std::byte, 4096> bioBytes{};
        BioAddressOwner actualLocal;
        BioAddressOwner actualPeer;
        std::size_t size = 0;
        RUVIA_CHECK(receiveBio(ssl.bio, bioBytes, actualLocal.address, actualPeer.address, size));
        RUVIA_CHECK_EQ(size, filler.size());
        RUVIA_CHECK(std::equal(filler.begin(), filler.end(), bioBytes.begin()));
        RUVIA_CHECK(endpoint.retryHeldReceive() == Endpoint::PumpResult::kPending);
        RUVIA_CHECK_EQ(notifications.inputAvailable, 2);

        for (std::size_t i = 1; i < queued; ++i) {
            RUVIA_CHECK(receiveBio(ssl.bio, bioBytes, actualLocal.address,
                actualPeer.address, size));
            RUVIA_CHECK_EQ(size, filler.size());
            RUVIA_CHECK(std::equal(filler.begin(), filler.end(), bioBytes.begin()));
        }
        for (std::size_t i = 0; i < queuedTail; ++i) {
            RUVIA_CHECK(receiveBio(ssl.bio, bioBytes, actualLocal.address,
                actualPeer.address, size));
            RUVIA_CHECK_EQ(size, tailFiller.size());
            RUVIA_CHECK(std::equal(tailFiller.begin(), tailFiller.end(), bioBytes.begin()));
        }
        RUVIA_CHECK(receiveBio(ssl.bio, bioBytes, actualLocal.address,
            actualPeer.address, size));
        RUVIA_CHECK_EQ(size, packet.size());
        RUVIA_CHECK(std::equal(packet.begin(), packet.end(), bioBytes.begin()));
        RUVIA_CHECK(sameBioAddress(actualLocal.address, *local));
        RUVIA_CHECK(sameBioAddress(actualPeer.address, *remote));

        endpoint.requestStop();
        RUVIA_CHECK(runUntil(io, [&] { return endpoint.socketDone(); }));
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kDone);
}

RUVIA_TEST(http3NetworkDatagramEndpointStopDrainsOutstandingSocketCallbacksAndBioLease) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::Notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();

    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        const std::array<std::byte, 3> packet{
            std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
        RUVIA_CHECK(sendBio(ssl.bio, packet,
            Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort())));
        RUVIA_CHECK(endpoint.start() == Endpoint::PumpResult::kPending);
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kPending);
        RUVIA_CHECK(endpoint.sendInFlight());
        notifications.endpoint = &endpoint;
        notifications.repeatStop = true;
        endpoint.requestStop();
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
        RUVIA_CHECK(runUntil(io, [&] { return endpoint.socketDone(); }));
        RUVIA_CHECK(!endpoint.sendInFlight());
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        RUVIA_CHECK(lease.bridge().takeOutbound(outbound) ==
                    ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty);
        RUVIA_CHECK_EQ(notifications.outputDrained, 0);
        RUVIA_CHECK_EQ(notifications.stopping, 1);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kDone);
    RUVIA_CHECK_EQ(notifications.stopping, 1);
}

RUVIA_TEST(http3NetworkDatagramEndpointCompletesRejectedSendAndRejectsUnsupportedAddresses) {
    asio::io_context io;
    NotificationState notifications;
    const Endpoint::Notification callback{&notifications, onNotification};
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] {
        NotificationState wildcardNotifications;
        Endpoint wildcard(io, Udp::endpoint(asio::ip::address_v4::any(), 0),
            Endpoint::Notification{&wildcardNotifications, onNotification});
        EndpointDrain drain{io, wildcard};
        wildcard.prepare();
        RUVIA_CHECK(wildcard.boundPort() != 0);
    }));
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] {
        NotificationState wildcardNotifications;
        Endpoint wildcard(io, Udp::endpoint(asio::ip::address_v6::any(), 0),
            Endpoint::Notification{&wildcardNotifications, onNotification});
        wildcard.requestStop();
    }));
    auto scopedBytes = asio::ip::address_v6::bytes_type{};
    scopedBytes[0] = 0xfe;
    scopedBytes[1] = 0x80;
    scopedBytes[15] = 1;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        Endpoint invalid(io, Udp::endpoint(asio::ip::address_v6(scopedBytes, 7), 0), callback);
    }));
    const auto mapped = asio::ip::address_v6::v4_mapped(asio::ip::address_v4::loopback());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        Endpoint invalid(io, Udp::endpoint(mapped, 0), callback);
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        Endpoint invalid(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), {});
    }));

    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), callback);
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        const auto bound = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.boundPort());
        const auto wrongSource = Udp::endpoint(asio::ip::address_v4({127, 0, 0, 2}),
            endpoint.boundPort());
        const std::array<std::byte, 2> packet{std::byte{0xd1}, std::byte{0xd2}};
        RUVIA_CHECK(sendBio(ssl.bio, packet, bound, wrongSource));
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kError);
        RUVIA_CHECK(endpoint.error() == std::make_error_code(std::errc::bad_message));
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        RUVIA_CHECK(lease.bridge().takeOutbound(outbound) ==
                    ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty);
        RUVIA_CHECK(endpoint.socketDone());
        RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kPending);
        RUVIA_CHECK_EQ(notifications.outputDrained, 0);
        RUVIA_CHECK_EQ(notifications.stopping, 1);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kError);
}

RUVIA_TEST(http3NetworkDatagramEndpointCompletesSendRejectedBeforeAsyncSubmission) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::Notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();

    {
        auto lease = endpoint.acquireBridge();
        BioOwner ssl{lease.bridge().releaseSslBio()};
        std::vector<std::byte> oversized(65508, std::byte{0x5a});
        const auto destination = Udp::endpoint(
            asio::ip::address_v4::loopback(), endpoint.boundPort());
        RUVIA_CHECK(sendBio(ssl.bio, oversized, destination));
        RUVIA_CHECK(endpoint.sendPending() == Endpoint::PumpResult::kError);
        RUVIA_CHECK(endpoint.error() == std::make_error_code(std::errc::io_error));
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        RUVIA_CHECK(lease.bridge().takeOutbound(outbound) ==
                    ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty);
        RUVIA_CHECK(endpoint.socketDone());
        RUVIA_CHECK_EQ(notifications.outputDrained, 0);
        RUVIA_CHECK_EQ(notifications.stopping, 1);
    }
    RUVIA_CHECK(endpoint.stopStatus() == Endpoint::StopStatus::kError);
}
