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
    std::atomic<bool> sse_wait_cancelled_{};
    std::atomic<bool> sse_wait_failed_{};
    std::atomic<bool> sse_context_stopped_{};
    std::atomic<bool> sse_wait_finished_{};
};

const auto sleeping_stream_observed = std::make_shared<sleeping_stream_observation>();
[[maybe_unused]] const bool sleeping_stream_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<sleeping_stream_observation>>([] { return sleeping_stream_observed; });
    return true;
}();

class sleeping_stream_routes final {
    RUVIA_ROUTES_BEGIN(sleeping_stream_routes)
    RUVIA_GET("/http2-sleeping-stream", sleeping);
    RUVIA_POST("/http2-count-upload", count_upload, ruvia::stream_body);
    RUVIA_GET("/http2-quiet-sse", quiet_sse);
    RUVIA_POST("/http2-post-sse", post_sse);
    RUVIA_ROUTES_END

    // Streams the request body and answers with the byte count.
    // A buffered POST body is readable before the SSE response starts.
    ruvia::task<void> post_sse(ruvia::context& context_value) {
        const auto received = co_await context_value.req().text();
        auto writer = context_value.stream_sse();
        co_await writer.write({.data_ = received});
    }

    ruvia::task<ruvia::http_response> count_upload(ruvia::context& context_value) {
        std::size_t bytes_value = 0;
        auto& reader_value = context_value.req().body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }
        std::pmr::string text(context_value.arena());
        text.append(std::to_string(bytes_value));
        co_return context_value.text(std::move(text));
    }

    ruvia::task<void> quiet_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        const auto context_stop = context_value.stop_token();
        const auto stop = writer.cancel_on_peer_eof();
        co_await writer.write({.data_ = "ready"});
        if (context_value.req().query("writer").has_value()) {
            try {
                observed.sleep_elapsed_ =
                    co_await writer.sleep(std::chrono::seconds(30)) == ruvia::timer_sleep_result::elapsed;
            } catch (const std::system_error&) {
                observed.sleep_failed_ = true;
                throw;
            }
            co_return;
        }
        try {
            observed.sse_wait_cancelled_ =
                co_await ruvia::sleep_for(context_value.worker(), std::chrono::seconds(30), stop) ==
                ruvia::timer_sleep_result::stop_requested;
        } catch (...) {
            observed.sse_wait_failed_ = true;
            throw;
        }
        observed.sse_context_stopped_ = context_stop.stop_requested();
        observed.sse_wait_finished_ = true;
    }

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

// With expect_continue the HEADERS announce the content but the DATA frame is
// left for the caller to send once the server has answered the expectation.
std::string request_wire(std::pmr::memory_resource* resource, std::string_view method,
    std::string_view path, std::string_view body, bool expect_continue = false) {
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
    if (expect_continue) {
        ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    }
    append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(body.empty() ? 0x5 : 0x4), 1, block);
    if (!body.empty() && !expect_continue) {
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
    }
    return wire;
}

struct received_frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;
};

received_frame read_frame(asio::ip::tcp::socket& client) {
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    asio::read(client, asio::buffer(header_bytes));
    const auto header_value = ruvia::parse_http2_frame_header(header_bytes);
    if (!header_value.has_value()) {
        throw std::logic_error("invalid HTTP/2 frame header");
    }
    received_frame frame{*header_value, std::string(header_value->length_, '\0')};
    if (!frame.payload_.empty()) {
        asio::read(client, asio::buffer(frame.payload_));
    }
    return frame;
}

