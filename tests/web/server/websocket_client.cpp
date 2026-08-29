#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <future>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <asio/bind_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/web/WebSocketClient.h"

namespace {

class WebSocketOrigin final {
public:
    explicit WebSocketOrigin(bool heartbeat = false, bool respondPong = true)
        : heartbeat_(heartbeat),
          respondPong_(respondPong),
          io_(),
          acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { serve(); }) {}

    WebSocketOrigin(const WebSocketOrigin&) = delete;
    WebSocketOrigin& operator=(const WebSocketOrigin&) = delete;

    ~WebSocketOrigin() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    [[nodiscard]] bool succeeded() const noexcept {
        return succeeded_.load(std::memory_order_acquire);
    }

private:
    static std::string header(std::string_view request, std::string_view name) {
        std::size_t cursor = request.find("\r\n") + 2;
        while (cursor < request.size()) {
            const auto end = request.find("\r\n", cursor);
            if (end == cursor || end == std::string_view::npos) {
                break;
            }
            const auto line = request.substr(cursor, end - cursor);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos && ruvia::detail::httpAsciiEqualsIgnoreCase(line.substr(0, colon), name)) {
                return std::string(ruvia::detail::httpTrimOws(line.substr(colon + 1)));
            }
            cursor = end + 2;
        }
        return {};
    }

    static std::string readClientFrame(asio::ip::tcp::socket& socket, std::uint8_t expectedOpcode) {
        std::array<unsigned char, 2> head{};
        asio::read(socket, asio::buffer(head));
        if ((head[0] & 0x0FU) != expectedOpcode || (head[1] & 0x80U) == 0 || (head[1] & 0x7FU) > 125) {
            return {};
        }
        const auto size = static_cast<std::size_t>(head[1] & 0x7FU);
        std::array<unsigned char, 4> mask{};
        asio::read(socket, asio::buffer(mask));
        std::string payload(size, '\0');
        if (size != 0) {
            asio::read(socket, asio::buffer(payload));
        }
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]);
        }
        return payload;
    }

    static void writeServerFrame(asio::ip::tcp::socket& socket, std::uint8_t opcode, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80U | opcode));
        frame.push_back(static_cast<char>(payload.size()));
        frame.append(payload);
        asio::write(socket, asio::buffer(frame));
    }

    static bool readClientControlFrame(asio::ip::tcp::socket& socket, std::uint8_t expectedOpcode) {
        std::array<unsigned char, 2> head{};
        asio::read(socket, asio::buffer(head));
        if ((head[0] & 0x8FU) != (0x80U | expectedOpcode) || (head[1] & 0x80U) == 0 || (head[1] & 0x7FU) > 125) {
            return false;
        }
        const auto size = static_cast<std::size_t>(head[1] & 0x7FU);
        std::array<unsigned char, 4> mask{};
        asio::read(socket, asio::buffer(mask));
        std::string payload(size, '\0');
        if (size != 0) {
            asio::read(socket, asio::buffer(payload));
        }
        return true;
    }

    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            std::string request(asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
            const auto key = header(request, "Sec-WebSocket-Key");
            if (key.empty() || !ruvia::detail::httpHasToken(header(request, "Upgrade"), "websocket") || header(request, "Sec-WebSocket-Protocol") != "chat, superchat") {
                return;
            }
            ruvia::detail::WebSocketAcceptKey accept{};
            ruvia::detail::encodeWebSocketAccept(accept, key);
            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                "Upgrade\r\nSec-WebSocket-Accept: ";
            response.append(accept.data(), accept.size());
            response += "\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
            asio::write(socket, asio::buffer(response));

            if (heartbeat_) {
                const bool receivedPing = readClientControlFrame(socket, 0x9);
                if (receivedPing && respondPong_) {
                    writeServerFrame(socket, 0xA, {});
                    writeServerFrame(socket, 0x1, "heartbeat");
                }
                if (!respondPong_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{250});
                }
                succeeded_.store(receivedPing, std::memory_order_release);
                return;
            }

            const bool receivedText = readClientFrame(socket, 0x1) == "hello";
            writeServerFrame(socket, 0x1, "world");
            const auto close = readClientFrame(socket, 0x8);
            if (close.size() >= 2) {
                writeServerFrame(socket, 0x8, close);
            }
            succeeded_.store(receivedText, std::memory_order_release);
        } catch (...) {
            succeeded_.store(false, std::memory_order_release);
        }
    }

    bool heartbeat_;
    bool respondPong_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic_bool succeeded_{false};
};

