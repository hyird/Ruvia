#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/WebSocketConnection.h"
#include "ruvia/http/WebSocketServerProtocol.h"

#include "test_harness.h"

namespace {
using namespace ruvia;

struct MaskSource {
    unsigned calls{0};
    bool fail{false};
    static bool generate(void* context, WebSocketMaskKey& key) noexcept {
        auto& self = *static_cast<MaskSource*>(context);
        ++self.calls;
        key = {static_cast<char>(self.calls), '\x23', '\x45', '\x67'};
        return !self.fail;
    }
};

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t liveBytes{0};
    std::size_t allocations{0};
    std::size_t deallocations{0};
    bool reject{false};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        liveBytes += bytes;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

WebSocketConnection client(MaskSource& source, ProtocolByteLimit limit = ProtocolByteLimit::unlimited()) {
    return WebSocketConnection({.messageLimit = limit, .role = WebSocketConnectionRole::kClient, .maskKeyGenerator = &MaskSource::generate, .maskKeyContext = &source});
}

std::string drain(WebSocketConnection& connection) {
    const std::string bytes(connection.outputPlan().bytes());
    if (connection.consumeOutput(bytes.size()) != WebSocketOutputConsumeStatus::kDrained) {
        throw std::runtime_error("output did not drain");
    }
    return bytes;
}
}  // namespace

RUVIA_TEST(ws_public_context_takeover_mixed_messages_and_connection_lifetime) {
    CountingResource memory;
    {
        MaskSource mask;
        WebSocketConnection sender({.resource = &memory, .messageLimit = ProtocolByteLimit::limited(16384), .compression = (WebSocketCompression{.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false}), .role = WebSocketConnectionRole::kClient, .maskKeyGenerator = &MaskSource::generate, .maskKeyContext = &mask, .compressionLevel = 9});
        WebSocketConnection receiver({.resource = &memory, .messageLimit = ProtocolByteLimit::limited(16384), .compression = (WebSocketCompression{.enabled = true, .serverNoContextTakeover = false, .clientNoContextTakeover = false}), .compressionLevel = 9});
        std::string noise(1024, '\0');
        std::uint32_t state = 1234567;
        for (auto& byte : noise) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            byte = static_cast<char>(state);
        }
        std::size_t stableMemory = 0;
        for (int round = 0; round < 256; ++round) {
            const std::string payload = round % 4 == 0 ? noise : std::string(8192, 'x');
            const bool compress = round % 4 != 2;
            RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kBinary, payload, compress) == WebSocketFrameSubmitStatus::kAccepted);
            const auto wire = drain(sender);
            if (!compress || round % 4 == 0) {
                RUVIA_CHECK((static_cast<unsigned char>(wire[0]) & 0x40U) == 0);
            }
            RUVIA_CHECK(receiver.feed(wire) == WebSocketFeedStatus::kAccepted);
            const auto event = receiver.nextEvent();
            RUVIA_CHECK(event && event->message());
            if (!event || !event->message()) {
                return;
            }
            RUVIA_CHECK_EQ(event->message()->payload(), payload);
            if (round == 63) {
                stableMemory = memory.liveBytes;
            }
            if (round > 63) {
                RUVIA_CHECK(memory.liveBytes <= stableMemory);
            }
        }
        RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kPing, "ping") == WebSocketFrameSubmitStatus::kAccepted);
        RUVIA_CHECK((static_cast<unsigned char>(drain(sender)[0]) & 0x40U) == 0);
        RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kBinary, std::string(16385, 'x')) == WebSocketFrameSubmitStatus::kMessageTooLarge);
    }
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
}

