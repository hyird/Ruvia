#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <system_error>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"

#include "test_harness.h"

namespace {
using Endpoint = ruvia::detail::Http3DatagramEndpoint;
using Udp = asio::ip::udp;
using namespace std::chrono_literals;

struct NotificationState final {
    int inputAvailable{};
    int outputDrained{};
    int stopping{};
    Endpoint* endpoint{};
    bool repeatStop{};
};

void onNotification(void* context, Endpoint::notification_kind kind) noexcept {
    auto& state = *static_cast<NotificationState*>(context);
    switch (kind) {
        case Endpoint::notification_kind::input_available:
            ++state.inputAvailable;
            break;
        case Endpoint::notification_kind::output_drained:
            ++state.outputDrained;
            break;
        case Endpoint::notification_kind::stopping:
            ++state.stopping;
            if (state.repeatStop && state.endpoint != nullptr) {
                state.endpoint->request_stop();
            }
            break;
        case Endpoint::notification_kind::io_retired:
            break;
    }
}

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
        endpoint.request_stop();
        if (!runUntil(io, [&] { return endpoint.socket_done(); }) ||
            endpoint.status() == Endpoint::stop_status::pending) {
            std::terminate();
        }
    }
};

struct DatagramState final {
    int calls{};
    asio::error_code error;
    std::size_t size{};
    Udp::endpoint peer;
    std::array<std::byte, 128> bytes{};
};

void receiveOne(Udp::socket& socket, DatagramState& state) {
    socket.async_receive_from(asio::buffer(state.bytes), state.peer,
        [&state](const asio::error_code& error, std::size_t size) noexcept {
            state.error = error;
            state.size = size;
            ++state.calls;
        });
}

}  // namespace

RUVIA_TEST(http3NetworkDatagramEndpointLoopbackReceivePreservesPacketMetadata) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    RUVIA_CHECK(endpoint.bound_port() != 0);
    RUVIA_CHECK(endpoint.start() == Endpoint::pump_result::pending);

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto destination = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.bound_port());
    const std::array<std::byte, 0> empty{};
    const std::array<std::byte, 5> packet{
        std::byte{0x13}, std::byte{0x24}, std::byte{0x35}, std::byte{0x46}, std::byte{0x57}};
    asio::error_code error;
    RUVIA_CHECK(peer.send_to(asio::buffer(empty), destination, 0, error) == 0);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(peer.send_to(asio::buffer(packet), destination, 0, error) == packet.size());
    RUVIA_CHECK(!error);
    RUVIA_CHECK(runUntil(io, [&] { return notifications.inputAvailable == 1; }));

    const auto received = endpoint.receive_slot();
    RUVIA_CHECK(received.has_value());
    if (received) {
        RUVIA_CHECK_EQ(received->bytes.size(), packet.size());
        RUVIA_CHECK(std::equal(packet.begin(), packet.end(), received->bytes.begin()));
        RUVIA_CHECK(received->peer == peer.local_endpoint());
        RUVIA_CHECK(received->local_destination == destination);
    }
    RUVIA_CHECK(endpoint.receive_slot().has_value());
    RUVIA_CHECK(endpoint.consume_receive() == Endpoint::pump_result::pending);
    RUVIA_CHECK_EQ(notifications.inputAvailable, 1);
    RUVIA_CHECK(!endpoint.error());

    endpoint.request_stop();
    RUVIA_CHECK(runUntil(io, [&] { return endpoint.socket_done(); }));
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::done);
    RUVIA_CHECK_EQ(notifications.inputAvailable, 1);
    RUVIA_CHECK_EQ(notifications.outputDrained, 0);
    RUVIA_CHECK_EQ(notifications.stopping, 1);
}

