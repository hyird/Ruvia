#include "test_io_context.h"
#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using asio::ip::tcp;
using ruvia::WebSocketCompression;
using ruvia::WebSocketOpcode;
using ruvia::detail::ConnectionScanner;
using ruvia::detail::SocketWebSocketConnection;
using ruvia::detail::WebSocketConnection;
using ruvia::detail::WebSocketDeflate;
using ruvia::detail::WebSocketSocketTransport;
using ruvia::detail::WsTransportDisposition;

ruvia::WorkerHandle testWorker(asio::io_context& io) {
    return ruvia::detail::WorkerHandleAccess::make(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64));
}

struct RecordingTransportState final {
    bool aborted{false};
    std::size_t writes{0};
    std::error_code readError;
    std::string lastNonEmptyBytes;
    WsTransportDisposition lastDisposition{WsTransportDisposition::kKeepOpen};
    bool suspendNextWrite{false};
    std::function<void()> completeWrite;
    void* beforeWriteCompletionTarget{nullptr};
    void (*beforeWriteCompletion)(void*) noexcept {nullptr};
};

class RecordingTransport final {
public:
    RecordingTransport(asio::io_context& io, RecordingTransportState& state) noexcept
        : io_(&io),
          state_(&state) {}

    [[nodiscard]] auto executor() const noexcept {
        return io_->get_executor();
    }

    [[nodiscard]] ruvia::Task<ruvia::detail::WsTransportReadResult> readMore(std::pmr::string&) {
        if (state_->readError) {
            co_return ruvia::detail::WsTransportReadResult::makeFailure(state_->readError);
        }
        co_return ruvia::detail::WsTransportReadResult::makeEnd();
    }

    [[nodiscard]] ruvia::Task<std::error_code> writeBytes(std::string_view bytes, WsTransportDisposition disposition) {
        ++state_->writes;
        if (!bytes.empty()) {
            state_->lastNonEmptyBytes.assign(bytes);
        }
        state_->lastDisposition = disposition;
        if (state_->beforeWriteCompletion != nullptr) {
            const auto callback = std::exchange(state_->beforeWriteCompletion, nullptr);
            callback(state_->beforeWriteCompletionTarget);
        }
        if (std::exchange(state_->suspendNextWrite, false)) {
            static_cast<void>(co_await ruvia::detail::asyncAsio<void>([state = state_](auto completion) mutable { state->completeWrite = [completion = std::move(completion)]() mutable { completion(std::error_code{}); }; }));
        }
        co_return std::error_code{};
    }

    void abort() noexcept {
        state_->aborted = true;
        if (state_->completeWrite != nullptr) {
            auto completion = std::exchange(state_->completeWrite, std::function<void()>{});
            completion();
        }
    }

private:
    asio::io_context* io_;
    RecordingTransportState* state_;
};

static_assert(!std::constructible_from<WebSocketConnection<RecordingTransport>, RecordingTransport, ConnectionScanner::Entry&, ruvia::WebSocketLifecycleOptions, std::size_t, std::pmr::memory_resource*, std::string_view, bool>);

std::string maskedFrame(std::uint8_t opcode, std::string_view payload, bool fin = true, bool rsv1 = false) {
    std::string frame;
    frame.reserve(payload.size() + 6);
    frame.push_back(static_cast<char>((fin ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode));
    frame.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
    constexpr std::array<unsigned char, 4> mask{0x11, 0x22, 0x33, 0x44};
    frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]));
    }
    return frame;
}

asio::awaitable<std::string> readShortServerFrame(tcp::socket& socket) {
    std::array<char, 2> head{};
    co_await asio::async_read(socket, asio::buffer(head), asio::use_awaitable);
    const auto size = static_cast<std::size_t>(static_cast<unsigned char>(head[1]) & 0x7FU);
    if (size >= 126) {
        co_return std::string{};
    }
    std::string frame(head.data(), head.size());
    frame.resize(2 + size);
    if (size != 0) {
        co_await asio::async_read(socket, asio::buffer(frame.data() + 2, size), asio::use_awaitable);
    }
    co_return frame;
}

}  // namespace

RUVIA_TEST(websocket_transport_read_failure_preserves_error_and_aborts) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    state.readError = std::make_error_code(std::errc::connection_reset);
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());
    std::error_code observed;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await ruvia::detail::taskAsAwaitable(connection.read());
            } catch (const std::system_error& error) {
                observed = error.code();
            }
        },
        asio::detached);
    io.run();

    RUVIA_CHECK_EQ(observed, state.readError);
    RUVIA_CHECK(state.aborted);
}