RUVIA_TEST(ws_public_server_events_use_public_payload_types) {
    MaskSource mask;
    auto sender = client(mask);
    std::pmr::string input;
    WebSocketServerProtocol protocol(input);

    RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kText, "hey") ==
                WebSocketFrameSubmitStatus::kAccepted);
    const std::string messageWire(sender.outputPlan().bytes());
    RUVIA_CHECK(sender.consumeOutput(messageWire.size()) == WebSocketOutputConsumeStatus::kDrained);
    input.append(messageWire);
    auto message = protocol.poll();
    RUVIA_CHECK(message && message->kind() == WebSocketEventKind::kMessage);
    RUVIA_CHECK(message && message->message());
    if (message && message->message()) {
        RUVIA_CHECK(message->message()->opcode() == WebSocketOpcode::kText);
        RUVIA_CHECK_EQ(message->message()->payload(), "hey");
    }

    RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kPing, "p") ==
                WebSocketFrameSubmitStatus::kAccepted);
    const std::string pingWire(sender.outputPlan().bytes());
    RUVIA_CHECK(sender.consumeOutput(pingWire.size()) == WebSocketOutputConsumeStatus::kDrained);
    input.append(pingWire);
    auto ping = protocol.poll();
    RUVIA_CHECK(ping && ping->kind() == WebSocketEventKind::kPing);
    RUVIA_CHECK(ping && ping->ping());
    if (ping && ping->ping()) {
        RUVIA_CHECK_EQ(ping->ping()->payload(), "p");
    }
}

RUVIA_TEST(ws_public_protocol_reuses_borrowed_input_and_releases_memory) {
    CountingResource memory;
    std::pmr::string input(&memory);
    input.reserve(1024);
    const auto bytesBeforeProtocol = memory.liveBytes;
    {
        WebSocketServerProtocol protocol(input);
        MaskSource mask;
        auto sender = client(mask);
        for (int i = 0; i < 32; ++i) {
            RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kText, "payload") ==
                        WebSocketFrameSubmitStatus::kAccepted);
            const std::string wire(sender.outputPlan().bytes());
            RUVIA_CHECK(sender.consumeOutput(wire.size()) == WebSocketOutputConsumeStatus::kDrained);
            input.append(wire);
            const auto allocationsBeforePoll = memory.allocations;
            auto event = protocol.poll();
            RUVIA_CHECK(event && event->message());
            RUVIA_CHECK_EQ(memory.allocations, allocationsBeforePoll);
            if (event && event->message()) {
                RUVIA_CHECK_EQ(event->message()->payload(), "payload");
                const auto payloadAddress = reinterpret_cast<std::uintptr_t>(
                    event->message()->payload().data());
                const auto inputAddress = reinterpret_cast<std::uintptr_t>(input.data());
                RUVIA_CHECK(payloadAddress >= inputAddress);
                RUVIA_CHECK(payloadAddress + event->message()->payload().size() <=
                            inputAddress + input.size());
            }
        }
        RUVIA_CHECK(protocol.submitFrame(WebSocketOpcode::kText, "out") ==
                    WebSocketFrameSubmitStatus::kAccepted);
        auto pending = protocol.outputPlan().bytes();
        RUVIA_CHECK(protocol.consumeOutput(pending.size()) ==
                    WebSocketOutputConsumeStatus::kDrained);
        const auto liveBeforeSubmit = memory.liveBytes;
        for (int i = 0; i < 32; ++i) {
            RUVIA_CHECK(protocol.submitFrame(WebSocketOpcode::kText, "out") ==
                        WebSocketFrameSubmitStatus::kAccepted);
            pending = protocol.outputPlan().bytes();
            RUVIA_CHECK(protocol.consumeOutput(pending.size()) ==
                        WebSocketOutputConsumeStatus::kDrained);
        }
        // PMR implementations may allocate a transient container proxy on each
        // submit; those allocations must not remain live after output drains.
        RUVIA_CHECK_EQ(memory.liveBytes, liveBeforeSubmit);
        protocol.notifyTransportEof();
        auto end = protocol.poll();
        RUVIA_CHECK(end && end->kind() == WebSocketEventKind::kTransportEnd);
        protocol.commitTransportEnd();
        RUVIA_CHECK(protocol.abort() == WebSocketAbortDisposition::kNoTransportAction);
    }
    RUVIA_CHECK_EQ(memory.liveBytes, bytesBeforeProtocol);
}