RUVIA_TEST(http3NetworkDatagramEndpointSendOwnsBytesUntilCompletion) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::any(), 0),
        Endpoint::notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    RUVIA_CHECK(endpoint.start() == Endpoint::pump_result::pending);

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::array<DatagramState, 2> received{};
    receiveOne(peer, received[0]);
    receiveOne(peer, received[1]);
    const auto source = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.bound_port());
    const std::array<std::byte, 6> first{
        std::byte{0x61}, std::byte{0x62}, std::byte{0x63},
        std::byte{0x71}, std::byte{0x72}, std::byte{0x73}};
    const std::array<std::byte, 4> second{
        std::byte{0x81}, std::byte{0x82}, std::byte{0x83}, std::byte{0x84}};

    RUVIA_CHECK(endpoint.send_datagram(first, source, peer.local_endpoint()) ==
                Endpoint::pump_result::pending);
    RUVIA_CHECK(endpoint.send_in_flight());
    RUVIA_CHECK(endpoint.send_datagram(second, source, peer.local_endpoint()) ==
                Endpoint::pump_result::pending);
    RUVIA_CHECK(runUntil(io, [&] { return received[0].calls == 1; }));
    RUVIA_CHECK(!received[0].error);
    RUVIA_CHECK_EQ(received[0].size, first.size());
    RUVIA_CHECK(std::equal(first.begin(), first.end(), received[0].bytes.begin()));
    RUVIA_CHECK(!endpoint.send_in_flight());
    RUVIA_CHECK_EQ(notifications.outputDrained, 1);

    RUVIA_CHECK(endpoint.send_datagram(second, source, peer.local_endpoint()) ==
                Endpoint::pump_result::pending);
    RUVIA_CHECK(runUntil(io, [&] { return received[1].calls == 1; }));
    RUVIA_CHECK(!received[1].error);
    RUVIA_CHECK_EQ(received[1].size, second.size());
    RUVIA_CHECK(std::equal(second.begin(), second.end(), received[1].bytes.begin()));
    RUVIA_CHECK_EQ(notifications.outputDrained, 2);

    endpoint.request_stop();
    RUVIA_CHECK(runUntil(io, [&] { return endpoint.socket_done(); }));
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::done);
}

RUVIA_TEST(http3NetworkDatagramEndpointHoldsReceiveSlotUntilExplicitConsumption) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    RUVIA_CHECK(endpoint.start() == Endpoint::pump_result::pending);

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto destination = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.bound_port());
    const std::array<std::byte, 4> first{std::byte{0x91}, std::byte{0x82}, std::byte{0x73}, std::byte{0x64}};
    const std::array<std::byte, 3> second{std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
    asio::error_code error;
    RUVIA_CHECK(peer.send_to(asio::buffer(first), destination, 0, error) == first.size());
    RUVIA_CHECK(!error);
    RUVIA_CHECK(runUntil(io, [&] { return notifications.inputAvailable == 1; }));
    const auto held = endpoint.receive_slot();
    RUVIA_CHECK(held.has_value());
    if (!held) {
        return;
    }
    RUVIA_CHECK(peer.send_to(asio::buffer(second), destination, 0, error) == second.size());
    RUVIA_CHECK(!error);
    io.run_for(20ms);
    RUVIA_CHECK_EQ(notifications.inputAvailable, 1);
    RUVIA_CHECK(std::equal(first.begin(), first.end(), held->bytes.begin()));

    RUVIA_CHECK(endpoint.consume_receive() == Endpoint::pump_result::pending);
    RUVIA_CHECK(runUntil(io, [&] { return notifications.inputAvailable == 2; }));
    const auto next = endpoint.receive_slot();
    RUVIA_CHECK(next.has_value());
    if (next) {
        RUVIA_CHECK_EQ(next->bytes.size(), second.size());
        RUVIA_CHECK(std::equal(second.begin(), second.end(), next->bytes.begin()));
    }
    endpoint.request_stop();
    RUVIA_CHECK(runUntil(io, [&] { return endpoint.socket_done(); }));
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::done);
}

