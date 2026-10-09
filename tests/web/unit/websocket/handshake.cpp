#include <string_view>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/http_websocket_handshake.h"

namespace {

using ruvia::http1_server_request_parser;
using ruvia::http_request;

class failing_handshake_write_stream final {
public:
    using executor_type = asio::io_context::executor_type;

    explicit failing_handshake_write_stream(asio::io_context& io) noexcept
        : executor_(io.get_executor()) {}

    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }

    template <typename const_buffer_sequence_type, typename handler_type>
    void async_write_some(const const_buffer_sequence_type&, handler_type&& handler) {
        asio::post(executor_, [handler = std::forward<handler_type>(handler)]() mutable {
            std::move(handler)(std::make_error_code(std::errc::broken_pipe), std::size_t{0});
        });
    }

private:
    executor_type executor_;
};

http_request parse_request(std::string_view raw_request) {
    http1_server_request_parser parser;
    auto parsed_value = parser.parse_message(raw_request);
    return std::move(parsed_value.request_);
}

std::string_view valid_handshake() {
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
    const auto request = parse_request(valid_handshake());
    const auto handshake = ruvia::make_websocket_server_handshake(request, {});
    asio::io_context& io = ruvia::test::new_test_io_context();
    failing_handshake_write_stream stream(io);
    auto result_value = asio::co_spawn(io,
        ruvia::as_awaitable(ruvia::detail::write_websocket_handshake(stream, handshake)),
        asio::use_future);
    io.run();
    RUVIA_CHECK_EQ(result_value.get(), std::make_error_code(std::errc::broken_pipe));
}