RUVIA_TEST(ws_public_server_protocol_reports_protocol_error_as_public_value) {
    std::pmr::string input;
    WebSocketServerProtocol protocol(input);
    // A server-side connection must reject an unmasked client frame.
    input.append("\x81\x01x", 3);
    auto error = protocol.poll();
    RUVIA_CHECK(error && error->kind() == WebSocketEventKind::kProtocolError);
    RUVIA_CHECK(error && error->protocolError() && error->protocolError()->closeCode() == 1002);
}

RUVIA_TEST(ws_public_server_protocol_controls_permessage_deflate_per_frame) {
    std::pmr::string input;
    WebSocketServerProtocol protocol(input, ProtocolByteLimit::unlimited(),
        WebSocketServerProtocolOptions{(WebSocketCompression{.enabled = true}), 6});
    const std::string payload(200, 'x');

    RUVIA_CHECK(protocol.submitFrame(WebSocketOpcode::kText, payload, false) ==
                WebSocketFrameSubmitStatus::kAccepted);
    auto output = protocol.outputPlan().bytes();
    RUVIA_CHECK(!output.empty());
    if (!output.empty()) {
        RUVIA_CHECK((static_cast<unsigned char>(output[0]) & 0x40U) == 0);
    }
    RUVIA_CHECK(protocol.consumeOutput(output.size()) ==
                WebSocketOutputConsumeStatus::kDrained);

    RUVIA_CHECK(protocol.submitFrame(WebSocketOpcode::kText, payload) ==
                WebSocketFrameSubmitStatus::kAccepted);
    output = protocol.outputPlan().bytes();
    RUVIA_CHECK(!output.empty());
    if (!output.empty()) {
        RUVIA_CHECK((static_cast<unsigned char>(output[0]) & 0x40U) != 0);
    }
}

RUVIA_TEST(ws_public_server_protocol_preserves_transport_end_semantics) {
    std::pmr::string input;
    WebSocketServerProtocol protocol(input);
    RUVIA_CHECK(protocol.submitClose(1000, "done") == WebSocketCloseSubmitStatus::kAccepted);
    RUVIA_CHECK(protocol.outputPlan().disposition() ==
                WebSocketTransportDisposition::kKeepOpen);
    const auto output = protocol.outputPlan().bytes();
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(protocol.consumeOutput(output.size()) ==
                WebSocketOutputConsumeStatus::kDrained);
    // After our Close frame is sent, the protocol still awaits the peer's
    // Close; transport EOF terminates that wait without fabricating a reply.
    RUVIA_CHECK(protocol.outputPlan().disposition() ==
                WebSocketTransportDisposition::kKeepOpen);
    protocol.notifyTransportEof();
    RUVIA_CHECK(protocol.outputPlan().disposition() ==
                WebSocketTransportDisposition::kEndTransport);
}

RUVIA_TEST(ws_public_server_eof_discards_pending_output_before_transport_end) {
    std::pmr::string input;
    WebSocketServerProtocol protocol(input);
    RUVIA_CHECK(protocol.submitFrame(WebSocketOpcode::kText, "unsent") ==
                WebSocketFrameSubmitStatus::kAccepted);
    const auto output = protocol.outputPlan().bytes();
    RUVIA_CHECK(output.size() > 1);
    RUVIA_CHECK(protocol.consumeOutput(1) == WebSocketOutputConsumeStatus::kPending);

    protocol.notifyTransportEof();
    const auto end = protocol.outputPlan();
    RUVIA_CHECK(end.bytes().empty());
    RUVIA_CHECK(end.disposition() == WebSocketTransportDisposition::kEndTransport);
    // The driver can finish its direction without consuming bytes discarded by EOF.
    RUVIA_CHECK(protocol.consumeOutput(end.bytes().size()) ==
                WebSocketOutputConsumeStatus::kDrained);
    protocol.commitTransportEnd();
    RUVIA_CHECK(protocol.abort() == WebSocketAbortDisposition::kNoTransportAction);
}