RUVIA_TEST(websocket_read_reservation_rejects_cold_overlap_and_releases) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    {
        auto cold = connection.read();
        bool rejected = false;
        try {
            auto overlapping = connection.read();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);

        bool closeRejected = false;
        try {
            auto closing = connection.close();
            static_cast<void>(closing);
        } catch (const std::logic_error&) {
            closeRejected = true;
        }
        RUVIA_CHECK(closeRejected);
    }

    auto following = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.read()), asio::use_future);
    io.run();
    const auto message = following.get();
    RUVIA_CHECK(!message.has_value());
    RUVIA_CHECK(!state.aborted);
}

RUVIA_TEST(websocket_session_finish_maps_chain_failure_to_1011) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    // The 1011 close code is all the peer learns; the listener is where the
    // reason survives an already-upgraded connection.
    struct FailureObservation final {
        std::size_t calls{0};
        std::string message;

        void operator()(const ruvia::ConnectionFailureRecord& record) noexcept {
            ++calls;
            try {
                std::rethrow_exception(record.exception());
            } catch (const std::exception& error) {
                message.assign(error.what());
            } catch (...) {
                message.assign("<unknown>");
            }
        }
    } observation;
    ruvia::detail::ConnectionFailureSink connectionFailure;
    connectionFailure.callback = ruvia::detail::CallbackAccess::bind<void(const ruvia::ConnectionFailureRecord&) noexcept>(observation);

    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(ruvia::detail::finishWebSocketSession(connection, std::make_exception_ptr(std::runtime_error("middleware post failed")), connectionFailure, "127.0.0.1")), asio::use_future);
    io.run();
    future.get();

    RUVIA_CHECK_EQ(observation.calls, std::size_t{1});
    RUVIA_CHECK_EQ(observation.message, std::string("middleware post failed"));
    RUVIA_CHECK_EQ(state.writes, std::size_t{2});
    RUVIA_CHECK(state.lastNonEmptyBytes.size() >= 4);
    if (state.lastNonEmptyBytes.size() >= 4) {
        const auto high = static_cast<unsigned char>(state.lastNonEmptyBytes[2]);
        const auto low = static_cast<unsigned char>(state.lastNonEmptyBytes[3]);
        RUVIA_CHECK_EQ(static_cast<std::uint16_t>((high << 8U) | low), std::uint16_t{1011});
    }
    RUVIA_CHECK(state.lastDisposition == WsTransportDisposition::kEndTransport);
    RUVIA_CHECK(!state.aborted);
}

// Periodic liveness failure aborts the WebSocket transport itself. The scanner
// callback has no connection-close return channel; for an RFC 8441 adapter abort
// means RST_STREAM(CANCEL), so one silent tunnel cannot tear down unrelated streams.
RUVIA_TEST(websocket_liveness_aborts_transport_not_scanner_owner) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    ruvia::WebSocketLifecycleOptions lifecycle;
    lifecycle.heartbeat = {
        .pingInterval = std::chrono::milliseconds(1),
        .pongTimeout = std::chrono::milliseconds(1),
    };
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, lifecycle, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    asio::post(io, [&connection] { WebSocketConnection<RecordingTransport>::heartbeatTickThunk(&connection, 10); });
    io.run();
    RUVIA_CHECK_EQ(state.writes, std::size_t{1});
    RUVIA_CHECK(state.lastDisposition == WsTransportDisposition::kKeepOpen);

    // No Pong arrived and its deadline elapsed. The callback can only abort its
    // own transport; it cannot ask Core to close the scanner's owning socket.
    io.restart();
    asio::post(io, [&connection] { WebSocketConnection<RecordingTransport>::heartbeatTickThunk(&connection, 12); });
    io.run();
    RUVIA_CHECK(state.aborted);
}

RUVIA_TEST(websocket_close_timeout_starts_after_close_write_commits) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    ruvia::WebSocketLifecycleOptions lifecycle;
    lifecycle.closeHandshakeTimeout = std::chrono::milliseconds(1);
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, lifecycle, ruvia::ProtocolByteLimit::limited(1024), memory.resource());
    state.beforeWriteCompletionTarget = &connection;
    state.beforeWriteCompletion = [](void* target) noexcept {
        auto* runtime = static_cast<WebSocketConnection<RecordingTransport>*>(target);
        // Even an arbitrarily large scanner time cannot expire a peer-response
        // window before the local Close write has completed.
        WebSocketConnection<RecordingTransport>::heartbeatTickThunk(runtime, 10000);
    };

    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.close()), asio::use_future);
    io.run();
    future.get();

    RUVIA_CHECK(!state.aborted);
    RUVIA_CHECK(state.beforeWriteCompletion == nullptr);
}

