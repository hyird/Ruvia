#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <span>
#include <system_error>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3UdpSocket.h"

#include "test_harness.h"

namespace {
using Socket = ruvia::detail::http3_udp_socket;
using Udp = asio::ip::udp;
using namespace std::chrono_literals;

[[noreturn]] void fixtureLifetimeViolation(const char* fixture) noexcept {
    std::fprintf(stderr,
        "[FATAL FIXTURE LIFETIME] %s did not drain every borrowed completion; terminating\n",
        fixture);
    std::fflush(stderr);
    std::terminate();
}

struct Exchange final {
    Socket* server{};
    Udp::endpoint replySource;
    std::array<std::byte, 8> replyBytes{};
    std::size_t replySize{};
    std::array<std::byte, 64> requestBytes{};
    std::size_t requestSize{};
    bool receivedIntoOwnedStorage{};
    Udp::endpoint receivedPeer;
    Udp::endpoint local_destination;
    std::array<std::byte, 64> replyBuffer{};
    Udp::endpoint replyPeer;
    int receiveCalls{};
    int sendCalls{};
    int replyCalls{};
    int requestSendCalls{};
    std::error_code receiveError;
    std::error_code sendError;
    std::error_code replyError;
    std::error_code requestError;
    std::size_t sentSize{};
    std::size_t requestSentSize{};
    std::size_t replyReceivedSize{};
    bool sendAccepted{};
    bool requestSent{};
};

void onSend(void* object, std::error_code error, std::size_t size) noexcept {
    auto& exchange = *static_cast<Exchange*>(object);
    ++exchange.sendCalls;
    exchange.sendError = error;
    exchange.sentSize = size;
}

void onReceive(void* object, std::error_code error, Socket::receive_view view) noexcept {
    auto& exchange = *static_cast<Exchange*>(object);
    ++exchange.receiveCalls;
    exchange.receiveError = error;
    if (error) {
        return;
    }
    exchange.receivedPeer = std::move(view.peer);
    exchange.local_destination = std::move(view.local_destination);
    exchange.requestSize = std::min(view.bytes.size(), exchange.requestBytes.size());
    exchange.receivedIntoOwnedStorage = view.bytes.data() == exchange.requestBytes.data();
    exchange.sendAccepted = exchange.server->async_send(
        Socket::send_view{exchange.replySource, exchange.receivedPeer,
            std::span<const std::byte>(exchange.replyBytes.data(), exchange.replySize)},
        &exchange, onSend);
}

Exchange runExchange(asio::io_context& io, Socket& server, Udp::socket& client,
    const Udp::endpoint& destination, const Udp::endpoint& replySource,
    std::span<const std::byte> request) {
    if (io.stopped()) {
        io.restart();
    }
    Exchange exchange;
    exchange.server = &server;
    exchange.replySource = replySource;
    exchange.replyBytes = {std::byte{'r'}, std::byte{'e'}, std::byte{'p'},
        std::byte{'l'}, std::byte{'y'}, std::byte{'!'}, std::byte{}, std::byte{}};
    exchange.replySize = 6;

    bool receiveAccepted = false;
    bool clientReceiveAccepted = false;
    bool clientSendAccepted = false;
    const auto cancelAndDrain = [&]() noexcept {
        server.request_stop();
        asio::error_code ignored;
        client.cancel(ignored);
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(2s);
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll();
        const int expectedReceiveCalls = receiveAccepted ? 1 : 0;
        const int expectedSendCalls = exchange.sendAccepted ? 1 : 0;
        const int expectedReplyCalls = clientReceiveAccepted ? 1 : 0;
        const int expectedClientSendCalls = clientSendAccepted ? 1 : 0;
        if (!server.done() || exchange.receiveCalls != expectedReceiveCalls ||
            exchange.sendCalls != expectedSendCalls || exchange.replyCalls != expectedReplyCalls ||
            exchange.requestSendCalls != expectedClientSendCalls) {
            fixtureLifetimeViolation("runExchange cancellation/drain");
        }
    };

    try {
        client.async_receive_from(asio::buffer(exchange.replyBuffer), exchange.replyPeer,
            [&exchange](const asio::error_code& error, std::size_t size) noexcept {
                ++exchange.replyCalls;
                exchange.replyError = error;
                exchange.replyReceivedSize = size;
            });
        clientReceiveAccepted = true;
        receiveAccepted = server.async_receive(exchange.requestBytes, &exchange, onReceive);
        client.async_send_to(asio::buffer(request), destination,
            [&exchange, expectedSize = request.size()](const asio::error_code& error,
                std::size_t size) noexcept {
                ++exchange.requestSendCalls;
                exchange.requestError = error;
                exchange.requestSentSize = size;
                exchange.requestSent = !error && size == expectedSize;
            });
        clientSendAccepted = true;
        io.run_for(2s);
    } catch (...) {
        cancelAndDrain();
        throw;
    }

    const bool complete = receiveAccepted && exchange.receiveCalls == 1 &&
                          exchange.sendCalls == (exchange.sendAccepted ? 1 : 0) &&
                          exchange.replyCalls == 1 && exchange.requestSendCalls == 1;
    if (!complete) {
        cancelAndDrain();
    }
    if (!receiveAccepted) {
        exchange.receiveError = std::make_error_code(std::errc::operation_not_permitted);
    }
    return exchange;
}

bool isOperationCanceled(std::error_code error) noexcept {
    return error == asio::error::operation_aborted ||
           error == std::errc::operation_canceled;
}

bool isIPv6Unavailable(std::error_code error) noexcept {
    return error == asio::error::address_family_not_supported ||
           error == std::errc::protocol_not_supported ||
           error == asio::error::operation_not_supported ||
           error == std::errc::address_not_available ||
           error == std::errc::network_unreachable;
}

struct StopReceive final {
    Socket* socket{};
    int calls{};
    std::error_code firstError;
    std::error_code secondError;
    std::size_t firstSize{};
    std::size_t secondSize{};
    bool rearm{};
    bool rearmAccepted{};
    bool postStopRearmAccepted{};
    bool doneInsideCompletion{};
    std::array<std::byte, 64> bytes{};
};

struct SendCompletionState final {
    int calls{};
    std::error_code error;
    std::size_t size{};
};

struct DatagramSendState final {
    int calls{};
    asio::error_code error;
    std::size_t size{};
};

void onCancellationSend(void* object, std::error_code error, std::size_t size) noexcept {
    auto& state = *static_cast<SendCompletionState*>(object);
    ++state.calls;
    state.error = error;
    state.size = size;
}

void onStopReceive(void* object, std::error_code error,
    Socket::receive_view view) noexcept {
    auto& state = *static_cast<StopReceive*>(object);
    ++state.calls;
    if (state.calls == 1) {
        state.firstError = error;
        state.firstSize = view.bytes.size();
        if (state.rearm) {
            state.rearmAccepted = state.socket->async_receive(state.bytes, &state, onStopReceive);
            state.socket->request_stop();
            state.doneInsideCompletion = state.socket->done();
            state.postStopRearmAccepted = state.socket->async_receive(state.bytes, &state, onStopReceive);
        }
    } else {
        state.secondError = error;
        state.secondSize = view.bytes.size();
    }
}

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t liveBytes{};
    std::size_t allocations{};
    std::size_t returns{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* const result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        liveBytes += bytes;
        ++allocations;
        return result;
    }