RUVIA_TEST(ws_public_server_keeps_pending_pong_before_peer_close) {
    std::pmr::string input;
    WebSocketServerProtocol protocol(input);
    RUVIA_CHECK(protocol.submitClose(1000, "done") ==
                WebSocketCloseSubmitStatus::kAccepted);
    auto output = protocol.outputPlan().bytes();
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(protocol.consumeOutput(output.size()) ==
                WebSocketOutputConsumeStatus::kDrained);

    // Masked client Ping("p") followed by an empty masked Close.
    input.append(
        "\x89\x81\x01\x02\x03\x04q"
        "\x88\x80\x05\x06\x07\x08",
        13);
    const auto ping = protocol.poll();
    RUVIA_CHECK(ping && ping->kind() == WebSocketEventKind::kPing);
    RUVIA_CHECK(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto close = protocol.poll();
    RUVIA_CHECK(close && close->kind() == WebSocketEventKind::kClose);

    const auto plan = protocol.outputPlan();
    RUVIA_CHECK_EQ(plan.bytes(), std::string_view("\x8a\x01p", 3));
    RUVIA_CHECK(plan.disposition() ==
                WebSocketTransportDisposition::kEndTransport);
    RUVIA_CHECK(protocol.consumeOutput(plan.bytes().size()) ==
                WebSocketOutputConsumeStatus::kDrained);
    protocol.commitTransportEnd();
    RUVIA_CHECK(protocol.livenessMode() == WebSocketLivenessMode::kInactive);
}

RUVIA_TEST(ws_public_client_server_exchange_and_partial_output) {
    MaskSource source;
    auto sender = client(source);
    WebSocketConnection receiver;
    const std::string payload(65536, '\xff');
    for (unsigned round = 0; round < 3; ++round) {
        RUVIA_CHECK(sender.submitFrame(WebSocketOpcode::kBinary, payload) == WebSocketFrameSubmitStatus::kAccepted);
        const std::string wire(sender.outputPlan().bytes());
        RUVIA_CHECK((static_cast<unsigned char>(wire[1]) & 0x80U) != 0);
        RUVIA_CHECK(sender.consumeOutput(wire.size() + 1) == WebSocketOutputConsumeStatus::kOutOfRange);
        RUVIA_CHECK(sender.consumeOutput(1) == WebSocketOutputConsumeStatus::kPending);
        RUVIA_CHECK_EQ(sender.outputPlan().bytes(), std::string_view(wire).substr(1));
        RUVIA_CHECK(sender.consumeOutput(wire.size() - 1) == WebSocketOutputConsumeStatus::kDrained);
        RUVIA_CHECK(receiver.feed(std::string_view(wire).substr(0, 5)) == WebSocketFeedStatus::kAccepted);
        RUVIA_CHECK(!receiver.nextEvent());
        RUVIA_CHECK(receiver.feed(std::string_view(wire).substr(5)) == WebSocketFeedStatus::kAccepted);
        const auto event = receiver.nextEvent();
        RUVIA_CHECK(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        RUVIA_CHECK_EQ(event->message()->payload(), payload);
        RUVIA_CHECK(receiver.submitFrame(WebSocketOpcode::kBinary, event->message()->payload()) == WebSocketFrameSubmitStatus::kAccepted);
        const auto reply = drain(receiver);
        RUVIA_CHECK((static_cast<unsigned char>(reply[1]) & 0x80U) == 0);
        RUVIA_CHECK(sender.feed(reply) == WebSocketFeedStatus::kAccepted);
        const auto echo = sender.nextEvent();
        RUVIA_CHECK(echo && echo->message());
        if (!echo || !echo->message()) {
            return;
        }
        RUVIA_CHECK_EQ(echo->message()->payload(), payload);
    }
    RUVIA_CHECK_EQ(source.calls, 3U);
}

RUVIA_TEST(ws_public_client_fragmented_message_with_ping_and_close) {
    MaskSource source;
    auto connection = client(source);
    WebSocketConnection server;
    // A fragmented binary message with a control frame between fragments.
    const std::string wire(
        "\x02\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x80\x02"
        "cd",
        11);
    RUVIA_CHECK(connection.feed(wire) == WebSocketFeedStatus::kAccepted);
    const auto ping = connection.nextEvent();
    RUVIA_CHECK(ping && ping->ping());
    const auto pong = drain(connection);
    RUVIA_CHECK(server.feed(pong) == WebSocketFeedStatus::kAccepted);
    const auto pongEvent = server.nextEvent();
    RUVIA_CHECK(pongEvent && pongEvent->pong() && pongEvent->pong()->payload() == "p");
    const auto message = connection.nextEvent();
    RUVIA_CHECK(message && message->message() && message->message()->payload() == "abcd");
    RUVIA_CHECK(server.submitClose(1000, "done") == WebSocketCloseSubmitStatus::kAccepted);
    const auto close = drain(server);
    RUVIA_CHECK(connection.feed(close) == WebSocketFeedStatus::kAccepted);
    const auto closed = connection.nextEvent();
    RUVIA_CHECK(closed && closed->close() && closed->close()->closeCode() == 1000);
    RUVIA_CHECK(connection.outputPlan().disposition() == WebSocketTransportDisposition::kEndTransport);
    const auto response = drain(connection);
    RUVIA_CHECK(server.feed(response) == WebSocketFeedStatus::kAccepted);
    const auto acknowledged = server.nextEvent();
    RUVIA_CHECK(acknowledged && acknowledged->close());
    connection.commitTransportEnd();
    RUVIA_CHECK(connection.feed("x") == WebSocketFeedStatus::kInactive);
    RUVIA_CHECK_EQ(source.calls, 2U);
    RUVIA_CHECK(pong.substr(2, 4) != response.substr(2, 4));
}

RUVIA_TEST(ws_public_client_rejects_masked_server_and_oversized_message) {
    MaskSource source;
    auto invalidServer = client(source);
    RUVIA_CHECK(invalidServer.submitFrame(WebSocketOpcode::kText, "bad") == WebSocketFrameSubmitStatus::kAccepted);
    const auto masked = drain(invalidServer);
    auto receiver = client(source);
    RUVIA_CHECK(receiver.feed(masked) == WebSocketFeedStatus::kAccepted);
    const auto error = receiver.nextEvent();
    RUVIA_CHECK(error && error->protocolError() && error->protocolError()->closeCode() == 1002);
    auto limited = client(source, ProtocolByteLimit::limited(2));
    RUVIA_CHECK(limited.feed(std::string_view("\x82\x03"
                                              "abc",
                    5)) == WebSocketFeedStatus::kAccepted);
    const auto large = limited.nextEvent();
    RUVIA_CHECK(large && large->protocolError() && large->protocolError()->closeCode() == 1009);
    RUVIA_CHECK(limited.submitFrame(WebSocketOpcode::kBinary, "x") == WebSocketFrameSubmitStatus::kNotOpen);
}

RUVIA_TEST(ws_public_client_requires_entropy_and_handles_transport_end) {
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        WebSocketConnection connection({.role = WebSocketConnectionRole::kClient});
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        WebSocketConnection connection({.role = static_cast<WebSocketConnectionRole>(255)});
    }));
    MaskSource source;
    auto connection = client(source);
    source.fail = true;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)connection.submitFrame(WebSocketOpcode::kBinary, "x");
    }));
    RUVIA_CHECK(connection.outputPlan().bytes().empty());
    RUVIA_CHECK(connection.abort() == WebSocketAbortDisposition::kAbortTransport);
    RUVIA_CHECK(connection.abort() == WebSocketAbortDisposition::kNoTransportAction);
    auto eof = client(source);
    eof.notifyTransportEof();
    const auto event = eof.nextEvent();
    RUVIA_CHECK(event && event->transportEnd());
    eof.commitTransportEnd();
    RUVIA_CHECK(eof.livenessMode() == WebSocketLivenessMode::kInactive);
}

