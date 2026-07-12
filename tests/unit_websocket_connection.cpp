#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSocketTransport.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using asio::ip::tcp;
using ruvia::WebSocketOpcode;
using ruvia::detail::ConnectionScanner;
using ruvia::detail::SocketWebSocketConnection;
using ruvia::detail::WebSocketDeflate;
using ruvia::detail::WebSocketDeflateNegotiation;
using ruvia::detail::WebSocketConnection;
using ruvia::detail::WebSocketSocketTransport;
using ruvia::detail::WsTransportDisposition;

struct RecordingTransportState final {
    bool aborted{false};
    std::size_t writes{0};
    WsTransportDisposition lastDisposition{WsTransportDisposition::kKeepOpen};
};

class RecordingTransport final {
public:
    RecordingTransport(asio::io_context& io, RecordingTransportState& state) noexcept
        : io_(&io), state_(&state) {}

    [[nodiscard]] auto executor() const noexcept {
        return io_->get_executor();
    }

    [[nodiscard]] ruvia::Task<bool> readMore(std::pmr::string&) {
        co_return false;
    }

    [[nodiscard]] ruvia::Task<std::error_code> writeBytes(
        std::string_view,
        WsTransportDisposition disposition) {
        ++state_->writes;
        state_->lastDisposition = disposition;
        co_return std::error_code{};
    }

    void abort() noexcept {
        state_->aborted = true;
    }

private:
    asio::io_context* io_;
    RecordingTransportState* state_;
};

static_assert(!std::constructible_from<
    WebSocketConnection<RecordingTransport>,
    RecordingTransport,
    ConnectionScanner::Entry&,
    ruvia::WebSocketLifecycleOptions,
    std::size_t,
    std::pmr::memory_resource*,
    std::string_view,
    bool>);

std::string maskedFrame(
    std::uint8_t opcode,
    std::string_view payload,
    bool fin = true,
    bool rsv1 = false) {
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

// Periodic liveness failure aborts the WebSocket transport itself and returns false
// to ConnectionScanner. For an RFC 8441 adapter that means RST_STREAM(CANCEL), so one
// silent tunnel cannot tear down unrelated streams on the multiplexed connection.
RUVIA_TEST(websocket_liveness_aborts_transport_not_scanner_owner) {
    asio::io_context io;
    RecordingTransportState state;
    ConnectionScanner::Entry scannerEntry;
    ruvia::WorkerMemory memory;
    ruvia::WebSocketLifecycleOptions lifecycle;
    lifecycle.pingInterval = std::chrono::milliseconds(1);
    lifecycle.pongTimeout = std::chrono::milliseconds(1);
    WebSocketConnection<RecordingTransport> connection(
        RecordingTransport(io, state),
        scannerEntry,
        lifecycle,
        1024,
        memory.resource());

    RUVIA_CHECK(!WebSocketConnection<RecordingTransport>::heartbeatTickThunk(&connection, 10));
    io.run();
    RUVIA_CHECK_EQ(state.writes, std::size_t{1});
    RUVIA_CHECK(state.lastDisposition == WsTransportDisposition::kKeepOpen);

    // No Pong arrived and its deadline elapsed. The callback still tells the
    // scanner not to close the owning socket because abort() already targeted this
    // transport.
    RUVIA_CHECK(!WebSocketConnection<RecordingTransport>::heartbeatTickThunk(&connection, 12));
    RUVIA_CHECK(state.aborted);
}

// HTTP/1 upgraded-byte-stream bridge: Ping is answered by the protocol core,
// fragmented Text is reassembled, the application echo is serialized by the
// same core, and the normal Close is emitted on the socket transport.
RUVIA_TEST(websocket_socket_bridge_ping_fragment_echo_and_close) {
    asio::io_context io;
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
            SocketWebSocketConnection<tcp::socket> connection(
                WebSocketSocketTransport<tcp::socket>(socket),
                scannerEntry,
                {},
                1024,
                memory.resource());
            const auto message = co_await ruvia::detail::taskAsAwaitable(connection.read());
            serverSawMessage = message.has_value() && message->payload() == "hello";
            if (message) {
                co_await ruvia::detail::taskAsAwaitable(
                    connection.write(message->opcode(), message->payload()));
            }
            co_await ruvia::detail::taskAsAwaitable(connection.close(1000, {}));
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
            gotEcho = echo.size() == 7 && static_cast<unsigned char>(echo[0]) == 0x81 &&
                echo.substr(2) == "hello";
            const auto close = co_await readShortServerFrame(socket);
            gotClose = close.size() >= 4 && static_cast<unsigned char>(close[0]) == 0x88 &&
                static_cast<unsigned char>(close[2]) == 0x03 &&
                static_cast<unsigned char>(close[3]) == 0xE8;
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
    asio::io_context io;
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
            SocketWebSocketConnection<tcp::socket> connection(
                WebSocketSocketTransport<tcp::socket>(socket),
                scannerEntry,
                {},
                1024,
                memory.resource(),
                {},
                WebSocketDeflateNegotiation::kAccepted);
            const auto message = co_await ruvia::detail::taskAsAwaitable(connection.read());
            serverDecoded = message.has_value() && message->payload() == original;
            if (message) {
                co_await ruvia::detail::taskAsAwaitable(
                    connection.write(WebSocketOpcode::kText, message->payload()));
            }
            co_await ruvia::detail::taskAsAwaitable(connection.close(1000, {}));
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
            const auto request = maskedFrame(
                0x1,
                std::string_view(compressed.data(), compressed.size()),
                true,
                true);
            co_await asio::async_write(socket, asio::buffer(request), asio::use_awaitable);

            const auto response = co_await readShortServerFrame(socket);
            if (response.size() < 2 || (static_cast<unsigned char>(response[0]) & 0x40U) == 0) {
                co_return;
            }
            WebSocketDeflate decoder;
            std::pmr::string decoded(std::pmr::get_default_resource());
            const auto result = decoder.decompress(
                std::string_view(response.data() + 2, response.size() - 2),
                decoded,
                1024);
            clientDecoded = result == ruvia::detail::WebSocketInflateResult::kOk &&
                std::string_view(decoded.data(), decoded.size()) == original;
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
    asio::io_context io;
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
            SocketWebSocketConnection<tcp::socket> connection(
                WebSocketSocketTransport<tcp::socket>(socket),
                scannerEntry,
                {},
                1024,
                memory.resource());
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
                closeCode = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(static_cast<unsigned char>(close[2])) << 8) |
                    static_cast<unsigned char>(close[3]));
            }
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(serverEnded);
    RUVIA_CHECK_EQ(closeCode, static_cast<std::uint16_t>(1002));
}