    void do_deallocate(void* memory, std::size_t bytes, std::size_t alignment) override {
        liveBytes -= bytes;
        ++returns;
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct SendRound final {
    std::pmr::vector<std::byte> payload;
    std::pmr::vector<std::byte>* retainedSent{};
    std::pmr::vector<std::byte>* retainedReceived{};
    bool payloadAlive{true};
    bool sendBorrowAlive{};
    int sendCalls{};
    int receiveCalls{};
    std::error_code sendError;
    std::error_code receiveError;
    std::size_t sentSize{};
    std::size_t receivedSize{};
    std::array<std::byte, 64> received{};
    Udp::endpoint sender;

    explicit SendRound(std::pmr::memory_resource* resource)
        : payload(resource) {}
};

void onRoundSend(void* object, std::error_code error, std::size_t size) noexcept {
    auto& round = *static_cast<SendRound*>(object);
    ++round.sendCalls;
    round.sendError = error;
    round.sentSize = size;
    round.sendBorrowAlive = round.payloadAlive;
    if (round.payloadAlive && !error && !round.payload.empty()) {
        round.retainedSent->insert(round.retainedSent->end(),
            round.payload.begin(), round.payload.end());
    }
}

}  // namespace

RUVIA_TEST(http3NetworkUdpSocketPreservesPktinfoAndExplicitReplySource) {
    asio::io_context io;
    Socket server(io, Udp::endpoint(asio::ip::address_v4::any(), 0));
    server.prepare();
    RUVIA_CHECK(server.bound_port() != 0);

    const auto loopback1 = asio::ip::address_v4::loopback();
    const auto loopback2 = asio::ip::address_v4({127, 0, 0, 2});
    Udp::socket client1(io, Udp::endpoint(loopback1, 0));
    Udp::socket client2(io, Udp::endpoint(loopback2, 0));
#ifdef _WIN32
    const Udp::endpoint source(loopback1, server.bound_port());
#else
    const Udp::endpoint source(loopback2, server.bound_port());
#endif
    const std::array<std::byte, 4> firstRequest{
        std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}};
    const std::array<std::byte, 5> secondRequest{
        std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}};

    auto first = runExchange(io, server, client1, Udp::endpoint(loopback1, server.bound_port()),
        source, firstRequest);
    RUVIA_CHECK(first.requestSent);
    RUVIA_CHECK(first.requestSendCalls == 1);
    RUVIA_CHECK(!first.requestError);
    RUVIA_CHECK(first.requestSentSize == firstRequest.size());
    RUVIA_CHECK(first.receiveCalls == 1);
    RUVIA_CHECK(!first.receiveError);
    RUVIA_CHECK(first.receivedPeer == client1.local_endpoint());
    RUVIA_CHECK(first.local_destination == Udp::endpoint(loopback1, server.bound_port()));
    RUVIA_CHECK(first.receivedIntoOwnedStorage);
    RUVIA_CHECK(first.requestSize == firstRequest.size());
    RUVIA_CHECK(std::equal(firstRequest.begin(), firstRequest.end(), first.requestBytes.begin()));
    RUVIA_CHECK(first.sendAccepted);
    RUVIA_CHECK(first.sendCalls == 1);
    RUVIA_CHECK(!first.sendError);
    RUVIA_CHECK(first.sentSize == first.replySize);
    RUVIA_CHECK(first.replyCalls == 1);
    RUVIA_CHECK(!first.replyError);
    RUVIA_CHECK(first.replyReceivedSize == first.replySize);
    RUVIA_CHECK(first.replyPeer == source);
    RUVIA_CHECK(std::equal(first.replyBytes.begin(),
        first.replyBytes.begin() + static_cast<std::ptrdiff_t>(first.replySize),
        first.replyBuffer.begin()));
    if (first.receiveCalls != 1 || first.sendCalls != 1 || first.replyCalls != 1 ||
        first.requestSendCalls != 1 || first.receiveError || first.sendError || first.replyError ||
        first.requestError) {
        return;
    }

    // Late replies to a departed peer must leave the shared receive side usable.
    client1.close();
    Exchange closed_peer;
    const bool late_send = server.async_send(
        Socket::send_view{source, first.receivedPeer, firstRequest}, &closed_peer, onSend);
    RUVIA_CHECK(late_send);
    io.restart();
    io.run_for(50ms);
    RUVIA_CHECK_EQ(closed_peer.sendCalls, 1);
    RUVIA_CHECK(!closed_peer.sendError);
    if (!late_send || closed_peer.sendCalls != 1 || closed_peer.sendError) {
        server.request_stop();
        io.restart();
        io.run_for(2s);
        if (!server.done()) {
            fixtureLifetimeViolation("closed peer send");
        }
        return;
    }

    auto second = runExchange(io, server, client2, Udp::endpoint(loopback2, server.bound_port()),
        source, secondRequest);
    RUVIA_CHECK(second.requestSent);
    RUVIA_CHECK(second.requestSendCalls == 1);
    RUVIA_CHECK(!second.requestError);
    RUVIA_CHECK(second.requestSentSize == secondRequest.size());
    RUVIA_CHECK(second.receiveCalls == 1);
    RUVIA_CHECK(!second.receiveError);
    RUVIA_CHECK(second.receivedPeer == client2.local_endpoint());
    RUVIA_CHECK(second.local_destination == Udp::endpoint(loopback2, server.bound_port()));
    RUVIA_CHECK(second.receivedIntoOwnedStorage);
    RUVIA_CHECK(second.requestSize == secondRequest.size());
    RUVIA_CHECK(std::equal(secondRequest.begin(), secondRequest.end(), second.requestBytes.begin()));
    RUVIA_CHECK(second.sendAccepted);
    RUVIA_CHECK(second.sendCalls == 1);
    RUVIA_CHECK(!second.sendError);
    RUVIA_CHECK(second.sentSize == second.replySize);
    RUVIA_CHECK(second.replyCalls == 1);
    RUVIA_CHECK(!second.replyError);
    RUVIA_CHECK(second.replyReceivedSize == second.replySize);
    RUVIA_CHECK(second.replyPeer == source);
    RUVIA_CHECK(std::equal(second.replyBytes.begin(),
        second.replyBytes.begin() + static_cast<std::ptrdiff_t>(second.replySize),
        second.replyBuffer.begin()));
    if (second.receiveCalls != 1 || second.sendCalls != 1 || second.replyCalls != 1 ||
        second.requestSendCalls != 1 || second.receiveError || second.sendError || second.replyError ||
        second.requestError) {
        return;
    }

    server.request_stop();
    RUVIA_CHECK(server.done());

    try {
        Socket ipv6Server(io, Udp::endpoint(asio::ip::address_v6::any(), 0));
        try {
            ipv6Server.prepare();
        } catch (const std::system_error& error) {
            if (isIPv6Unavailable(error.code())) {
                std::fprintf(stderr,
                    "[SKIP] IPv6 network (::1) unavailable during prepare: %s\n",
                    error.code().message().c_str());
            } else {
                std::fprintf(stderr, "IPv6 network prepare failed: %s\n",
                    error.code().message().c_str());
                RUVIA_CHECK(false && "unexpected IPv6 network prepare failure");
            }
            ipv6Server.request_stop();
            RUVIA_CHECK(ipv6Server.done());
            return;
        }

        Udp::socket ipv6Client(io);
        asio::error_code ipv6Error;
        ipv6Client.open(Udp::v6(), ipv6Error);
        if (!ipv6Error) {
            ipv6Client.bind(Udp::endpoint(asio::ip::address_v6::loopback(), 0), ipv6Error);
        }
        if (ipv6Error) {
            if (isIPv6Unavailable(ipv6Error)) {
                std::fprintf(stderr,
                    "[SKIP] IPv6 network (::1) unavailable for client bind: %s\n",
                    ipv6Error.message().c_str());
            } else {
                std::fprintf(stderr, "IPv6 network client setup failed: %s\n",
                    ipv6Error.message().c_str());
                RUVIA_CHECK(false && "unexpected IPv6 network client setup failure");
            }
            ipv6Server.request_stop();
            RUVIA_CHECK(ipv6Server.done());
            return;
        }

        const auto ipv6Loopback = asio::ip::address_v6::loopback();
        const Udp::endpoint ipv6Source(ipv6Loopback, ipv6Server.bound_port());
        const std::array<std::byte, 3> ipv6Request{
            std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
        auto ipv6 = runExchange(io, ipv6Server, ipv6Client,
            Udp::endpoint(ipv6Loopback, ipv6Server.bound_port()), ipv6Source, ipv6Request);
        RUVIA_CHECK(ipv6.requestSent);
        RUVIA_CHECK(ipv6.requestSendCalls == 1);
        RUVIA_CHECK(!ipv6.requestError);
        RUVIA_CHECK(ipv6.requestSentSize == ipv6Request.size());
        RUVIA_CHECK(ipv6.receiveCalls == 1);
        RUVIA_CHECK(!ipv6.receiveError);
        RUVIA_CHECK(ipv6.receivedPeer == ipv6Client.local_endpoint());
        RUVIA_CHECK(ipv6.local_destination ==
                    Udp::endpoint(ipv6Loopback, ipv6Server.bound_port()));
        RUVIA_CHECK(ipv6.receivedIntoOwnedStorage);
        RUVIA_CHECK(ipv6.requestSize == ipv6Request.size());
        RUVIA_CHECK(std::equal(ipv6Request.begin(), ipv6Request.end(), ipv6.requestBytes.begin()));
        RUVIA_CHECK(ipv6.sendAccepted);
        RUVIA_CHECK(ipv6.sendCalls == 1);
        RUVIA_CHECK(!ipv6.sendError);
        RUVIA_CHECK(ipv6.sentSize == ipv6.replySize);
        RUVIA_CHECK(ipv6.replyCalls == 1);
        RUVIA_CHECK(!ipv6.replyError);
        RUVIA_CHECK(ipv6.replyReceivedSize == ipv6.replySize);
        RUVIA_CHECK(ipv6.replyPeer == ipv6Source);
        RUVIA_CHECK(std::equal(ipv6.replyBytes.begin(),
            ipv6.replyBytes.begin() + static_cast<std::ptrdiff_t>(ipv6.replySize),
            ipv6.replyBuffer.begin()));
        ipv6Server.request_stop();
        RUVIA_CHECK(ipv6Server.done());
    } catch (const std::exception& error) {
        RUVIA_CHECK(false && "unexpected IPv6 network setup/runtime exception");
        std::fprintf(stderr, "IPv6 network exception: %s\n", error.what());
    }
}

