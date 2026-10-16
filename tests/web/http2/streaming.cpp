#include "ruvia/web/streaming.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace http2_streaming_test {

struct sleeping_stream_observation final {
    std::atomic<bool> sleep_failed_{};
    std::atomic<bool> sleep_elapsed_{};
};

const auto sleeping_stream_observed = std::make_shared<sleeping_stream_observation>();
[[maybe_unused]] const bool sleeping_stream_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<sleeping_stream_observation>>([] { return sleeping_stream_observed; });
    return true;
}();

class sleeping_stream_routes final {
    RUVIA_ROUTES_BEGIN(sleeping_stream_routes)
    RUVIA_GET_STREAM("/http2-sleeping-stream", sleeping);
    RUVIA_ROUTES_END

    // Writes one chunk, then pauses far longer than the test waits: only a
    // peer disconnect can end the sleep in time.
    ruvia::task<void> sleeping(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& writer = context_value.stream_text();
        co_await writer.write("first\n");
        try {
            if (co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed) {
                observed.sleep_elapsed_ = true;
            }
        } catch (const std::system_error&) {
            observed.sleep_failed_ = true;
            throw;
        }
        co_await writer.write("second\n");
    }
};

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header_value{};
    if (!ruvia::encode_http2_frame_header(
            header_value, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header_value.data(), header_value.size());
    wire.append(payload);
}

std::string request_wire(std::pmr::memory_resource* resource, std::string_view method,
    std::string_view path, std::string_view body) {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    if (!body.empty()) {
        ruvia::hpack_encoder::encode_header(block, "content-length", std::to_string(body.size()));
    }
    append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(body.empty() ? 0x5 : 0x4), 1, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
    }
    return wire;
}

// Reads frames until the first DATA frame on stream 1 and returns its payload;
// a GOAWAY or RST_STREAM before that ends the read with an empty result.
std::string read_first_data(asio::ip::tcp::socket& client) {
    for (;;) {
        std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
        asio::read(client, asio::buffer(header_bytes));
        const auto header_value = ruvia::parse_http2_frame_header(header_bytes);
        if (!header_value.has_value()) {
            return {};
        }
        std::string payload(header_value->length_, '\0');
        if (!payload.empty()) {
            asio::read(client, asio::buffer(payload));
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            header_value->stream_id_ == 1) {
            return payload;
        }
        if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
            header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
            return {};
        }
    }
}

}  // namespace http2_streaming_test

using http2_streaming_test::append_frame;
using http2_streaming_test::read_first_data;
using http2_streaming_test::request_wire;
using http2_streaming_test::sleeping_stream_observed;

// A client that disconnects while an HTTP/2 stream producer is inside sleep()
// ends that sleep with the transport error, so the handler, its stream and
// the connection retire instead of staying suspended for the full duration.
RUVIA_TEST(http2_response_stream_sleep_ends_when_the_peer_disconnects) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    for (const bool send_goaway : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire(&resource, "GET", "/http2-sleeping-stream", {});
        asio::write(client, asio::buffer(wire));
        RUVIA_CHECK_EQ(read_first_data(client), std::string("first\n"));
        if (send_goaway) {
            std::string goaway;
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
            asio::write(client, asio::buffer(goaway));
        }
        client.close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(sleeping_stream_observed->sleep_failed_);
        RUVIA_CHECK(!sleeping_stream_observed->sleep_elapsed_);
        sleeping_stream_observed->sleep_failed_ = false;
    }
    server.finish();
}
