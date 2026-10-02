#include <array>
#include <exception>
#include <optional>
#include <string>

#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/HttpCapsuleStream.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/detail/http/HttpTunnelSession.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct CapsuleTransportState {
    std::string input;
    std::string output;
    std::size_t offset{};
    std::size_t block{};
    unsigned fins{};
    bool aborted{};
};
struct CapsuleTransport {
    CapsuleTransportState& state;
    asio::io_context& io;
    ruvia::Task<ruvia::detail::HttpStreamReadResult> readMore(std::pmr::string& bytes) {
        co_await ruvia::asyncAsio<void>([&](auto done) {
            asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
        });
        if (state.aborted) {
            co_return ruvia::detail::HttpStreamReadResult::makeFailure(std::make_error_code(std::errc::operation_canceled));
        }
        if (state.offset == state.input.size()) {
            co_return ruvia::detail::HttpStreamReadResult::makeEnd();
        }
        const auto count = std::min(state.block, state.input.size() - state.offset);
        bytes.append(state.input.data() + state.offset, count);
        state.offset += count;
        co_return ruvia::detail::HttpStreamReadResult::makeData();
    }
    ruvia::Task<std::error_code> writeBytes(std::string_view bytes, ruvia::detail::HttpStreamEnd end) {
        if (state.aborted) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        state.output.append(bytes);
        state.fins += end == ruvia::detail::HttpStreamEnd::kEnd;
        co_return std::error_code{};
    }
    void abort() noexcept {
        state.aborted = true;
    }
};
std::string capsuleWire(std::uint64_t type, std::string_view payload) {
    std::array<char, 16> header;
    const auto encoded = ruvia::encodeHttpCapsuleHeader(header, type, payload.size());
    std::string bytes(header.data(), *encoded);
    bytes.append(payload);
    return bytes;
}
}  // namespace
RUVIA_TEST(http_capsule_stream_owns_cold_inputs_retained_results_and_split_or_coalesced_capsules) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        try {
            for (std::size_t block : {std::size_t{1}, std::size_t{65536}}) {
                const std::string payload(20003, 'c');
                CapsuleTransportState wire{.input = capsuleWire(0x123456789ULL, payload) + capsuleWire(0, "") + capsuleWire(7, "last"), .block = block};
                ruvia::detail::HttpTunnelSession session(CapsuleTransport{wire, io}, worker, memory);
                std::optional<ruvia::HttpCapsule> retained;
                {
                    auto capsules = session.tunnel().capsules();
                    {
                        auto cold = capsules.write(10, "discarded");
                    }
                    std::string input(10003, 'w');
                    auto output = capsules.write(11, input);
                    input.assign("mutated");
                    bool busy{};
                    try {
                        (void)capsules.finish();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    auto first = capsules.read();
                    auto moved = std::move(capsules);
                    co_await std::move(output);
                    retained = co_await std::move(first);
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == payload);
                    auto empty = co_await moved.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    auto last = co_await moved.read();
                    RUVIA_CHECK(last && last->type() == 7 && last->payload() == "last");
                    RUVIA_CHECK(!(co_await moved.read()));
                    co_await moved.finish();
                    co_await moved.finish();
                    RUVIA_CHECK(wire.output == capsuleWire(11, std::string(10003, 'w')));
                    RUVIA_CHECK_EQ(wire.fins, 1U);
                }
                RUVIA_CHECK(retained->payload() == payload);
                RUVIA_CHECK(!wire.aborted);
                retained.reset();
                co_await session.join();
            }
            for (bool oversized : {false, true}) {
                auto bytes = capsuleWire(0, oversized ? "123456789" : "1234");
                if (!oversized) {
                    bytes.pop_back();
                }
                CapsuleTransportState wire{.input = bytes, .block = 1};
                ruvia::detail::HttpTunnelSession session(CapsuleTransport{wire, io}, worker, memory);
                auto capsules = session.tunnel().capsules({.maxCapsuleLength = 8});
                bool rejected{};
                try {
                    (void)co_await capsules.read();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                RUVIA_CHECK(rejected && wire.aborted);
                co_await session.join();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(http_udp_tunnel_ignores_unknown_capsules_and_contexts_preserves_empty_packets_and_owned_data) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        try {
            const std::string payload(65527, 'u');
            CapsuleTransportState wire{.input = capsuleWire(987, "unknown capsule") + capsuleWire(0, std::string(1, char(9)) + "unknown context") + capsuleWire(0, std::string(1, char(0)) + payload) + capsuleWire(0, std::string(1, char(0))), .block = 65536};
            for (unsigned i = 0; i != 20000; ++i) {
                wire.input.insert(0, capsuleWire(63, ""));
            }
            ruvia::detail::HttpTunnelSession session(CapsuleTransport{wire, io}, worker, memory);
            std::optional<ruvia::HttpUdpDatagram> retained;
            {
                ruvia::HttpUdpTunnel udp(session.tunnel().capsules());
                retained = co_await udp.read();
                RUVIA_CHECK(retained && std::string_view(reinterpret_cast<const char*>(retained->payload().data()), retained->payload().size()) == payload);
                auto empty = co_await udp.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await udp.read()));
                std::string bytes = payload;
                auto write = udp.send(bytes);
                bytes.assign("mutated");
                auto moved = std::move(udp);
                co_await std::move(write);
                co_await moved.send("");
                bool rejected{};
                try {
                    (void)moved.send(std::string(65528, 'x'));
                } catch (const std::length_error&) {
                    rejected = true;
                }
                RUVIA_CHECK(rejected);
                co_await moved.finish();
                RUVIA_CHECK(wire.output == capsuleWire(0, std::string(1, char(0)) + payload) + capsuleWire(0, std::string(1, char(0))));
            }
            RUVIA_CHECK(retained->payload().size() == payload.size());
            retained.reset();
            co_await session.join();
            CapsuleTransportState malformed{.input = capsuleWire(0, std::string(1, char(0x40))), .block = 4096};
            ruvia::detail::HttpTunnelSession failedSession(CapsuleTransport{malformed, io}, worker, memory);
            ruvia::HttpUdpTunnel udp(failedSession.tunnel().capsules());
            bool rejected{};
            try {
                (void)co_await udp.read();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected && malformed.aborted);
            co_await failedSession.join();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}

namespace {
struct MixedDatagramState final {
    std::array<std::string, 3> input;
    std::string streamOutput;
    std::string nativeOutput;
    std::size_t next{};
    bool aborted{};
};
struct MixedDatagramTransport final {
    MixedDatagramState& state;
    asio::io_context& io;
    std::pmr::memory_resource* resource;
    ruvia::Task<std::optional<ruvia::detail::HttpDatagramInput>> readDatagramInput() {
        co_await ruvia::asyncAsio<void>([&](auto done) { asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); }); });
        if (state.aborted) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (state.next == state.input.size()) {
            co_return std::nullopt;
        }
        const auto next = state.next++;
        co_return ruvia::detail::HttpDatagramInput{std::pmr::string(state.input[next], resource), next == 1};
    }
    ruvia::HttpDatagramSessionConfig datagramConfig() const {
        return {.http3StreamId = 4, .localH3Datagram = true, .peerH3Datagram = true, .quicDatagram = true, .maxQuicPayloadBytes = 16};
    }
    void sendDatagram(std::span<const std::byte> bytes) {
        state.nativeOutput.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    ruvia::Task<ruvia::detail::HttpStreamReadResult> readMore(std::pmr::string&) {
        co_return ruvia::detail::HttpStreamReadResult::makeEnd();
    }
    ruvia::Task<std::error_code> writeBytes(std::string_view bytes, ruvia::detail::HttpStreamEnd) {
        state.streamOutput.append(bytes);
        co_return std::error_code{};
    }
    void abort() noexcept {
        state.aborted = true;
    }
};
}  // namespace
RUVIA_TEST(http_datagram_stream_preserves_partial_capsules_when_native_packets_arrive_and_selects_send_transport) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource memory;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        try {
            auto wire = capsuleWire(0, "abcde");
            MixedDatagramState mixed{.input = {wire.substr(0, 4), std::string(1, '\1') + "xyz", wire.substr(4) + capsuleWire(17, "ignored") + capsuleWire(0, "")}};
            ruvia::detail::HttpTunnelSession session(MixedDatagramTransport{mixed, io, &memory}, worker, memory);
            std::optional<ruvia::HttpDatagram> retained;
            {
                auto stream = session.tunnel().datagrams();
                retained = co_await stream.read();
                RUVIA_CHECK(retained && retained->transport() == ruvia::HttpDatagramTransport::kQuic);
                RUVIA_CHECK(retained && retained->payload().size() == 3 && retained->payload()[0] == std::byte{'x'});
                auto reliable = co_await stream.read();
                RUVIA_CHECK(reliable && reliable->transport() == ruvia::HttpDatagramTransport::kCapsule);
                RUVIA_CHECK(reliable && reliable->payload().size() == 5 && reliable->payload()[4] == std::byte{'e'});
                auto empty = co_await stream.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await stream.read()));
                std::string value = "hello";
                auto cold = stream.send(value);
                value.assign("changed");
                co_await std::move(cold);
                RUVIA_CHECK(mixed.nativeOutput == std::string(1, '\1') + "hello");
                co_await stream.send(std::string(20, 'p'));
                RUVIA_CHECK(mixed.streamOutput == capsuleWire(0, std::string(20, 'p')));
                co_await stream.finish();
                RUVIA_CHECK(ruvia::testing::throwsOn([&] { static_cast<void>(stream.send("closed")); }));
            }
            RUVIA_CHECK(retained && retained->payload().size() == 3);
            retained.reset();
            co_await session.join();
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
    RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