RUVIA_TEST(http3NetworkDatagramEndpointStopDrainsOutstandingCallbacksAndReceiveSlot) {
    asio::io_context io;
    NotificationState notifications;
    Endpoint endpoint(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::notification{&notifications, onNotification});
    EndpointDrain drain{io, endpoint};
    endpoint.prepare();
    RUVIA_CHECK(endpoint.start() == Endpoint::pump_result::pending);

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto destination = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.bound_port());
    const std::array<std::byte, 3> packet{std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}};
    asio::error_code error;
    RUVIA_CHECK(peer.send_to(asio::buffer(packet), destination, 0, error) == packet.size());
    RUVIA_CHECK(!error);
    RUVIA_CHECK(runUntil(io, [&] { return notifications.inputAvailable == 1; }));
    RUVIA_CHECK(endpoint.receive_slot().has_value());
    notifications.endpoint = &endpoint;
    notifications.repeatStop = true;
    endpoint.request_stop();
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::done);
    RUVIA_CHECK(runUntil(io, [&] { return endpoint.socket_done(); }));
    RUVIA_CHECK(!endpoint.send_in_flight());
    RUVIA_CHECK(!endpoint.receive_slot().has_value());
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::done);
    RUVIA_CHECK_EQ(notifications.stopping, 1);
}

RUVIA_TEST(http3NetworkDatagramEndpointRejectsInvalidAddressesAndOversizedSends) {
    asio::io_context io;
    NotificationState notifications;
    const Endpoint::notification callback{&notifications, onNotification};
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] {
        NotificationState wildcardNotifications;
        Endpoint wildcard(io, Udp::endpoint(asio::ip::address_v4::any(), 0),
            Endpoint::notification{&wildcardNotifications, onNotification});
        EndpointDrain drain{io, wildcard};
        wildcard.prepare();
        RUVIA_CHECK(wildcard.bound_port() != 0);
    }));
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] {
        NotificationState ipv6_notifications;
        Endpoint wildcard_v6(io, Udp::endpoint(asio::ip::address_v6::any(), 0),
            Endpoint::notification{&ipv6_notifications, onNotification});
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
    RUVIA_CHECK(endpoint.start() == Endpoint::pump_result::pending);
    const auto wrongSource = Udp::endpoint(asio::ip::address_v4({127, 0, 0, 2}), endpoint.bound_port());
    const auto destination = Udp::endpoint(asio::ip::address_v4::loopback(), endpoint.bound_port());
    const std::array<std::byte, 2> packet{std::byte{0xd1}, std::byte{0xd2}};
    RUVIA_CHECK(endpoint.send_datagram(packet, wrongSource, destination) == Endpoint::pump_result::error);
    RUVIA_CHECK(endpoint.error() == std::make_error_code(std::errc::bad_message));
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::pending);
    RUVIA_CHECK(runUntil(io, [&] { return endpoint.socket_done(); }));
    RUVIA_CHECK_EQ(notifications.outputDrained, 0);
    RUVIA_CHECK_EQ(notifications.stopping, 1);
    RUVIA_CHECK(endpoint.status() == Endpoint::stop_status::error);

    asio::io_context oversizedIo;
    NotificationState oversizedNotifications;
    Endpoint oversizedEndpoint(oversizedIo,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        Endpoint::notification{&oversizedNotifications, onNotification});
    EndpointDrain oversizedDrain{oversizedIo, oversizedEndpoint};
    oversizedEndpoint.prepare();
    RUVIA_CHECK(oversizedEndpoint.start() == Endpoint::pump_result::pending);
    std::array<std::byte, 65508> oversized{};
    const auto oversizedSource = Udp::endpoint(asio::ip::address_v4::loopback(),
        oversizedEndpoint.bound_port());
    RUVIA_CHECK(oversizedEndpoint.send_datagram(oversized, oversizedSource, destination) ==
                Endpoint::pump_result::error);
    RUVIA_CHECK(oversizedEndpoint.error() == std::make_error_code(std::errc::message_size));
    RUVIA_CHECK(runUntil(oversizedIo, [&] { return oversizedEndpoint.socket_done(); }));
    RUVIA_CHECK_EQ(oversizedNotifications.outputDrained, 0);
    RUVIA_CHECK_EQ(oversizedNotifications.stopping, 1);
    RUVIA_CHECK(oversizedEndpoint.status() == Endpoint::stop_status::error);
}