RUVIA_TEST(ws_public_connection_reuses_buffers_and_releases_owned_memory) {
    CountingResource resource;
    MaskSource source;
    {
        WebSocketConnection connection({.resource = &resource,
            .role = WebSocketConnectionRole::kClient,
            .maskKeyGenerator = &MaskSource::generate,
            .maskKeyContext = &source});
        WebSocketConnection server;
        const std::string payload(8192, 'x');
        RUVIA_CHECK(server.submitFrame(WebSocketOpcode::kBinary, payload) == WebSocketFrameSubmitStatus::kAccepted);
        const auto wire = drain(server);
        std::size_t warmedBytes = 0;
        for (unsigned i = 0; i < 100; ++i) {
            RUVIA_CHECK(connection.feed(wire) == WebSocketFeedStatus::kAccepted);
            const auto event = connection.nextEvent();
            RUVIA_CHECK(event && event->message());
            if (!event || !event->message()) {
                return;
            }
            RUVIA_CHECK(connection.submitFrame(WebSocketOpcode::kBinary, event->message()->payload()) == WebSocketFrameSubmitStatus::kAccepted);
            (void)drain(connection);
            RUVIA_CHECK(!connection.nextEvent());
            if (i == 2) {
                warmedBytes = resource.liveBytes;
            }
            if (i > 2) {
                RUVIA_CHECK_EQ(resource.liveBytes, warmedBytes);
            }
        }
        RUVIA_CHECK(resource.liveBytes > 0);
        (void)connection.abort();
    }
    RUVIA_CHECK_EQ(resource.liveBytes, std::size_t{0});
}