ruvia::Task<int> exercise(ruvia::WebSocketClient& client, ruvia::WorkerId expectedWorker) {
    co_await client.connect();
    if (!client.connected() || !client.worker().isCurrent() || client.worker().id() != expectedWorker || client.subprotocol() != "chat") {
        co_return 1;
    }
    {
        auto discardedRead = client.read();
        bool competingReadRejected = false;
        try {
            auto competingRead = client.read();
            static_cast<void>(competingRead);
        } catch (const ruvia::WebSocketClientError& error) {
            competingReadRejected = error.code() == ruvia::WebSocketClientError::Code::kInvalidState;
        }
        if (!competingReadRejected) {
            co_return 2;
        }
    }
    {
        auto discardedWrite = client.text("discarded");
        bool competingWriteRejected = false;
        try {
            auto competingWrite = client.text("also-discarded");
            static_cast<void>(competingWrite);
        } catch (const ruvia::WebSocketClientError& error) {
            competingWriteRejected = error.code() == ruvia::WebSocketClientError::Code::kInvalidState;
        }
        if (!competingWriteRejected) {
            co_return 3;
        }
    }
    {
        auto discardedClose = client.close({.code = 1000});
        bool competingCloseRejected = false;
        try {
            auto competingClose = client.close({.code = 1000});
            static_cast<void>(competingClose);
        } catch (const ruvia::WebSocketClientError& error) {
            competingCloseRejected = error.code() == ruvia::WebSocketClientError::Code::kInvalidState;
        }
        if (!competingCloseRejected) {
            co_return 4;
        }
    }
    co_await client.text("hello");
    const auto message = co_await client.read();
    if (!message.has_value() || !message->text() || message->payload() != "world") {
        co_return 5;
    }
    co_await client.close({.code = 1000});
    co_await client.shutdown();
    co_return 0;
}

ruvia::Task<int> exerciseHeartbeat(ruvia::WebSocketClient& client, bool expectTimeout) {
    co_await client.connect();
    int result = 0;
    try {
        const auto message = co_await client.read();
        client.abort();
        result = expectTimeout || !message.has_value() || !message->text() || message->payload() != "heartbeat" ? 1 : 0;
    } catch (const ruvia::WebSocketClientError& error) {
        client.abort();
        result = expectTimeout&& error.code() == ruvia::WebSocketClientError::Code::kTimeout ? 0 : 2;
    }
    co_await client.shutdown();
    co_return result;
}

ruvia::Task<void> consumeUntilShutdown(ruvia::WebSocketClient& client, ruvia::detail::WorkerSignal& started) {
    auto read = client.read();
    started.notify();
    try {
        static_cast<void>(co_await std::move(read));
    } catch (...) {
    }
}

ruvia::Task<int> exerciseShutdownWhileReading(ruvia::EventLoop loop, ruvia::WebSocketClient& client) {
    co_await client.connect();
    ruvia::detail::WorkerSignal readStarted(client.worker());
    ruvia::detail::asyncStartTask(consumeUntilShutdown(client, readStarted), asio::bind_executor(loop.executor(), [](ruvia::detail::TaskCompletionResult<void> result) {
        if (result.failure()) {
            std::terminate();
        }
    }));
    co_await readStarted.wait();
    co_await client.shutdown();
    co_return client.connected() ? 1 : 0;
}

ruvia::Task<int> exerciseAll(ruvia::EventLoop loop, ruvia::WebSocketClient& client, ruvia::WebSocketClient& heartbeatClient, ruvia::WebSocketClient& timeoutClient, ruvia::WebSocketClient& shutdownClient, ruvia::WebSocketClient& freshClient, ruvia::WorkerId expectedWorker) {
    co_await freshClient.shutdown();
    const auto shutdownResult = co_await exerciseShutdownWhileReading(loop, shutdownClient);
    if (shutdownResult != 0) {
        co_return shutdownResult;
    }
    const auto heartbeatResult = co_await exerciseHeartbeat(heartbeatClient, false);
    if (heartbeatResult != 0) {
        co_return heartbeatResult;
    }
    const auto timeoutResult = co_await exerciseHeartbeat(timeoutClient, true);
    if (timeoutResult != 0) {
        co_return 10 + timeoutResult;
    }
    co_return co_await exercise(client, expectedWorker);
}

