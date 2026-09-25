#include <string_view>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/WebSocketHandshake.h"
#include "ruvia/web/detail/websocket/HttpWebSocketHandshake.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::Http1ServerRequestParser;
using ruvia::HttpRequest;

class FailingHandshakeWriteStream final {
public:
    using executor_type = asio::io_context::executor_type;

    explicit FailingHandshakeWriteStream(asio::io_context& io) noexcept
        : executor_(io.get_executor()) {}

    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }

    template <typename ConstBufferSequence, typename Handler>
    void async_write_some(const ConstBufferSequence&, Handler&& handler) {
        asio::post(executor_, [handler = std::forward<Handler>(handler)]() mutable {
            std::move(handler)(std::make_error_code(std::errc::broken_pipe), std::size_t{0});
        });
    }

private:
    executor_type executor_;
};

HttpRequest parseRequest(std::string_view rawRequest) {
    Http1ServerRequestParser parser;
    auto parsed = parser.parseMessage(rawRequest);
    return std::move(parsed.request);
}

std::string_view validHandshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

}  // namespace

RUVIA_TEST(ws_handshake_writer_preserves_transport_error) {
    const auto request = parseRequest(validHandshake());
    const auto handshake = ruvia::makeWebSocketServerHandshake(request, {});
    asio::io_context& io = ruvia::test::newTestIoContext();
    FailingHandshakeWriteStream stream(io);
    auto result = asio::co_spawn(io,
        ruvia::asAwaitable(ruvia::detail::writeWebSocketHandshake(stream, handshake)),
        asio::use_future);
    io.run();
    RUVIA_CHECK_EQ(result.get(), std::make_error_code(std::errc::broken_pipe));
}