RUVIA_TEST(http3NetworkUdpSocketStopDrainsReceiveCompletions) {
    constexpr int kRounds = 12;
    asio::io_context io;
    const auto loopback = asio::ip::address_v4::loopback();
    {
        Socket cold(io, Udp::endpoint(loopback, 0));
        StopReceive coldState{.socket = &cold, .firstError = {}, .secondError = {}};
        RUVIA_CHECK(!cold.async_receive(coldState.bytes, &coldState, onStopReceive));
        RUVIA_CHECK_EQ(coldState.calls, 0);
        cold.prepare();
        cold.request_stop();
        RUVIA_CHECK(cold.done());
    }

    for (int round = 0; round < kRounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        Socket socket(io, Udp::endpoint(loopback, 0));
        socket.prepare();
        StopReceive state{.socket = &socket, .firstError = {}, .secondError = {}};
        const bool accepted = socket.async_receive(state.bytes, &state, onStopReceive);
        RUVIA_CHECK(accepted);
        const bool duplicateAccepted = socket.async_receive(state.bytes, &state, onStopReceive);
        RUVIA_CHECK(!duplicateAccepted);
#ifndef _WIN32
        RUVIA_CHECK_EQ(io.poll_one(), 1U);
        RUVIA_CHECK_EQ(state.calls, 0);
#endif
        socket.request_stop();
        RUVIA_CHECK(!socket.done());
        io.run_for(2s);
        if (!socket.done()) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
        }
        RUVIA_CHECK(state.calls == 1);
        RUVIA_CHECK(isOperationCanceled(state.firstError));
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixtureLifetimeViolation("pending receive stop");
        }
        if (state.calls != 1) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }

    for (int round = 0; round < kRounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        Socket socket(io, Udp::endpoint(loopback, 0));
        socket.prepare();
        Udp::socket sender(io, Udp::endpoint(loopback, 0));
        StopReceive state{.socket = &socket, .firstError = {}, .secondError = {}, .rearm = true};
        const bool accepted = socket.async_receive(state.bytes, &state, onStopReceive);
        RUVIA_CHECK(accepted);
        const std::array<std::byte, 2> packet{std::byte{0x41}, std::byte{0x42}};
        DatagramSendState sendState;
        bool sendAccepted = false;
        const auto cancelAndDrain = [&]() noexcept {
            socket.request_stop();
            asio::error_code ignored;
            sender.cancel(ignored);
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
            if (io.stopped()) {
                io.restart();
            }
            (void)io.poll();
            const int expectedReceiveCalls = (accepted ? 1 : 0) +
                                             (state.rearmAccepted ? 1 : 0);
            const int expectedSendCalls = sendAccepted ? 1 : 0;
            if (!socket.done() || state.calls != expectedReceiveCalls ||
                sendState.calls != expectedSendCalls) {
                fixtureLifetimeViolation("receive completion/stop race cleanup");
            }
        };
        try {
            sender.async_send_to(asio::buffer(packet),
                Udp::endpoint(loopback, socket.bound_port()),
                [&sendState](const asio::error_code& error, std::size_t size) noexcept {
                    ++sendState.calls;
                    sendState.error = error;
                    sendState.size = size;
                });
            sendAccepted = true;
            io.run_for(2s);
        } catch (...) {
            cancelAndDrain();
            throw;
        }
        if (!socket.done() || state.calls != 2 || sendState.calls != 1) {
            cancelAndDrain();
        }
        RUVIA_CHECK(state.calls == 2);
        RUVIA_CHECK(!sendState.error);
        RUVIA_CHECK(sendState.size == packet.size());
        RUVIA_CHECK(!state.firstError);
        RUVIA_CHECK(state.firstSize == packet.size());
        RUVIA_CHECK(state.rearmAccepted);
        RUVIA_CHECK(!state.doneInsideCompletion);
        RUVIA_CHECK(!state.postStopRearmAccepted);
        RUVIA_CHECK(isOperationCanceled(state.secondError));
        RUVIA_CHECK(state.secondSize == 0);
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixtureLifetimeViolation("receive completion/stop race");
        }
        if (state.calls != 2) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }

    for (int round = 0; round < kRounds; ++round) {
        if (io.stopped()) {
            io.restart();
        }
        Socket socket(io, Udp::endpoint(asio::ip::address_v4::any(), 0));
        socket.prepare();
        Udp::socket peer(io, Udp::endpoint(loopback, 0));
        SendCompletionState state;
        const std::array<std::byte, 3> payload{
            std::byte{0x51}, std::byte{0x52}, std::byte{0x53}};
        const bool accepted = socket.async_send(
            Socket::send_view{Udp::endpoint(loopback, socket.bound_port()),
                peer.local_endpoint(), payload},
            &state, onCancellationSend);
        RUVIA_CHECK(accepted);
        socket.request_stop();
        RUVIA_CHECK(!socket.done());
        io.run_for(2s);
        if (!socket.done()) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2s);
        }
        RUVIA_CHECK(state.calls == 1);
        RUVIA_CHECK(!state.error || isOperationCanceled(state.error));
        RUVIA_CHECK(state.error ? state.size == 0 : state.size == payload.size());
        RUVIA_CHECK(socket.done());
        if (!socket.done()) {
            fixtureLifetimeViolation("pending send cancellation");
        }
        if (state.calls != 1) {
            return;
        }
        if (io.stopped()) {
            io.restart();
        }
        RUVIA_CHECK_EQ(io.poll(), 0U);
    }
}