bool rejectsClientConfig(const ruvia::EventLoop& loop, const ruvia::WebSocketClientConfig& config) {
    try {
        ruvia::WebSocketClient client(loop, config);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

int main() {
    try {
        ruvia::EventLoopPool loops({.loopCount = 1});
        auto loop = loops.loop(0);
        ruvia::WebSocketClientConfig invalidConfig{
            .scheme = ruvia::WebSocketScheme::kWs,
            .host = "127.0.0.1",
            .port = 80,
        };
        invalidConfig.scheme = std::bit_cast<ruvia::WebSocketScheme>(std::uint8_t{255});
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 1;
        }
        invalidConfig.scheme = ruvia::WebSocketScheme::kWs;
        invalidConfig.port = 0;
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 2;
        }
        invalidConfig.port = 80;
        invalidConfig.tcpNoDelay = static_cast<ruvia::TcpNoDelayPolicy>(255);
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 3;
        }
        invalidConfig.tcpNoDelay = ruvia::TcpNoDelayPolicy::kEnable;
        invalidConfig.certificateChainFile = "client.pem";
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 4;
        }

        invalidConfig.privateKeyFile = "client-key.pem";
        invalidConfig.headers.emplace_back("X-Test", "safe\r\ninjected: true");
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 5;
        }
        invalidConfig.headers.clear();
        invalidConfig.target = "/events#fragment";
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 6;
        }
        invalidConfig.target = "/";
        invalidConfig.subprotocols = {"chat", "chat"};
        if (!rejectsClientConfig(loop, invalidConfig)) {
            return 7;
        }
        invalidConfig.subprotocols.clear();
        ruvia::WebSocketClient plainClientWithInactiveTlsConfig(loop, invalidConfig);

        WebSocketOrigin heartbeatOrigin(true, true);
        ruvia::WebSocketClient heartbeatClient(loop, {
                                                         .scheme = ruvia::WebSocketScheme::kWs,
                                                         .host = "127.0.0.1",
                                                         .port = heartbeatOrigin.port(),
                                                         .target = "/events",
                                                         .subprotocols = {"chat", "superchat"},
                                                         .heartbeat = {.pingInterval = std::chrono::milliseconds{20}, .pongTimeout = std::chrono::milliseconds{100}},
                                                     });
        WebSocketOrigin timeoutOrigin(true, false);
        ruvia::WebSocketClient timeoutClient(loop, {
                                                       .scheme = ruvia::WebSocketScheme::kWs,
                                                       .host = "127.0.0.1",
                                                       .port = timeoutOrigin.port(),
                                                       .target = "/events",
                                                       .subprotocols = {"chat", "superchat"},
                                                       .heartbeat = {.pingInterval = std::chrono::milliseconds{20}, .pongTimeout = std::chrono::milliseconds{50}},
                                                   });
        WebSocketOrigin shutdownOrigin;
        ruvia::WebSocketClient shutdownClient(loop, {
                                                        .scheme = ruvia::WebSocketScheme::kWs,
                                                        .host = "127.0.0.1",
                                                        .port = shutdownOrigin.port(),
                                                        .target = "/events",
                                                        .subprotocols = {"chat", "superchat"},
                                                    });
        WebSocketOrigin origin;
        ruvia::WebSocketClient client(loop, {
                                                .scheme = ruvia::WebSocketScheme::kWs,
                                                .host = "127.0.0.1",
                                                .port = origin.port(),
                                                .target = "/events",
                                                .subprotocols = {"chat", "superchat"},
                                            });
        loops.start();
        std::promise<int> completion;
        auto future = completion.get_future();
        const auto posted = loop.post([&] {
            ruvia::detail::asyncStartTask(exerciseAll(loop, client, heartbeatClient, timeoutClient, shutdownClient, plainClientWithInactiveTlsConfig, loop.id()), asio::bind_executor(loop.executor(), [&completion](ruvia::detail::TaskCompletionResult<int> result) {
                if (auto* success = result.success()) {
                    completion.set_value(std::move(*success).takeValue());
                } else {
                    completion.set_exception(result.failure()->exception());
                }
            }));
        });
        if (!posted.accepted()) {
            return 2;
        }
        const auto result = future.get();
        loops.stop();
        loops.join();
        if (result != 0) {
            return 10 + result;
        }
        return origin.succeeded() ? 0 : 20;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "WebSocket client test failed: %s\n", error.what());
        return 100;
    }
}
