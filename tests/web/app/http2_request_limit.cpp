// A connection whose max_requests_per_connection_ is reached starts its
// GOAWAY drain on the request head that reached the limit; that head is below
// the advertised last stream id, so it is served like any accepted stream even
// when it is a CONNECT tunnel or an RFC 8441 WebSocket handshake.
#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

#include "test_harness.h"

namespace http2_request_limit_test {
using namespace std::chrono_literals;

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

class limit_routes final {
public:
    RUVIA_ROUTES_BEGIN(limit_routes)
    RUVIA_CONNECT("target.test:443", tunnel);
    RUVIA_GET_WS("/socket", socket);
    RUVIA_ROUTES_END

    // Both long-lived handlers run until the client closes the connection.
    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        while (co_await tunnel_value.read()) {
        }
    }
    ruvia::task<void> socket(ruvia::context& context_value) {
        auto& socket_value = context_value.websocket();
        while (co_await socket_value.read()) {
        }
    }
};

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

struct frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;
};
frame read_frame(asio::ip::tcp::socket& socket) {
    std::array<char, ruvia::http2_frame_header_bytes> bytes{};
    asio::read(socket, asio::buffer(bytes));
    const auto header = ruvia::parse_http2_frame_header(bytes);
    if (!header || header->length_ > 64 * 1024) {
        throw std::runtime_error("invalid HTTP/2 response frame");
    }
    frame value{*header, std::string(header->length_, '\0')};
    asio::read(socket, asio::buffer(value.payload_));
    return value;
}
std::uint32_t uint32(std::string_view bytes) {
    if (bytes.size() < 4) {
        throw std::runtime_error("truncated HTTP/2 frame");
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

// The server's answer to stream 1: the decoded :status of its first HEADERS
// frame, or "RST_STREAM:<code>" when the stream was reset first. GOAWAY frames
// seen on the way record their last stream id.
struct stream_outcome final {
    std::string answer_;
    std::optional<std::uint32_t> goaway_last_stream_id_;
};
stream_outcome first_outcome(asio::ip::tcp::socket& socket, ruvia::hpack_decoder& decoder) {
    stream_outcome outcome;
    for (;;) {
        const auto value = read_frame(socket);
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway)) {
            outcome.goaway_last_stream_id_ = uint32(value.payload_) & 0x7fffffffU;
            continue;
        }
        if (value.header_.stream_id_ != 1) {
            continue;
        }
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
            outcome.answer_ = "RST_STREAM:" + std::to_string(uint32(value.payload_));
            return outcome;
        }
        if (value.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(value.payload_, [&](std::string_view name, std::string_view field) {
                if (name == ":status") {
                    outcome.answer_ = field;
                }
                return true;
            });
            if (decoded.has_value()) {
                throw std::runtime_error("invalid HPACK response block");
            }
            return outcome;
        }
    }
}

enum class upgrade_kind : std::uint8_t {
    connect_tunnel,
    websocket,
};
stream_outcome open_upgrade(const asio::ip::tcp::endpoint& endpoint, upgrade_kind kind) {
    asio::io_context io;
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
    if (kind == upgrade_kind::websocket) {
        ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", "/socket");
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    } else {
        ruvia::hpack_encoder::encode_header(block, ":authority", "target.test:443");
    }
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    asio::write(socket, asio::buffer(wire));
    auto outcome = first_outcome(socket, decoder);
    // Closing the transport ends the established tunnel or socket.
    socket.close();
    return outcome;
}
}  // namespace http2_request_limit_test

RUVIA_TEST(http2_upgrade_on_the_request_that_reaches_the_connection_limit_is_served) {
    using namespace http2_request_limit_test;
    using namespace std::chrono_literals;
    auto& app = ruvia::app();
    app.on_start([] { started().release(); });
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .max_requests_per_connection_ = 1});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    std::exception_ptr failure;
    std::thread thread([&app, &failure] {
        try {
            app.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    if (!started().try_acquire_for(3s)) {
        app.stop();
        thread.join();
        throw std::runtime_error("HTTP/2 request limit app did not start");
    }
    std::optional<stream_outcome> tunnel;
    std::optional<stream_outcome> websocket;
    std::exception_ptr client_failure;
    try {
        tunnel = open_upgrade(endpoint, upgrade_kind::connect_tunnel);
        websocket = open_upgrade(endpoint, upgrade_kind::websocket);
    } catch (...) {
        client_failure = std::current_exception();
    }
    app.stop();
    thread.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
    // The limit was reached by this very request: GOAWAY advertises it as the
    // last accepted stream, and the upgrade still completes on it.
    RUVIA_CHECK_EQ(tunnel->answer_, std::string("200"));
    RUVIA_CHECK(tunnel->goaway_last_stream_id_.has_value());
    RUVIA_CHECK_EQ(tunnel->goaway_last_stream_id_.value_or(0), 1U);
    RUVIA_CHECK_EQ(websocket->answer_, std::string("200"));
    RUVIA_CHECK(websocket->goaway_last_stream_id_.has_value());
    RUVIA_CHECK_EQ(websocket->goaway_last_stream_id_.value_or(0), 1U);
}