RUVIA_TEST(websocket_runtime_maps_typed_outbound_rejections) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(4), memory.resource());
    bool messageRejected = false;
    bool invalidTextRejected = false;
    bool closeRejected = false;
    const std::string invalidText("\xc0\x80", 2);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, "12345"));
            } catch (const std::invalid_argument&) {
                messageRejected = true;
            }
            try {
                co_await ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, invalidText));
            } catch (const std::invalid_argument&) {
                invalidTextRejected = true;
            }
            try {
                co_await ruvia::detail::taskAsAwaitable(connection.close({.code = 1005}));
            } catch (const std::invalid_argument&) {
                closeRejected = true;
            }
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(messageRejected);
    RUVIA_CHECK(invalidTextRejected);
    RUVIA_CHECK(closeRejected);
    RUVIA_CHECK_EQ(state.writes, std::size_t{0});
    RUVIA_CHECK(!state.aborted);
}

RUVIA_TEST(websocket_write_guard_rejects_overlap_and_releases_after_suspend) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    state.suspendNextWrite = true;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    auto first = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, "first")), asio::use_future);
    io.poll();
    RUVIA_CHECK(state.completeWrite != nullptr);

    io.restart();
    bool rejected = false;
    try {
        auto overlapping = connection.write(WebSocketOpcode::kText, "overlap");
        static_cast<void>(overlapping);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    auto completeWrite = std::move(state.completeWrite);
    asio::post(io, std::move(completeWrite));
    io.restart();
    io.run();
    first.get();

    io.restart();
    auto following = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, "following")), asio::use_future);
    io.run();
    following.get();
    RUVIA_CHECK_EQ(state.writes, std::size_t{2});
}

RUVIA_TEST(websocket_close_guard_rejects_write_until_close_flush_commits) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    state.suspendNextWrite = true;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    auto closing = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.close()), asio::use_future);
    io.poll();
    RUVIA_CHECK(state.completeWrite != nullptr);

    io.restart();
    bool rejected = false;
    try {
        auto overlapping = connection.write(WebSocketOpcode::kText, "late");
        static_cast<void>(overlapping);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    auto completeWrite = std::move(state.completeWrite);
    asio::post(io, std::move(completeWrite));
    io.restart();
    io.run();
    closing.get();
    RUVIA_CHECK_EQ(state.writes, std::size_t{2});
    RUVIA_CHECK(state.lastDisposition == WsTransportDisposition::kEndTransport);
}

RUVIA_TEST(websocket_teardown_aborts_and_joins_suspended_application_write) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    RecordingTransportState state;
    state.suspendNextWrite = true;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    WebSocketConnection<RecordingTransport> connection(RecordingTransport(io, state), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());

    auto writing = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, "in flight")), asio::use_future);
    io.poll();
    RUVIA_CHECK(state.completeWrite != nullptr);

    io.restart();
    auto teardown = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(connection.detachAndDrainWrites()), asio::use_future);
    io.run();

    writing.get();
    teardown.get();
    RUVIA_CHECK(state.aborted);
    RUVIA_CHECK(state.completeWrite == nullptr);
}