RUVIA_TEST(http3NetworkUdpSocketBorrowsSendBytesUntilCompletion) {
    constexpr std::size_t kRounds = 10;
    constexpr std::size_t kPayloadSize = 37;
    asio::io_context io;
    const auto loopback = asio::ip::address_v4::loopback();
    Socket server(io, Udp::endpoint(asio::ip::address_v4::any(), 0));
    server.prepare();
    Udp::socket peer(io, Udp::endpoint(loopback, 0));
    const Udp::endpoint explicitSource(loopback, server.bound_port());

    CountingResource resource;
    {
        std::pmr::vector<std::byte> retainedSent(&resource);
        std::pmr::vector<std::byte> retainedReceived(&resource);
        retainedSent.reserve(kRounds * kPayloadSize);
        retainedReceived.reserve(kRounds * kPayloadSize);
        const std::size_t retainedAllocationBytes = resource.liveBytes;

        SendCompletionState invalidSourceState;
        Socket::send_view invalidSource{
            Udp::endpoint(asio::ip::address_v4::any(), server.bound_port()),
            peer.local_endpoint(), {}};
        RUVIA_CHECK(!server.async_send(invalidSource, &invalidSourceState, onCancellationSend));
        RUVIA_CHECK_EQ(invalidSourceState.calls, 0);
        SendCompletionState invalidPortState;
        Socket::send_view invalidPort{
            Udp::endpoint(loopback, static_cast<std::uint16_t>(server.bound_port() + 1)),
            peer.local_endpoint(), {}};
        RUVIA_CHECK(!server.async_send(invalidPort, &invalidPortState, onCancellationSend));
        RUVIA_CHECK_EQ(invalidPortState.calls, 0);

        for (std::size_t roundIndex = 0; roundIndex < kRounds; ++roundIndex) {
            if (io.stopped()) {
                io.restart();
            }
            {
                SendRound round(&resource);
                round.retainedSent = &retainedSent;
                round.retainedReceived = &retainedReceived;
                round.payload.resize(kPayloadSize);
                for (std::size_t byte = 0; byte < round.payload.size(); ++byte) {
                    round.payload[byte] = static_cast<std::byte>(
                        (roundIndex * 19 + byte * 7) & 0xff);
                }

                peer.async_receive_from(asio::buffer(round.received), round.sender,
                    [&round](const asio::error_code& error, std::size_t size) noexcept {
                        ++round.receiveCalls;
                        round.receiveError = error;
                        round.receivedSize = size;
                        if (!error && size != 0) {
                            round.retainedReceived->insert(round.retainedReceived->end(),
                                round.received.begin(),
                                round.received.begin() + static_cast<std::ptrdiff_t>(size));
                        }
                    });
                const bool accepted = server.async_send(
                    Socket::send_view{explicitSource, peer.local_endpoint(), round.payload},
                    &round, onRoundSend);
                RUVIA_CHECK(accepted);
                RUVIA_CHECK(round.sendCalls == 0);
                RUVIA_CHECK(round.receiveCalls == 0);
                io.run_for(2s);
                RUVIA_CHECK(round.sendCalls == 1);
                RUVIA_CHECK(!round.sendError);
                RUVIA_CHECK(round.sendBorrowAlive);
                RUVIA_CHECK(round.sentSize == kPayloadSize);
                RUVIA_CHECK(round.receiveCalls == 1);
                RUVIA_CHECK(!round.receiveError);
                RUVIA_CHECK(round.receivedSize == kPayloadSize);
                RUVIA_CHECK(round.sender == explicitSource);
                RUVIA_CHECK(std::equal(round.payload.begin(), round.payload.end(),
                    round.received.begin()));
                if (round.sendCalls != 1 || round.receiveCalls != 1 ||
                    round.sendError || round.receiveError) {
                    server.request_stop();
                    asio::error_code ignored;
                    peer.cancel(ignored);
                    if (io.stopped()) {
                        io.restart();
                    }
                    io.run_for(2s);
                    if (io.stopped()) {
                        io.restart();
                    }
                    (void)io.poll();
                    const int expectedSendCalls = accepted ? 1 : 0;
                    if (!server.done() || round.sendCalls != expectedSendCalls ||
                        round.receiveCalls != 1) {
                        fixtureLifetimeViolation("SendRound timeout cleanup");
                    }
                    return;
                }
                round.payloadAlive = false;
            }
            RUVIA_CHECK(resource.liveBytes == retainedAllocationBytes);
        }

        if (io.stopped()) {
            io.restart();
        }
        {
            SendRound zeroLength(&resource);
            zeroLength.retainedSent = &retainedSent;
            zeroLength.retainedReceived = &retainedReceived;
            peer.async_receive_from(asio::buffer(zeroLength.received), zeroLength.sender,
                [&zeroLength](const asio::error_code& error, std::size_t size) noexcept {
                    ++zeroLength.receiveCalls;
                    zeroLength.receiveError = error;
                    zeroLength.receivedSize = size;
                });
            const bool accepted = server.async_send(
                Socket::send_view{explicitSource, peer.local_endpoint(), zeroLength.payload},
                &zeroLength, onRoundSend);
            RUVIA_CHECK(accepted);
            io.run_for(2s);
            RUVIA_CHECK(zeroLength.sendCalls == 1);
            RUVIA_CHECK(!zeroLength.sendError);
            RUVIA_CHECK(zeroLength.sendBorrowAlive);
            RUVIA_CHECK_EQ(zeroLength.sentSize, 0U);
            RUVIA_CHECK(zeroLength.receiveCalls == 1);
            RUVIA_CHECK(!zeroLength.receiveError);
            RUVIA_CHECK_EQ(zeroLength.receivedSize, 0U);
            RUVIA_CHECK(zeroLength.sender == explicitSource);
            if (zeroLength.sendCalls != 1 || zeroLength.receiveCalls != 1) {
                server.request_stop();
                asio::error_code ignored;
                peer.cancel(ignored);
                if (io.stopped()) {
                    io.restart();
                }
                io.run_for(2s);
                if (io.stopped()) {
                    io.restart();
                }
                (void)io.poll();
                if (!server.done() || zeroLength.sendCalls != (accepted ? 1 : 0) ||
                    zeroLength.receiveCalls != 1) {
                    fixtureLifetimeViolation("zero-length SendRound timeout cleanup");
                }
                return;
            }
            zeroLength.payloadAlive = false;
        }
        RUVIA_CHECK(resource.liveBytes == retainedAllocationBytes);
        RUVIA_CHECK(retainedSent.size() == kRounds * kPayloadSize);
        RUVIA_CHECK(retainedReceived.size() == kRounds * kPayloadSize);
        for (std::size_t roundIndex = 0; roundIndex < kRounds; ++roundIndex) {
            for (std::size_t byte = 0; byte < kPayloadSize; ++byte) {
                const auto expected = static_cast<std::byte>((roundIndex * 19 + byte * 7) & 0xff);
                const auto index = roundIndex * kPayloadSize + byte;
                RUVIA_CHECK(retainedSent[index] == expected);
                RUVIA_CHECK(retainedReceived[index] == expected);
            }
        }
        RUVIA_CHECK(resource.liveBytes == retainedAllocationBytes);
    }
    RUVIA_CHECK_EQ(resource.liveBytes, 0U);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
    server.request_stop();
    RUVIA_CHECK(server.done());
}
