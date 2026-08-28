#include <array>
#include <atomic>
#include <bit>
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
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/web/WebSocketClient.h"

namespace {

class WebSocketOrigin final {
public:
    WebSocketOrigin()
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { serve(); }) {}

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
            if (colon != std::string_view::npos &&
                ruvia::detail::httpAsciiEqualsIgnoreCase(line.substr(0, colon), name)) {
                return std::string(ruvia::detail::httpTrimOws(line.substr(colon + 1)));
            }
            cursor = end + 2;
        }
        return {};
    }

    static std::string readClientFrame(asio::ip::tcp::socket& socket, std::uint8_t expectedOpcode) {
        std::array<unsigned char, 2> head{};
        asio::read(socket, asio::buffer(head));
        if ((head[0] & 0x0FU) != expectedOpcode || (head[1] & 0x80U) == 0 ||
            (head[1] & 0x7FU) > 125) {
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
            payload[i] =
                static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % mask.size()]);
        }
        return payload;
    }

    static void writeServerFrame(
        asio::ip::tcp::socket& socket, std::uint8_t opcode, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80U | opcode));
        frame.push_back(static_cast<char>(payload.size()));
        frame.append(payload);
        asio::write(socket, asio::buffer(frame));
    }

    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            std::string request(
                asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data()));
            const auto key = header(request, "Sec-WebSocket-Key");
            if (key.empty() ||
                !ruvia::detail::httpHasToken(header(request, "Upgrade"), "websocket")) {
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

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic_bool succeeded_{false};
};

ruvia::Task<int> exercise(ruvia::WebSocketClient& client, ruvia::WorkerId expectedWorker) {
    co_await client.connect();
    if (!client.connected() || !client.worker().isCurrent() ||
        client.worker().id() != expectedWorker || client.subprotocol() != "chat") {
        co_return 1;
    }
    co_await client.text("hello");
    const auto message = co_await client.read();
    if (!message.has_value() || !message->text() || message->payload() != "world") {
        co_return 2;
    }
    co_await client.close({.code = 1000});
    co_return 0;
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
        ruvia::WebSocketClient plainClientWithInactiveTlsConfig(loop, invalidConfig);

        WebSocketOrigin origin;
        ruvia::WebSocketClient client(loop, {
                                                .scheme = ruvia::WebSocketScheme::kWs,
                                                .host = "127.0.0.1",
                                                .port = origin.port(),
                                                .target = "/events",
                                                .subprotocols = "chat, superchat",
                                            });
        loops.start();
        std::promise<int> completion;
        auto future = completion.get_future();
        const auto posted = loop.post([&] {
            ruvia::detail::asyncStartTask(exercise(client, loop.id()),
                asio::bind_executor(loop.executor(),
                    [&completion](ruvia::detail::TaskCompletionResult<int> result) {
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