// HTTP/1 upgraded-byte-stream bridge: Ping is answered by the protocol core,
// fragmented Text is reassembled, the application echo is serialized by the
// same core, and the normal Close is emitted on the socket transport.
RUVIA_TEST(websocket_socket_bridge_ping_fragment_echo_and_close) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    bool serverSawMessage = false;
    bool gotPong = false;
    bool gotEcho = false;
    bool gotClose = false;
    bool serverCloseCompleted = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory memory;
            ConnectionScanner::Entry scannerEntry;
            SocketWebSocketConnection<tcp::socket> connection(WebSocketSocketTransport<tcp::socket>(socket), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());
            const auto message = co_await ruvia::detail::taskAsAwaitable(connection.read());
            serverSawMessage = message.has_value() && message->payload() == "hello";
            if (message) {
                co_await ruvia::detail::taskAsAwaitable(connection.write(message->opcode(), message->payload()));
            }
            co_await ruvia::detail::taskAsAwaitable(connection.close());
            serverCloseCompleted = true;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            std::string input = maskedFrame(0x9, "p");
            input += maskedFrame(0x1, "hel", false);
            input += maskedFrame(0x0, "lo", true);
            co_await asio::async_write(socket, asio::buffer(input), asio::use_awaitable);

            const auto pong = co_await readShortServerFrame(socket);
            gotPong = pong.size() == 3 && static_cast<unsigned char>(pong[0]) == 0x8A && pong[2] == 'p';
            const auto echo = co_await readShortServerFrame(socket);
            gotEcho = echo.size() == 7 && static_cast<unsigned char>(echo[0]) == 0x81 && echo.substr(2) == "hello";
            const auto close = co_await readShortServerFrame(socket);
            gotClose = close.size() >= 4 && static_cast<unsigned char>(close[0]) == 0x88 && static_cast<unsigned char>(close[2]) == 0x03 && static_cast<unsigned char>(close[3]) == 0xE8;
            if (gotClose) {
                const auto reply = maskedFrame(0x8, std::string_view("\x03\xE8", 2));
                co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            }
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(serverSawMessage);
    RUVIA_CHECK(gotPong);
    RUVIA_CHECK(gotEcho);
    RUVIA_CHECK(gotClose);
    RUVIA_CHECK(serverCloseCompleted);
}

// Negotiated permessage-deflate crosses the actual socket bridge in both
// directions: core decode on read, core encode on application write.
RUVIA_TEST(websocket_socket_bridge_permessage_deflate_round_trip) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    const std::string original(200, 'a');
    bool serverDecoded = false;
    bool clientDecoded = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory memory;
            ConnectionScanner::Entry scannerEntry;
            SocketWebSocketConnection<tcp::socket> connection(WebSocketSocketTransport<tcp::socket>(socket), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource(), {}, WebSocketCompression::kPermessageDeflate);
            const auto message = co_await ruvia::detail::taskAsAwaitable(connection.read());
            serverDecoded = message.has_value() && message->payload() == original;
            if (message) {
                co_await ruvia::detail::taskAsAwaitable(connection.write(WebSocketOpcode::kText, message->payload()));
            }
            co_await ruvia::detail::taskAsAwaitable(connection.close());
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            WebSocketDeflate encoder;
            std::pmr::string compressed(std::pmr::get_default_resource());
            if (!encoder.compress(original, compressed)) {
                co_return;
            }
            const auto request = maskedFrame(0x1, std::string_view(compressed.data(), compressed.size()), true, true);
            co_await asio::async_write(socket, asio::buffer(request), asio::use_awaitable);

            const auto response = co_await readShortServerFrame(socket);
            if (response.size() < 2 || (static_cast<unsigned char>(response[0]) & 0x40U) == 0) {
                co_return;
            }
            WebSocketDeflate decoder;
            std::pmr::string decoded(std::pmr::get_default_resource());
            const auto result = decoder.decompress(std::string_view(response.data() + 2, response.size() - 2), decoded, ruvia::ProtocolByteLimit::limited(1024));
            clientDecoded = result == ruvia::detail::WebSocketInflateResult::kOk && std::string_view(decoded.data(), decoded.size()) == original;
            (void)(co_await readShortServerFrame(socket));
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(serverDecoded);
    RUVIA_CHECK(clientDecoded);
}

// An unmasked client frame is rejected by the core and the HTTP/1 socket bridge
// flushes the generated 1002 Close instead of rebuilding it in the web layer.
RUVIA_TEST(websocket_socket_bridge_protocol_error_flushes_core_close) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    const auto workerHandle = testWorker(io);
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    bool serverEnded = false;
    std::uint16_t closeCode = 0;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory memory;
            ConnectionScanner::Entry scannerEntry;
            SocketWebSocketConnection<tcp::socket> connection(WebSocketSocketTransport<tcp::socket>(socket), workerHandle, scannerEntry, {}, ruvia::ProtocolByteLimit::limited(1024), memory.resource());
            const auto message = co_await ruvia::detail::taskAsAwaitable(connection.read());
            serverEnded = !message.has_value();
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            const std::string invalid{"\x81\x02hi", 4};
            co_await asio::async_write(socket, asio::buffer(invalid), asio::use_awaitable);
            const auto close = co_await readShortServerFrame(socket);
            if (close.size() >= 4 && static_cast<unsigned char>(close[0]) == 0x88) {
                closeCode = static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(close[2])) << 8) | static_cast<unsigned char>(close[3]));
            }
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(serverEnded);
    RUVIA_CHECK_EQ(closeCode, static_cast<std::uint16_t>(1002));
}