// The quiet-stream regression must fail promptly even if no response arrives.
// Its socket is nonblocking; one deadline bounds the entire frame sequence.
received_frame read_frame(asio::ip::tcp::socket& client, std::chrono::steady_clock::time_point deadline) {
    const auto read_exact = [&](char* bytes, std::size_t size) {
        while (size != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("HTTP/2 quiet SSE response timed out");
            }
            asio::error_code error;
            const auto count = client.read_some(asio::buffer(bytes, size), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                throw std::system_error(error);
            }
            bytes += count;
            size -= count;
        }
    };
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    read_exact(header_bytes.data(), header_bytes.size());
    const auto header_value = ruvia::parse_http2_frame_header(header_bytes);
    if (!header_value || header_value->length_ > 64 * 1024) {
        throw std::logic_error("invalid HTTP/2 quiet SSE frame");
    }
    received_frame frame{*header_value, std::string(header_value->length_, '\0')};
    read_exact(frame.payload_.data(), frame.payload_.size());
    return frame;
}

// Reads frames until the first HEADERS frame on stream 1 and returns the
// decoded :status; connection-level frames before it are skipped.
std::string read_first_status(asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
    for (;;) {
        const auto frame = read_frame(client);
        if (frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
            frame.header_.stream_id_ != 1) {
            continue;
        }
        std::string status;
        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
            if (name == ":status") {
                status = value;
            }
            return true;
        });
        if (decoded.has_value()) {
            throw std::logic_error("invalid HPACK block");
        }
        return status;
    }
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
using http2_streaming_test::read_first_status;
using http2_streaming_test::request_wire;
using http2_streaming_test::sleeping_stream_observed;

// A client that disconnects while an HTTP/2 stream producer is inside sleep()
// ends that sleep with the transport error, so the handler, its stream and
// the connection retire instead of staying suspended for the full duration;
// the disconnect is not counted as a connection failure.
RUVIA_TEST(http2_response_stream_sleep_ends_when_the_peer_disconnects) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto failures_before = ruvia::app().http_stats().connection_failures_;
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
    // The peer going away is a normal end of the connection, not a fault.
    RUVIA_CHECK_EQ(ruvia::app().http_stats().connection_failures_, failures_before);
}

RUVIA_TEST(http2_post_sse_reads_the_request_body_before_streaming) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::pmr::unsynchronized_pool_resource resource;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire(&resource, "POST", "/http2-post-sse", "{\"id\":\"1\"}");
    asio::write(client, asio::buffer(wire));
    RUVIA_CHECK_EQ(read_first_data(client), std::string("data: {\"id\":\"1\"}\n\n"));
}

// A quiet SSE producer waits on the public opt-in token, not on a transport
// write or writer.sleep(). RST_STREAM must wake it without closing its sibling.
RUVIA_TEST(http2_quiet_sse_peer_reset_cancels_wait_and_preserves_connection) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sse_wait_cancelled_ = false;
    sleeping_stream_observed->sse_wait_failed_ = false;
    sleeping_stream_observed->sse_context_stopped_ = false;
    sleeping_stream_observed->sse_wait_finished_ = false;
    const auto stats_before = ruvia::app().http_stats();
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire(&resource, "GET", "/http2-quiet-sse", {});
    asio::write(client, asio::buffer(wire));
    client.non_blocking(true);
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string event;
    while (event.size() < std::string_view("data: ready\n\n").size()) {
        const auto frame = http2_streaming_test::read_frame(client, ready_deadline);
        RUVIA_CHECK(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        RUVIA_CHECK(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
            RUVIA_CHECK(!decoded.has_value());
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            event.append(frame.payload_);
        }
    }
    RUVIA_CHECK_EQ(event, std::string("data: ready\n\n"));
    RUVIA_CHECK(!sleeping_stream_observed->sse_wait_finished_);

    client.non_blocking(false);
    // Keep a sibling upload open across the reset, rather than merely opening
    // a new stream after cancellation has already finished.
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-count-upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    std::string sibling_request;
    append_frame(sibling_request, ruvia::http2_frame_type::headers, 0x4, 3, block);
    asio::write(client, asio::buffer(sibling_request));
    std::string reset;
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sse_wait_finished_ && std::chrono::steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(sleeping_stream_observed->sse_wait_finished_);
    RUVIA_CHECK(sleeping_stream_observed->sse_wait_cancelled_);
    RUVIA_CHECK(!sleeping_stream_observed->sse_wait_failed_);
    RUVIA_CHECK(!sleeping_stream_observed->sse_context_stopped_);

    std::string sibling_body;
    append_frame(sibling_body, ruvia::http2_frame_type::data, 0x1, 3, "x");
    asio::write(client, asio::buffer(sibling_body));
    client.non_blocking(true);
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status;
    std::string body;
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, response_deadline);
        RUVIA_CHECK(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        RUVIA_CHECK(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (frame.header_.stream_id_ == 3 && name == ":status") {
                    status = value;
                }
                return true;
            });
            RUVIA_CHECK(!decoded.has_value());
        }
        if (frame.header_.stream_id_ == 3) {
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
                frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
    }
    RUVIA_CHECK_EQ(status, std::string("200"));
    RUVIA_CHECK_EQ(body, std::string("1"));
    client.close();
    server.finish();
    const auto stats_after = ruvia::app().http_stats();
    RUVIA_CHECK_EQ(stats_after.connection_failures_, stats_before.connection_failures_);
    RUVIA_CHECK_EQ(stats_after.worker_failures_, stats_before.worker_failures_);
}