RUVIA_TEST(ws_public_connection_moves_keep_event_and_output_storage_stable) {
    CountingResource sourceMemory;
    CountingResource replacedMemory;
    MaskSource mask;
    const std::string payload(1024, 'x');
    {
        WebSocketConnection source({.resource = &sourceMemory,
            .role = WebSocketConnectionRole::kClient,
            .maskKeyGenerator = &MaskSource::generate,
            .maskKeyContext = &mask});
        WebSocketConnection server;
        RUVIA_CHECK(server.submitFrame(WebSocketOpcode::kBinary, payload) == WebSocketFrameSubmitStatus::kAccepted);
        const auto wire = drain(server);
        RUVIA_CHECK(source.feed(wire) == WebSocketFeedStatus::kAccepted);
        const auto event = source.nextEvent();
        RUVIA_CHECK(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        const auto retainedPayload = event->message()->payload();
        RUVIA_CHECK(source.submitFrame(WebSocketOpcode::kBinary, retainedPayload) == WebSocketFrameSubmitStatus::kAccepted);
        const auto retainedOutput = source.outputPlan().bytes();
        const auto liveBeforeMove = sourceMemory.liveBytes;
        const auto allocationsBeforeMove = sourceMemory.allocations;
        WebSocketConnection moved(std::move(source));
        WebSocketConnection replacement({.resource = &replacedMemory});
        RUVIA_CHECK(replacedMemory.liveBytes > 0);
        replacement = std::move(moved);
        RUVIA_CHECK_EQ(replacedMemory.liveBytes, std::size_t{0});
        RUVIA_CHECK_EQ(sourceMemory.liveBytes, liveBeforeMove);
        RUVIA_CHECK_EQ(sourceMemory.allocations, allocationsBeforeMove);
        RUVIA_CHECK_EQ(retainedPayload, payload);
        RUVIA_CHECK(replacement.outputPlan().bytes().data() == retainedOutput.data());
        RUVIA_CHECK_EQ(replacement.outputPlan().bytes(), retainedOutput);
        RUVIA_CHECK(server.feed(retainedOutput) == WebSocketFeedStatus::kAccepted);
        const auto echoed = server.nextEvent();
        RUVIA_CHECK(echoed && echoed->message() && echoed->message()->payload() == payload);
        // EOF/abort discard logical output without invalidating a transport's
        // outstanding write borrow. Only destruction releases the backing bytes.
        const std::string savedOutput(retainedOutput);
        replacement.notifyTransportEof();
        RUVIA_CHECK(replacement.abort() == WebSocketAbortDisposition::kAbortTransport);
        RUVIA_CHECK_EQ(retainedOutput, savedOutput);
        RUVIA_CHECK_EQ(sourceMemory.liveBytes, liveBeforeMove);
    }
    RUVIA_CHECK_EQ(sourceMemory.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(sourceMemory.allocations, sourceMemory.deallocations);
    RUVIA_CHECK_EQ(replacedMemory.allocations, replacedMemory.deallocations);
}

RUVIA_TEST(ws_public_driver_implementation_lifetime_and_construction_failures) {
    CountingResource memory;
    {
        WebSocketConnection unused({.resource = &memory});
        RUVIA_CHECK(memory.liveBytes > 0);
    }
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        WebSocketConnection invalid({.resource = &memory, .role = WebSocketConnectionRole::kClient});
    }));
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        WebSocketConnection invalid({.resource = &memory, .compression = WebSocketCompression{.enabled = true, .serverMaxWindowBits = 7}});
    }));
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    {
        std::pmr::string input(&memory);
        const auto inputBytes = memory.liveBytes;
        {
            WebSocketServerProtocol unused(input);
            RUVIA_CHECK(memory.liveBytes > inputBytes);
        }
        RUVIA_CHECK_EQ(memory.liveBytes, inputBytes);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            WebSocketServerProtocol invalid(input, ProtocolByteLimit::unlimited(),
                WebSocketServerProtocolOptions{.compression = WebSocketCompression{.enabled = true, .serverMaxWindowBits = 7}});
        }));
        RUVIA_CHECK_EQ(memory.liveBytes, inputBytes);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            WebSocketServerProtocol invalid(input, ProtocolByteLimit::unlimited(),
                WebSocketServerProtocolOptions{.compressionLevel = 10});
        }));
        RUVIA_CHECK_EQ(memory.liveBytes, inputBytes);
    }
    memory.reject = true;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        WebSocketConnection rejected({.resource = &memory});
    }));
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(ws_default_message_limit_rejects_oversized_declared_frame_before_payload) {
    // A masked binary frame declaring 16 MiB + 1 bytes requires no payload to
    // establish that it cannot fit the finite default message policy.
    const std::string header("\x82\xff\x00\x00\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00", 14);
    WebSocketConnection connection;
    RUVIA_CHECK(connection.feed(header) == WebSocketFeedStatus::kAccepted);
    const auto event = connection.nextEvent();
    RUVIA_CHECK(event && event->protocolError());
    RUVIA_CHECK_EQ(event->protocolError()->closeCode(), std::uint16_t{1009});
    std::pmr::string input(header);
    WebSocketServerProtocol protocol(input);
    const auto borrowed_event = protocol.poll();
    RUVIA_CHECK(borrowed_event && borrowed_event->protocolError());
    RUVIA_CHECK_EQ(borrowed_event->protocolError()->closeCode(), std::uint16_t{1009});
}

RUVIA_TEST(ws_owned_input_backpressure_does_not_copy_rejected_bytes) {
    CountingResource resource;
    WebSocketConnection connection({.resource = &resource, .max_buffered_input_bytes = 16});
    const auto baseline = resource.liveBytes;
    const std::string oversized(17, 'x');
    RUVIA_CHECK(connection.feed(oversized) == WebSocketFeedStatus::backpressured);
    RUVIA_CHECK_EQ(resource.liveBytes, baseline);
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    RUVIA_CHECK(connection.feed(ping) == WebSocketFeedStatus::kAccepted);
    const auto event = connection.nextEvent();
    RUVIA_CHECK(event && event->ping());
    RUVIA_CHECK_EQ(event->ping()->payload(), "p");
    (void)connection.consumeOutput(connection.outputPlan().bytes().size());
    RUVIA_CHECK(!connection.nextEvent());
    RUVIA_CHECK(connection.feed(ping) == WebSocketFeedStatus::kAccepted);
    RUVIA_CHECK(connection.nextEvent()->ping() != nullptr);
}