RUVIA_TEST(http2_opt_in_writer_sleep_preserves_reset_error) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto before = ruvia::app().http_stats().connection_failures_;
    std::pmr::unsynchronized_pool_resource resource;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire(&resource, "GET", "/http2-quiet-sse?writer=1", {})));
    RUVIA_CHECK_EQ(read_first_data(client), std::string("data: ready\n\n"));
    std::string reset;
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(sleeping_stream_observed->sleep_failed_);
    RUVIA_CHECK(!sleeping_stream_observed->sleep_elapsed_);
    client.close();
    server.finish();
    RUVIA_CHECK_EQ(ruvia::app().http_stats().connection_failures_, before);
}

// A request whose HEADERS and complete DATA arrive in one read is still a
// streaming upload: the handler is dispatched for the head and consumes the
// already-received content rather than the stream being reset.
RUVIA_TEST(http2_streaming_upload_received_in_one_read_is_dispatched) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::size_t body_size : {std::size_t{1}, std::size_t{100}, std::size_t{5000}}) {
        std::pmr::unsynchronized_pool_resource resource;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire(&resource, "POST", "/http2-count-upload", std::string(body_size, 'x'));
        asio::write(client, asio::buffer(wire));
        RUVIA_CHECK_EQ(read_first_data(client), std::to_string(body_size));
        client.close();
    }
    server.finish();
}

// RFC 9110 §10.1.1 on HTTP/2 follows the HTTP/1 policy: 100 Continue is sent
// when the server starts consuming the content (a streaming handler's first
// body read), and a request whose final status is known from the head alone
// (404) is answered with that status and never asked for its content.
RUVIA_TEST(http2_expect_continue_is_answered_when_the_content_is_consumed) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    {
        std::pmr::unsynchronized_pool_resource resource;
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string body(40, 'x');
        asio::write(client, asio::buffer(request_wire(&resource, "POST", "/http2-count-upload", body, true)));
        RUVIA_CHECK_EQ(read_first_status(client, decoder), std::string("100"));
        std::string data;
        append_frame(data, ruvia::http2_frame_type::data, 0x1, 1, body);
        asio::write(client, asio::buffer(data));
        RUVIA_CHECK_EQ(read_first_status(client, decoder), std::string("200"));
        RUVIA_CHECK_EQ(read_first_data(client), std::string("40"));
        client.close();
    }
    {
        std::pmr::unsynchronized_pool_resource resource;
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire(&resource, "POST", "/http2-missing-upload", std::string(40, 'x'), true)));
        RUVIA_CHECK_EQ(read_first_status(client, decoder), std::string("404"));
        client.close();
    }
    server.finish();
}
