#include "ruvia/web/streaming.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/model.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

namespace http2_streaming_test {

struct sleeping_stream_observation final {
    std::atomic<bool> sleep_failed_{};
    std::atomic<bool> sleep_elapsed_{};
    std::atomic<bool> sse_wait_cancelled_{};
    std::atomic<bool> sse_wait_failed_{};
    std::atomic<bool> sse_wait_finished_{};
    std::atomic<int> head_loop_writes_{};
    std::atomic<bool> head_loop_aborted_{};
    std::atomic<bool> head_loop_sleep_rejected_{};
    std::atomic<bool> head_loop_token_stopped_{};
    std::atomic<bool> head_loop_finished_{};
    std::atomic<bool> naive_loop_failed_{};
    std::atomic<bool> naive_loop_finished_{};
    std::atomic<bool> head_trailer_stopped_{};
    std::atomic<bool> head_trailer_sleep_stopped_{};
    std::atomic<bool> head_trailer_aborted_{};
    std::atomic<bool> head_trailer_done_{};
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
    RUVIA_GET("/http2-paced-sse", paced_sse);
    RUVIA_GET("/http2-head-loop-sse", head_loop_sse);
    RUVIA_GET("/http2-head-naive-sse", head_naive_sse);
    RUVIA_POST("/http2-bounded-buffered", bounded_buffered, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-stream", count_upload, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_POST("/http2-bounded-delayed-stream", bounded_delayed_stream, ruvia::stream_body, ruvia::body_limit<16>);
    RUVIA_GET("/http2-head-trailer-write", head_trailer_write);
    RUVIA_GET("/http2-head-trailer-only", head_trailer_only);
    RUVIA_GET("/http2-stream-custom", stream_custom);
    RUVIA_GET("/http2-stream-204", stream_204);
    RUVIA_ROUTES_END

    // HEAD 正常完成，不启动生产循环，也不会取消业务等待 token。
    ruvia::task<void> head_loop_sse(ruvia::context& context_value) {
        if (context_value.req().query("not-modified").has_value()) {
            context_value.draft().status(ruvia::http_status::not_modified);
        }
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        const auto token = context_value.stop_token();
        if (co_await writer.commit() == ruvia::stream_commit_result::body) {
            co_await writer.write({.data_ = "tick"});
            observed.head_loop_writes_ = 1;
        } else {
            observed.head_loop_writes_ = -1;
            try {
                co_await writer.write({.data_ = "late"});
            } catch (const std::logic_error&) {
                observed.head_loop_writes_ = 0;
            }
            try {
                static_cast<void>(co_await writer.sleep(std::chrono::seconds(30)));
            } catch (const std::logic_error&) {
                observed.head_loop_sleep_rejected_ = true;
            }
        }
        observed.head_loop_aborted_ = writer.aborted();
        observed.head_loop_token_stopped_ = token.stop_requested();
        co_await writer.end();
        observed.head_loop_finished_ = true;
    }

    // 提交结果结束无正文响应，不靠轮询取消或等待 peer 断开。
    ruvia::task<void> head_naive_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            observed.naive_loop_finished_ = true;
            co_return;
        }
        try {
            for (int i = 0; i < 1000; ++i) {
                co_await writer.write({.data_ = "tick"});
                static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)));
            }
        } catch (...) {
            observed.naive_loop_failed_ = true;
            observed.naive_loop_finished_ = true;
            throw;
        }
        observed.naive_loop_finished_ = true;
    }

    ruvia::task<void> head_trailer_write(ruvia::context& context_value) {
        auto& out = context_value.stream_text();
        context_value.draft().header("Trailer", "X-Complete");
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("body");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        co_await out.end(trailers);
    }

    ruvia::task<void> head_trailer_only(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& out = context_value.stream_text();
        const auto token = context_value.stop_token();
        context_value.draft().header("Trailer", "X-Complete");
        const ruvia::http_header_view trailers[] = {{"X-Complete", "yes"}};
        if (co_await out.commit() == ruvia::stream_commit_result::body) {
            co_await out.end(trailers);
        }
        try {
            static_cast<void>(co_await out.sleep(std::chrono::seconds(10)));
        } catch (const std::logic_error&) {
            observed.head_trailer_sleep_stopped_ = true;
        }
        observed.head_trailer_aborted_ = out.aborted();
        observed.head_trailer_stopped_ = token.stop_requested();
        observed.head_trailer_done_ = true;
    }

    ruvia::task<void> stream_custom(ruvia::context& context_value) {
        context_value.draft().header("Content-Encoding", "x-custom");
        auto& out = context_value.stream_text();
        if (co_await out.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await out.write("payload");
        co_await out.end();
    }

    ruvia::task<void> stream_204(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::no_content);
        co_await context_value.stream_text().end();
    }

    ruvia::task<ruvia::http_response> bounded_buffered(ruvia::context& context_value) {
        co_return context_value.text(co_await context_value.req().text());
    }

    ruvia::task<ruvia::http_response> bounded_delayed_stream(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(40));
        if (context_value.req().query("read").has_value()) {
            static_cast<void>(co_await context_value.req().body_reader().read());
        }
        co_return context_value.text("accepted");
    }

    // Streams the request body and answers with the byte count.
    // A buffered POST body is readable before the SSE response starts.
    ruvia::task<void> post_sse(ruvia::context& context_value) {
        const auto received = co_await context_value.req().text();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write({.data_ = received});
    }

    ruvia::task<void> paced_sse(ruvia::context& context_value) {
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int i = 0; i < 4; ++i) {
            co_await writer.write({.data_ = "tick"});
            (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(35));
        }
    }

    ruvia::task<ruvia::http_response> count_upload(ruvia::context& context_value) {
        std::size_t bytes_value = 0;
        auto& reader_value = context_value.req().body_reader();
        while (auto chunk = co_await reader_value.read()) {
            bytes_value += chunk->size();
        }
        std::string text;
        text.append(std::to_string(bytes_value));
        co_return context_value.text(std::move(text));
    }

    ruvia::task<void> quiet_sse(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto writer = context_value.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // The request's own stop: RST_STREAM of this stream requests it.
        const auto stop = context_value.stop_token();
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
        observed.sse_wait_finished_ = true;
    }

    // Writes one chunk, then pauses far longer than the test waits: only a
    // peer disconnect can end the sleep in time.
    ruvia::task<void> sleeping(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<sleeping_stream_observation>>();
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
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

using ruvia::test::append_frame;

// With expect_continue the HEADERS announce the content but the DATA frame is
// left for the caller to send once the server has answered the expectation.
std::string request_wire(std::string_view method,
    std::string_view path, std::string_view body, bool expect_continue = false) {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    std::string block;
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

using ruvia::test::read_frame;

// Reads frames until the first HEADERS frame on stream 1 and returns the
// decoded :status; connection-level frames before it are skipped.
std::string read_first_status(asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
    for (;;) {
        const auto frame = read_frame(client);
        if (frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) ||
            frame.header_.stream_id_ != 1) {
            continue;
        }
        std::string status{};
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
            return std::string{};
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
            return std::string{};
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
TEST(Ruvia, http2_response_stream_sleep_ends_when_the_peer_disconnects) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto failures_before = ruvia::app().stats().connection_failures_;
    for (const bool send_goaway : {false, true}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("GET", "/http2-sleeping-stream", {});
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string("first\n"));
        if (send_goaway) {
            std::string goaway{};
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
            asio::write(client, asio::buffer(goaway));
        }
        client.close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
        EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
        sleeping_stream_observed->sleep_failed_ = false;
    }
    server.finish();
    // The peer going away is a normal end of the connection, not a fault.
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, http2_post_sse_reads_the_request_body_before_streaming) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("POST", "/http2-post-sse", "{\"id\":\"1\"}");
    asio::write(client, asio::buffer(wire));
    EXPECT_EQ(read_first_data(client), std::string("data: {\"id\":\"1\"}\n\n"));
}

// A quiet SSE producer waits on context::stop_token(), not on a transport
// write or writer.sleep(). RST_STREAM must wake it without closing its sibling.
TEST(Ruvia, http2_quiet_sse_peer_reset_cancels_wait_and_preserves_connection) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sse_wait_cancelled_ = false;
    sleeping_stream_observed->sse_wait_failed_ = false;
    sleeping_stream_observed->sse_wait_finished_ = false;
    const auto stats_before = ruvia::app().stats();
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const auto wire = request_wire("GET", "/http2-quiet-sse", {});
    asio::write(client, asio::buffer(wire));
    client.non_blocking(true);
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string event{};
    while (event.size() < std::string_view("data: ready\n\n").size()) {
        const auto frame = http2_streaming_test::read_frame(client, ready_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
            EXPECT_TRUE(!decoded.has_value());
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            event.append(frame.payload_);
        }
    }
    EXPECT_EQ(event, std::string("data: ready\n\n"));
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_finished_);

    client.non_blocking(false);
    // Keep a sibling upload open across the reset, rather than merely opening
    // a new stream after cancellation has already finished.
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-count-upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    std::string sibling_request{};
    append_frame(sibling_request, ruvia::http2_frame_type::headers, 0x4, 3, block);
    asio::write(client, asio::buffer(sibling_request));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sse_wait_finished_ && std::chrono::steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_finished_);
    EXPECT_TRUE(sleeping_stream_observed->sse_wait_cancelled_);
    EXPECT_TRUE(!sleeping_stream_observed->sse_wait_failed_);

    std::string sibling_body{};
    append_frame(sibling_body, ruvia::http2_frame_type::data, 0x1, 3, "x");
    asio::write(client, asio::buffer(sibling_body));
    client.non_blocking(true);
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, response_deadline);
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (frame.header_.stream_id_ == 3 && name == ":status") {
                    status = value;
                }
                return true;
            });
            EXPECT_TRUE(!decoded.has_value());
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
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("1"));
    client.close();
    server.finish();
    const auto stats_after = ruvia::app().stats();
    EXPECT_EQ(stats_after.connection_failures_, stats_before.connection_failures_);
    EXPECT_EQ(stats_after.worker_failures_, stats_before.worker_failures_);
}

TEST(Ruvia, http2_opt_in_writer_sleep_preserves_reset_error) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    sleeping_stream_observed->sleep_failed_ = false;
    sleeping_stream_observed->sleep_elapsed_ = false;
    const auto before = ruvia::app().stats().connection_failures_;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-quiet-sse?writer=1", {})));
    EXPECT_EQ(read_first_data(client), std::string("data: ready\n\n"));
    std::string reset{};
    append_frame(reset, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
    asio::write(client, asio::buffer(reset));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!sleeping_stream_observed->sleep_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(sleeping_stream_observed->sleep_failed_);
    EXPECT_TRUE(!sleeping_stream_observed->sleep_elapsed_);
    client.close();
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, before);
}

// A request whose HEADERS and complete DATA arrive in one read is still a
// streaming upload: the handler is dispatched for the head and consumes the
// already-received content rather than the stream being reset.
TEST(Ruvia, http2_streaming_upload_received_in_one_read_is_dispatched) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::size_t body_size : {std::size_t{1}, std::size_t{100}, std::size_t{5000}}) {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const auto wire = request_wire("POST", "/http2-count-upload", std::string(body_size, 'x'));
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_data(client), std::string(std::to_string(body_size)));
        client.close();
    }
    server.finish();
}

// RFC 9110 §10.1.1 on HTTP/2 follows the HTTP/1 policy: 100 Continue is sent
// when the server starts consuming the content (a streaming handler's first
// body read), and a request whose final status is known from the head alone
// (404) is answered with that status and never asked for its content.
TEST(Ruvia, http2_expect_continue_is_answered_when_the_content_is_consumed) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string body(40, 'x');
        asio::write(client, asio::buffer(request_wire("POST", "/http2-count-upload", body, true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("100"));
        std::string data{};
        append_frame(data, ruvia::http2_frame_type::data, 0x1, 1, body);
        asio::write(client, asio::buffer(data));
        EXPECT_EQ(read_first_status(client, decoder), std::string("200"));
        EXPECT_EQ(read_first_data(client), std::string("40"));
        client.close();
    }
    {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", "/http2-missing-upload", std::string(40, 'x'), true)));
        EXPECT_EQ(read_first_status(client, decoder), std::string("404"));
        client.close();
    }
    server.finish();
}

// RFC 9110 9.3.2: HEAD on an SSE GET route answers 200 with the GET head and
// ends the stream without content.
TEST(Ruvia, http2_head_falls_back_to_sse_get_route_without_content) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string content_type{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                } else if (name == "content-type") {
                    content_type = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(content_type.starts_with("text/event-stream"));
    EXPECT_TRUE(body.empty());
    client.close();
    server.finish();
}

// 完成态拒绝非法生产/等待，aborted 与取消 token 只表示真实失败。
TEST(Ruvia, http2_head_and_304_streams_complete_without_abort) {
    for (const bool not_modified : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        ruvia::test::http_server_fixture server(io);
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire(not_modified ? "GET" : "HEAD",
                                not_modified ? "/http2-head-loop-sse?not-modified" : "/http2-head-loop-sse", {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }

        auto& observed = *http2_streaming_test::sleeping_stream_observed;
        const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!observed.head_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(observed.head_loop_finished_.load());
        EXPECT_EQ(observed.head_loop_writes_.load(), 0);
        EXPECT_TRUE(!observed.head_loop_aborted_.load());
        EXPECT_TRUE(!observed.head_loop_token_stopped_.load());
        EXPECT_TRUE(observed.head_loop_sleep_rejected_.load());
        client.close();
        server.finish();
    }
}

// HEAD 在提交时完成，不要求对端断开才结束生产者。
TEST(Ruvia, http2_head_only_producer_finishes_at_commit) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("HEAD", "/http2-head-naive-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        if (frame.header_.stream_id_ == 1) {
            EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    client.close();

    auto& observed = *http2_streaming_test::sleeping_stream_observed;
    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.naive_loop_finished_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.naive_loop_finished_.load());
    EXPECT_TRUE(!observed.naive_loop_failed_.load());
    server.finish();
}

// A GET/SSE stream whose request body is already closed (END_STREAM on HEADERS)
// must not be held in connection_scanner::phase::reading_payload and killed by
// request_body_timeout_ or body_completion_timeout_ while streaming responses.
TEST(Ruvia, http2_sse_response_is_not_killed_by_request_body_completion_timeout) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io,
        ruvia::server_config{
            .worker_count_ = 1,
            .idle_timeout_ = std::chrono::milliseconds(2000),
            .connection_scan_interval_ = std::chrono::milliseconds(10),
            .request_header_timeout_ = std::chrono::milliseconds(500),
            .request_body_timeout_ = std::chrono::milliseconds(60),
            .body_completion_timeout_ = std::chrono::milliseconds(80),
        });
    ruvia::hpack_decoder decoder;
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    asio::write(client, asio::buffer(request_wire("GET", "/http2-paced-sse", {})));
    client.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string status{};
    std::string body{};
    bool ended = false;
    while (!ended) {
        const auto frame = http2_streaming_test::read_frame(client, deadline);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
            (frame.header_.flags_ & 0x1) == 0) {
            std::string ack{};
            append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
            asio::write(client, asio::buffer(ack));
            continue;
        }
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        EXPECT_TRUE(frame.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    status = value;
                }
                return true;
            });
            ended = (frame.header_.flags_ & 0x1) != 0;
        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
            body.append(frame.payload_);
            ended = (frame.header_.flags_ & 0x1) != 0;
        }
    }
    EXPECT_EQ(status, std::string("200"));
    EXPECT_EQ(body, std::string("data: tick\n\ndata: tick\n\ndata: tick\n\ndata: tick\n\n"));
    client.close();
    server.finish();
}

TEST(Ruvia, http2_oversized_request_body_answers_413_without_100_continue) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string oversized(64, 'x');
    // Read through END_STREAM: a valid head alone cannot prove that the
    // rejection's borrowed diagnostic survived until its response was sent.
    const auto check_body_limit_response = [&](asio::ip::tcp::socket& client, ruvia::hpack_decoder& decoder) {
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::string body{};
        unsigned response_heads = 0;
        bool ended = false;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                (frame.header_.stream_id_ == 1 &&
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream))) {
                throw std::runtime_error("oversized HTTP/2 request reset before its complete 413 response");
            }
            if (frame.header_.stream_id_ != 1) {
                continue;
            }
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                ++response_heads;
                std::string status{};
                const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                    if (name == ":status") {
                        status = value;
                    }
                    return true;
                });
                EXPECT_TRUE(!decoded.has_value());
                EXPECT_EQ(status, std::string("413"));
                ended = (frame.header_.flags_ & 0x1) != 0;
            } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                body.append(frame.payload_);
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(response_heads, 1U);
        const auto error = ruvia::json_object::parse(body);
        const auto status = error.get<ruvia::uint32>("status").value();
        const auto code = error.get<ruvia::string>("code").value();
        EXPECT_TRUE(status && static_cast<std::uint32_t>(*status) == 413);
        EXPECT_TRUE(code && code->view() == "request_body_too_large");
    };

    // 1. Declared Content-Length exceeding route body_limit<16> with Expect: 100-continue
    //    must answer 413 immediately without sending 100 Continue first.
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("POST", path, oversized, true)));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 2. Unannounced (no Content-Length) DATA exceeding route body_limit<16> on both
    //    buffered and stream_body routes must answer 413 rather than RST_STREAM(CANCEL).
    for (const std::string_view path : {"/http2-bounded-buffered", "/http2-bounded-stream"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }

    // 3. Post-dispatch oversized DATA while stream_body handler is awaiting another
    //    async operation:
    //    - when the handler returns 200 without reading the body, the server still
    //      answers 413 instead of committing 200;
    //    - when the request carries Expect: 100-continue (without Content-Length) and
    //      the handler performs its first read after oversized DATA arrived, 100 Continue
    //      is skipped and 413 is returned directly.
    for (const bool read_after_wait : {false, true}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(
            block, ":path", read_after_wait ? "/http2-bounded-delayed-stream?read=1" : "/http2-bounded-delayed-stream");
        if (read_after_wait) {
            ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, oversized);
        asio::write(client, asio::buffer(wire));
        check_body_limit_response(client, decoder);
        client.close();
    }
    server.finish();
}

TEST(Ruvia, http2_head_stream_with_trailers_strips_trailer_header_and_stops_eof_token) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    auto& observed = *sleeping_stream_observed;
    observed.head_trailer_stopped_ = false;
    observed.head_trailer_sleep_stopped_ = false;
    observed.head_trailer_aborted_ = false;
    observed.head_trailer_done_ = false;

    for (const std::string_view path : {"/http2-head-trailer-write", "/http2-head-trailer-only"}) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request_wire("HEAD", path, {})));
        client.non_blocking(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool ended = false;
        bool saw_trailer_header = false;
        unsigned header_frames = 0;
        while (!ended) {
            const auto frame = http2_streaming_test::read_frame(client, deadline);
            if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 0x1) == 0) {
                std::string ack{};
                append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                asio::write(client, asio::buffer(ack));
                continue;
            }
            if (frame.header_.stream_id_ == 1) {
                EXPECT_TRUE(frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
                ++header_frames;
                (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view) {
                    if (name == "trailer" || name == "x-complete") {
                        saw_trailer_header = true;
                    }
                    return true;
                });
                ended = (frame.header_.flags_ & 0x1) != 0;
            }
        }
        EXPECT_EQ(header_frames, 1U);
        EXPECT_TRUE(!saw_trailer_header);
        client.close();
    }

    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!observed.head_trailer_done_ && std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed.head_trailer_done_.load());
    EXPECT_TRUE(!observed.head_trailer_aborted_.load());
    EXPECT_TRUE(!observed.head_trailer_stopped_.load());
    EXPECT_TRUE(observed.head_trailer_sleep_stopped_.load());
    server.finish();
}

TEST(Ruvia, http2_stream_custom_content_encoding_and_bodyless_status_negotiation) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto check_status = [&](std::string_view path, std::string_view accept_encoding,
                                  std::string_view expected_status) {
        ruvia::hpack_decoder decoder;
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", path);
        ruvia::hpack_encoder::encode_header(block, "accept-encoding", accept_encoding);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        asio::write(client, asio::buffer(wire));
        EXPECT_EQ(read_first_status(client, decoder), std::string(expected_status));
        client.close();
    };
    check_status("/http2-stream-custom", "x-custom, identity;q=0", "200");
    check_status("/http2-stream-custom", "gzip", "406");
    check_status("/http2-stream-204", "*;q=0", "204");
    server.finish();
}

namespace {
ruvia::task<std::string> collect_text(ruvia::http_client_response& response) {
    const auto bytes_value = co_await response.stream().read_all();
    const auto view = bytes_value.bytes();
    co_return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}
struct push_observation final {
    std::atomic<unsigned> promised_{};
    std::atomic<unsigned> refused_{};
    std::atomic<unsigned> streamed_{};
};

const auto observation = std::make_shared<push_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<push_observation>>([] { return observation; });
    return true;
}();

std::shared_ptr<push_observation> prepare_observation() {
    observation->promised_ = 0;
    observation->refused_ = 0;
    observation->streamed_ = 0;
    return observation;
}

class push_routes final {
public:
    RUVIA_ROUTES_BEGIN(push_routes)
    RUVIA_GET("/parent", parent);
    RUVIA_GET("/two", parent);
    RUVIA_GET("/large", parent);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/large-asset", large);
    RUVIA_GET("/delayed-parent", parent);
    RUVIA_GET("/delayed-asset", delayed);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> parent(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        const auto count = context_value.req().path() == "/two" ? 2U : 1U;
        const auto path = context_value.req().path() == "/large"            ? "/large-asset"
                          : context_value.req().path() == "/delayed-parent" ? "/delayed-asset"
                                                                            : "/asset";
        const std::array<ruvia::http_header_view, 1> headers{{{"x-promise", "owned-request"}}};
        for (unsigned i = 0; i != count; ++i) {
            if (context_value.push({.scheme_ = "http", .authority_ = context_value.req().authority(), .path_ = path, .headers_ = headers})) {
                ++owner_value.promised_;
            } else {
                ++owner_value.refused_;
            }
        }
        co_return context_value.text("parent");
    }
    ruvia::task<ruvia::http_response> asset(ruvia::context& context_value) {
        if (context_value.req().header("x-promise") != "owned-request") {
            throw std::runtime_error("lost push request metadata");
        }
        context_value.draft().header("x-asset", "metadata");
        co_return context_value.text("pushed-body");
    }
    // Answers well after the client's first, interrupted wait for it.
    ruvia::task<ruvia::http_response> delayed(ruvia::context& context_value) {
        (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(300), context_value.stop_token());
        co_return context_value.text("late-asset");
    }
    ruvia::task<void> large(ruvia::context& context_value) {
        auto& owner_value = *context_value.worker_state<std::shared_ptr<push_observation>>();
        std::string chunk(16384, 'p');
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (unsigned i = 0; i != 80; ++i) {
            co_await context_value.stream().write(chunk);
        }
        ++owner_value.streamed_;
        co_await context_value.stream().end();
    }
};

}  // namespace

TEST(Ruvia, http2_push_routes_and_client_preserve_owners_flow_control_cold_reads_and_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_push> retained;
        std::optional<ruvia::http_client_response> retained_response;
        {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = true}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                for (unsigned repeat = 0; repeat != 128; ++repeat) {
                    auto parent_value = co_await client_origin.get("/parent").send();
                    EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                    auto push = client_origin.next_push();
                    EXPECT_TRUE(push.has_value());
                    if (!push) {
                        throw std::runtime_error("missing push");
                    }
                    EXPECT_TRUE(push->request().path_ == "/asset");
                    EXPECT_TRUE(push->request().headers_.front().value() == "owned-request");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        auto overlap = push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    EXPECT_TRUE(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                    EXPECT_TRUE(response.header("x-asset") == "metadata");
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                    EXPECT_TRUE(!client_origin.next_push());
                    if (repeat == 0) {
                        retained.emplace(std::move(moved));
                        retained_response.emplace(std::move(response));
                    }
                    EXPECT_TRUE(retained->request().path_ == "/asset");
                    EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
                }
                auto parent_value = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto pushed = client_origin.next_push();
                EXPECT_TRUE(pushed.has_value());
                if (!pushed) {
                    throw std::runtime_error("missing large push");
                }
                auto response = co_await pushed->response();
                std::size_t bytes_value = 0;
                while (auto chunk = co_await response.stream().text()) {
                    EXPECT_TRUE(chunk->find_first_not_of('p') == std::string_view::npos);
                    bytes_value += chunk->size();
                }
                EXPECT_EQ(bytes_value, std::size_t{80 * 16384});
                EXPECT_EQ(observed->streamed_.load(), 1U);
                auto cancelled_parent = co_await client_origin.get("/large").send();
                EXPECT_TRUE((co_await collect_text(cancelled_parent)) == "parent");
                auto cancelled_push = client_origin.next_push();
                EXPECT_TRUE(cancelled_push.has_value());
                cancelled_push.reset();
                auto after_cancel = co_await client_origin.get("/parent").send();
                EXPECT_TRUE((co_await collect_text(after_cancel)) == "parent");
                auto after_push = client_origin.next_push();
                EXPECT_TRUE(after_push.has_value());
                if (after_push) {
                    auto after_response = co_await after_push->response();
                    EXPECT_TRUE((co_await collect_text(after_response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, std::size_t{131});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, std::size_t{0});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        EXPECT_TRUE(retained->request().path_ == "/asset");
        EXPECT_TRUE(retained_response->header("x-asset") == "metadata");
        retained_response.reset();
        retained.reset();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, http2_push_queue_overflow_and_disabled_permission_keep_parent_response_usable) {
    for (const bool enabled : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto observed = prepare_observation();
        ruvia::test::http_server_fixture server(io);
        auto run = [&]() -> ruvia::task<void> {
            const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                             .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                             .push_ = {.enabled_ = enabled, .max_queued_pushes_ = 1}});
            const auto client_origin = client.client();
            std::exception_ptr failure;
            try {
                auto parent_value = co_await client_origin.get("/two").send();
                EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
                auto push = client_origin.next_push();
                EXPECT_TRUE(push.has_value() == enabled);
                if (push) {
                    auto response = co_await push->response();
                    EXPECT_TRUE((co_await collect_text(response)) == "pushed-body");
                }
                EXPECT_EQ(client_origin.stats().received_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(client_origin.stats().rejected_pushes_, enabled ? std::size_t{1} : std::size_t{0});
                EXPECT_EQ(observed->refused_.load(), enabled ? 0U : 2U);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            server.finish();
            if (failure) {
                std::rethrow_exception(failure);
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
    }
}

// Waiting for a pushed response is a read: its timeout or an already-stopped
// token ends only that wait, and a later response() still receives it.
TEST(Ruvia, http2_push_response_wait_honours_its_operation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto observed = prepare_observation();
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false},
                                                         .push_ = {.enabled_ = true}});
        const auto client_origin = client.client();
        std::exception_ptr failure;
        try {
            auto parent_value = co_await client_origin.get("/delayed-parent").send();
            EXPECT_TRUE((co_await collect_text(parent_value)) == "parent");
            auto push = client_origin.next_push();
            EXPECT_TRUE(push.has_value());
            if (push) {
                EXPECT_THROW((void)push->response({.timeout_ = std::chrono::milliseconds(0)}), std::invalid_argument);
                const auto failure_of = [](ruvia::task<ruvia::http_client_response> wait) -> ruvia::task<std::optional<std::error_code>> {
                    try {
                        (void)co_await std::move(wait);
                    } catch (const ruvia::http_client_error& error) {
                        co_return error.code();
                    }
                    co_return std::nullopt;
                };
                EXPECT_TRUE((co_await failure_of(push->response({.timeout_ = std::chrono::milliseconds(50)}))) ==
                            std::make_error_code(std::errc::timed_out));
                ruvia::stop_source stop;
                stop.request_stop();
                EXPECT_TRUE((co_await failure_of(push->response({.stop_token_ = stop.token()}))) ==
                            std::make_error_code(std::errc::operation_canceled));
                auto response = co_await push->response();
                EXPECT_TRUE((co_await collect_text(response)) == "late-asset");
            }
            EXPECT_EQ(observed->promised_.load(), 1U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        server.finish();
        if (failure) {
            std::rethrow_exception(failure);
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

namespace http2_file_flow_control_test {

constexpr std::size_t file_size = 65535;

std::filesystem::path& file_path() {
    static std::filesystem::path path;
    return path;
}

[[nodiscard]] const std::filesystem::path& empty_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-http2-empty-file");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> empty_root;

class file_routes final {
    RUVIA_ROUTES_BEGIN(file_routes)
    RUVIA_GET("/http2-file-flow-control", file);
    RUVIA_GET("/http2-file-mutated", mutated_file);
    RUVIA_GET("/http2-file-ok", ok_text);
    RUVIA_GET("/http2-indexed-empty", indexed_empty);
    RUVIA_GET("/http2-empty-mutated", mutated_empty);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        auto response = co_await context_value.file({.path_ = file_path()});
        // Make the exercised transport path observable, rather than assuming
        // that the file is above the context::file in-memory threshold.
        response.header("x-file-backed", response.file_body().has_value() ? "yes" : "no");
        co_return response;
    }

    ruvia::task<ruvia::http_response> mutated_file(ruvia::context& context_value) {
        {
            std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
            const std::string bytes(file_size, 'f');
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        // The client truncates the file once HEADERS arrive: the file was
        // validated before them, and the multipart boundary DATA is submitted
        // before the file range read fails and resets the stream.
        co_return co_await context_value.file({.path_ = file_path()});
    }

    ruvia::task<ruvia::http_response> mutated_empty(ruvia::context& context_value) {
        const auto path = empty_directory() / "body.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
        }
        auto response = co_await context_value.file({.path_ = path});
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "changed";
        }
        co_return response;
    }

    ruvia::task<ruvia::http_response> indexed_empty(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*empty_root, {.relative_path_ = "body.bin"});
    }

    ruvia::task<ruvia::http_response> ok_text(ruvia::context& context_value) {
        co_return context_value.text("alive");
    }
};

struct test_file final {
    test_file() {
        file_path() = directory_.path() / "body.bin";
        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
        const std::string bytes(file_size, 'f');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write HTTP/2 flow-control test file");
        }
    }

private:
    ruvia::test::scoped_temp_directory directory_{"ruvia-http2-file"};
};

using ruvia::test::append_frame;

std::string request_wire(bool exhaust_connection) {
    std::string wire(ruvia::http2_client_preface);
    // In each case exactly one window reaches zero after the file. Keep the
    // stream window positive when testing the connection window, and vice versa.
    const std::string_view settings = exhaust_connection
                                          ? std::string_view("\0\4\0\1\xff\xfe", 6)   // INITIAL_WINDOW_SIZE = 131070
                                          : std::string_view("\0\4\0\0\xff\xff", 6);  // INITIAL_WINDOW_SIZE = 65535
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    if (!exhaust_connection) {
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
            std::string_view("\0\0\xff\xff", 4));
    }
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

using ruvia::test::received_frame;

ruvia::task<received_frame> read_frame(asio::ip::tcp::socket& socket) {
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    const auto head = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(socket, asio::buffer(header_bytes), std::move(handler));
    });
    if (head.error_code()) {
        throw std::system_error(head.error_code());
    }
    const auto header = ruvia::parse_http2_frame_header(header_bytes);
    if (!header) {
        throw std::logic_error("invalid HTTP/2 flow-control response frame");
    }
    received_frame frame{*header, std::string(header->length_, '\0')};
    if (!frame.payload_.empty()) {
        const auto body = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read(socket, asio::buffer(frame.payload_), std::move(handler));
        });
        if (body.error_code()) {
            throw std::system_error(body.error_code());
        }
    }
    co_return frame;
}

void check_file_end(bool exhaust_connection) {
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool file_backed = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        // Bound missing END_STREAM without a sleeping thread or a race between
        // reading the final file bytes and deciding whether to return credit.
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto wire = request_wire(exhaust_connection);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            bool credited = false;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    const auto acknowledged = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(ack), std::move(handler));
                    });
                    if (acknowledged.error_code()) {
                        throw std::system_error(acknowledged.error_code());
                    }
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 file response aborted");
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        } else if (name == "x-file-backed") {
                            file_backed = value == "yes";
                        }
                        return true;
                    });
                    if (decoded.has_value()) {
                        throw std::runtime_error("invalid HTTP/2 file response headers");
                    }
                    EXPECT_TRUE((frame.header_.flags_ & 0x1) == 0);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    EXPECT_TRUE((frame.header_.flags_ & 0x8) == 0);
                    body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                    if (ended) {
                        EXPECT_TRUE(frame.payload_.empty());
                        EXPECT_TRUE(credited);
                    }
                }
                // The file exhausted one send window. The empty END_STREAM DATA
                // frame waits for that window to open (RFC 9113 section 6.9.1
                // permits sending it at zero window; nghttp2 does not), so one
                // byte of credit releases it.
                if (!ended && !credited && body.size() == file_size) {
                    credited = true;
                    std::string increment{};
                    append_frame(increment, ruvia::http2_frame_type::window_update, 0, exhaust_connection ? 0 : 1,
                        std::string_view("\0\0\0\1", 4));
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(increment), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(file_backed);
    EXPECT_EQ(body, std::string(file_size, 'f'));
    EXPECT_TRUE(ended);
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_file_flow_control_test

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_connection_window) {
    http2_file_flow_control_test::check_file_end(true);
}

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_stream_window) {
    http2_file_flow_control_test::check_file_end(false);
}

TEST(Ruvia, http2_buffered_response_resumes_after_stream_window_update) {
    using namespace http2_file_flow_control_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool credited = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view("\0\4\0\0\0\1", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                std::string reply;
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    append_frame(reply, ruvia::http2_frame_type::settings, 0x1, 0, {});
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 buffered response aborted");
                }
                if (frame.header_.stream_id_ == 1) {
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status = value;
                            }
                            return true;
                        });
                        if (decoded.has_value()) {
                            throw std::runtime_error("invalid HTTP/2 buffered response headers");
                        }
                        ended = (frame.header_.flags_ & 0x1) != 0;
                    } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        body.append(frame.payload_);
                        ended = (frame.header_.flags_ & 0x1) != 0;
                        if (!credited && !body.empty()) {
                            EXPECT_EQ(body, std::string_view("a"));
                            EXPECT_FALSE(ended);
                            credited = true;
                            append_frame(reply, ruvia::http2_frame_type::window_update, 0, 1,
                                std::string_view("\0\0\0\4", 4));
                        }
                    }
                }
                if (!reply.empty()) {
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(reply), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_EQ(status, std::string_view("200"));
    EXPECT_EQ(body, std::string_view("alive"));
    EXPECT_TRUE(credited);
    EXPECT_TRUE(ended);
    EXPECT_FALSE(expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// A file body is validated before HEADERS but read only once the peer grants
// send window: bytes the file held before the window opened are never sent.
// Truncating the file between HEADERS and the first WINDOW_UPDATE therefore
// fails the read and resets the stream without a single DATA byte.
TEST(Ruvia, http2_file_body_is_read_only_after_send_window_is_granted) {
    using namespace http2_file_flow_control_test;
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool reset = false;
    std::string status{};
    std::size_t data_bytes = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            std::string wire(ruvia::http2_client_preface);
            // INITIAL_WINDOW_SIZE = 0: the stream has no send window at all.
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            while (!reset) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    co_await send_bytes(ack);
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        }
                        return true;
                    });
                    {
                        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    }
                    std::string update{};
                    append_frame(update, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\0\0\xff\xff", 4));
                    co_await send_bytes(update);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    data_bytes += frame.payload_.size();
                    if ((frame.header_.flags_ & 0x1) != 0) {
                        break;
                    }
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    reset = true;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(reset);
    EXPECT_EQ(data_bytes, std::size_t{0});
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_failed_file_writes_release_output_budget_slots) {
    const http2_file_flow_control_test::test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    std::string final_status{};
    std::string final_body{};
    std::size_t resets_seen = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(15));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::string wire(ruvia::http2_client_preface);
            // A 10-byte initial stream window splits the first multipart
            // boundary chunk (~80 bytes) across window updates, so DATA output
            // is in flight when the file range read fails and resets the stream.
            http2_file_flow_control_test::append_frame(
                wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\x0a", 6));
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            // Five failed file responses exceed the 4 connection-wide DATA
            // output budget slots if any reset leaks its slot.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-mutated");
                ruvia::hpack_encoder::encode_header(block, "range", "bytes=0-3, 10-13");
                std::string req{};
                http2_file_flow_control_test::append_frame(
                    req, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(req);
                bool stream_done = false;
                while (!stream_done) {
                    const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        http2_file_flow_control_test::append_frame(
                            ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        (void)decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
                        std::ofstream output(http2_file_flow_control_test::file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        std::string update{};
                        http2_file_flow_control_test::append_frame(
                            update, ruvia::http2_frame_type::window_update, 0, stream_id,
                            std::string_view("\0\0\x10\0", 4));
                        co_await send_bytes(update);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        ++resets_seen;
                        stream_done = true;
                    }
                }
            }
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            std::string req{};
            http2_file_flow_control_test::append_frame(req, ruvia::http2_frame_type::headers, 0x5, 11, block);
            co_await send_bytes(req);
            bool ended = false;
            while (!ended) {
                const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                if (frame.header_.stream_id_ != 11) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            final_status = value;
                        }
                        return true;
                    });
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    final_body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(resets_seen, std::size_t{5});
    EXPECT_EQ(final_status, std::string("200"));
    EXPECT_EQ(final_body, std::string("alive"));
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_empty_file_validation_precedes_end_stream_headers) {
    using namespace http2_file_flow_control_test;
    const auto& directory = empty_directory();
    const auto path = directory / "body.bin";
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            ruvia::hpack_decoder decoder;
            std::string preface(ruvia::http2_client_preface);
            append_frame(preface, ruvia::http2_frame_type::settings, 0, 0, {});
            co_await send_bytes(preface);
            // Valid empty, changed, removed, changed after framing, then a
            // healthy stream proves failed validation remains stream-local.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                // Changed, removed, and changed after framing: each fails
                // validation before HEADERS and gets an error response.
                const bool invalid = stream_id == 3 || stream_id == 5 || stream_id == 7;
                if (stream_id <= 5) {
                    {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    }
                    empty_root = std::make_unique<ruvia::static_root>(directory,
                        ruvia::static_root_options{
                            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
                        });
                    if (stream_id == 3) {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                        output << "changed";
                    } else if (stream_id == 5) {
                        EXPECT_TRUE(std::filesystem::remove(path));
                    }
                }
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path",
                    stream_id == 9 ? "/http2-file-ok" : stream_id == 7 ? "/http2-empty-mutated"
                                                                       : "/http2-indexed-empty");
                std::string request{};
                append_frame(request, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(request);
                bool ended = false;
                bool reset = false;
                bool saw_head = false;
                std::string status{};
                std::string body{};
                while (!ended && !reset) {
                    const auto frame = co_await read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id) {
                        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                            saw_head = true;
                            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                                if (name == ":status") {
                                    status = value;
                                }
                                return true;
                            });
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                            body.append(frame.payload_);
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                            reset = true;
                        }
                    }
                }
                EXPECT_TRUE(saw_head);
                if (invalid) {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("500"));
                } else {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("200"));
                    EXPECT_EQ(body, stream_id == 9 ? std::string("alive") : std::string{});
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto operation = attachment.loop().start(run());
    io.run();
    std::move(operation).get();
    empty_root.reset();
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace http2_client_upload_test {

enum class response_mode { ordinary,
    continued,
    rejected,
    reset,
    abandoned,
    abandoned_pending,
    buffered,
    streaming,
    disconnected,
    refused,
    refused_always,
    connection_lost,
    connection_lost_upload,
    goaway_upload };

constexpr std::size_t streaming_payload_bytes = 256 * 1024;

using ruvia::test::append_frame;

// A raw peer deliberately keeps the request half open after the final response.
// It advertises one stream and never supplies the optional server-side reset, so
// client retirement cannot be masked by a server request-lease destructor.
class early_final_peer final {
public:
    early_final_peer(asio::io_context& io, const ruvia::worker_ref& worker, response_mode mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          done_(worker),
          mode_(mode) {}
    early_final_peer(asio::io_context&, ruvia::worker_ref&&, response_mode) = delete;

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            done_.notify();
        });
    }

    void stop() {
        stopping_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::string received_{};
    bool request_ended_{false};
    bool request_reset_{false};
    unsigned requests_{0};

private:
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 client preface");
        }
        if (mode_ == response_mode::connection_lost || mode_ == response_mode::connection_lost_upload ||
            mode_ == response_mode::goaway_upload) {
            std::string wire{};
            const std::array<char, 6> no_streams{0, 3, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {no_streams.data(), no_streams.size()});
            const std::array<char, 8> ping{};
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
            co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            for (;;) {
                std::array<char, ruvia::http2_frame_header_bytes> bytes;
                co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
                const auto header = ruvia::parse_http2_frame_header(bytes);
                if (!header) {
                    throw std::runtime_error("invalid client HTTP/2 frame");
                }
                std::string payload(header->length_, '\0');
                if (!payload.empty()) {
                    co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
                }
                const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
                if (type == ruvia::http2_frame_type::headers) {
                    throw std::runtime_error("request submitted despite zero stream limit");
                }
                if (type == ruvia::http2_frame_type::ping && (header->flags_ & 1U) != 0) {
                    break;
                }
            }
            if (mode_ == response_mode::goaway_upload) {
                wire.clear();
                const std::array<char, 8> goaway{};
                append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                std::array<char, 1024> remaining;
                std::error_code error;
                do {
                    co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
                } while (!error);
            }
            socket_.close();
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
            if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
                throw std::runtime_error("missing retried HTTP/2 client preface");
            }
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        if (mode_ == response_mode::disconnected) {
            const std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {zero_window.data(), zero_window.size()});
        }
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        ruvia::hpack_decoder decoder;
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid client HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            wire.clear();
            const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
            if (type == ruvia::http2_frame_type::settings && (header->flags_ & 1U) == 0) {
                append_frame(wire, type, 1, 0, {});
            } else if (type == ruvia::http2_frame_type::headers) {
                std::string method{};
                std::string path{};
                const auto decoded = decoder.decode(payload, [&](std::string_view name, std::string_view value) {
                    if (name == ":method") {
                        method = value;
                    }
                    if (name == ":path") {
                        path = value;
                    }
                    return true;
                });
                if (decoded.has_value() || (header->flags_ & 4U) == 0) {
                    throw std::runtime_error("invalid client request fields");
                }
                ++requests_;
                if ((requests_ == 1 && mode_ == response_mode::refused) || mode_ == response_mode::refused_always) {
                    // RFC 9113 section 8.7: the request was not processed.
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected refused request");
                    }
                    first_stream = header->stream_id_;
                    request_ended_ = (header->flags_ & 1U) != 0;
                    const std::array<char, 4> refused_stream{0, 0, 0, 7};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream,
                        {refused_stream.data(), refused_stream.size()});
                    co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                    continue;
                }
                if (requests_ == 1 && mode_ == response_mode::connection_lost) {
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected retried request");
                    }
                    first_stream = header->stream_id_;
                } else if (requests_ == 1) {
                    if (method != "POST" || path != "/upload") {
                        throw std::runtime_error("unexpected initial request");
                    }
                    first_stream = header->stream_id_;
                    if (mode_ == response_mode::continued) {
                        std::string interim;
                        ruvia::hpack_encoder::encode_header(interim, ":status", "100");
                        append_frame(wire, type, 4, first_stream, interim);
                    }
                } else if (requests_ == 2) {
                    if (method != "GET" || path != "/next" || header->stream_id_ != first_stream + 2 ||
                        (!request_ended_ && !request_reset_ && mode_ != response_mode::reset)) {
                        throw std::runtime_error("previous request half was not retired on the reused connection");
                    }
                } else {
                    throw std::runtime_error("unexpected extra request");
                }
                const bool rejected = requests_ == 1 && mode_ == response_mode::rejected;
                const std::string_view body = requests_ == 2 || mode_ == response_mode::connection_lost ? "next" : rejected ? "no"
                                                                                                                            : "ok";
                std::string fields;
                const bool empty_final = requests_ == 1 &&
                                         (mode_ == response_mode::buffered || mode_ == response_mode::streaming);
                ruvia::hpack_encoder::encode_header(fields, ":status", empty_final ? "204" : rejected ? "413"
                                                                                                      : "200");
                if (!empty_final) {
                    ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(body.size()));
                }
                append_frame(wire, type, empty_final ? 5 : 4, header->stream_id_, fields);
                if (!empty_final && (requests_ != 1 || mode_ != response_mode::abandoned_pending)) {
                    append_frame(wire, ruvia::http2_frame_type::data, 1, header->stream_id_, body);
                }
                if (requests_ == 1 && mode_ == response_mode::reset) {
                    const std::array<char, 4> no_error{};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream, {no_error.data(), no_error.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::data) {
                received_.append(payload);
                request_ended_ = (header->flags_ & 1U) != 0;
                if (mode_ == response_mode::buffered && received_.size() == 65535 && !window_returned_) {
                    window_returned_ = true;
                    const std::string_view increment("\0\0\0\1", 4);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream, increment);
                } else if (mode_ == response_mode::streaming && !payload.empty()) {
                    const auto increment_value = static_cast<std::uint32_t>(payload.size());
                    const std::array<char, 4> increment{
                        static_cast<char>((increment_value >> 24U) & 0x7fU),
                        static_cast<char>((increment_value >> 16U) & 0xffU),
                        static_cast<char>((increment_value >> 8U) & 0xffU),
                        static_cast<char>(increment_value & 0xffU)};
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
                        {increment.data(), increment.size()});
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream,
                        {increment.data(), increment.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::rst_stream) {
                request_reset_ = true;
            }
            if (!wire.empty()) {
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            }
            if (requests_ == 2 || (requests_ == 1 && (mode_ == response_mode::disconnected ||
                                                         mode_ == response_mode::connection_lost ||
                                                         ((mode_ == response_mode::connection_lost_upload ||
                                                              mode_ == response_mode::goaway_upload) &&
                                                             request_ended_)))) {
                co_return;
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    ruvia::worker_signal done_;
    response_mode mode_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool finished_{false};
    bool window_returned_{false};
};

class draining_peer final {
public:
    draining_peer(asio::io_context& io, const ruvia::worker_ref& worker)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          start_drain_(io),
          ready_(worker),
          done_(worker) {
        start_drain_.expires_at(asio::steady_timer::time_point::max());
    }
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            ready_.notify();
            done_.notify();
        });
    }
    void drain() {
        start_drain_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        start_drain_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    [[nodiscard]] ruvia::task<void> wait_ready() {
        while (!draining_ && !finished_) {
            co_await ready_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 preface");
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                first_stream = header->stream_id_;
                break;
            }
        }
        wire.clear();
        const std::array<char, 1> ok{static_cast<char>(0x88)};
        append_frame(wire, ruvia::http2_frame_type::headers, 4, first_stream, {ok.data(), ok.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::error_code error;
        co_await start_drain_.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (stopping_) {
            co_return;
        }
        wire.clear();
        const std::array<char, 8> goaway{0, 0, 0, static_cast<char>(first_stream), 0, 0, 0, 0};
        const std::array<char, 8> ping{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                throw std::runtime_error("second request was submitted despite stream limit");
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::ping &&
                (header->flags_ & 1U) != 0) {
                draining_ = true;
                ready_.notify();
                break;
            }
        }
        std::array<char, 1024> remaining;
        do {
            co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
        } while (!error);
    }
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer start_drain_;
    ruvia::worker_signal ready_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool draining_{false};
    bool stopping_{false};
    bool finished_{false};
};

void exercise_abandon_during_drain() {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    draining_peer peer(io, worker);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto held = co_await client.client().get("/hold").send();
            EXPECT_TRUE(held.status() == ruvia::http_status::ok);
            {
                auto exchange = co_await client.client().post("/abandon").upload({.content_length_ = 7});
                // The peer's single stream and max_open_ leave the upload
                // buffered for admission, its head never submitted.
                for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 1; ++i) {
                    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
                }
                EXPECT_EQ(client.client().stats().buffered_requests_, 1U);
                EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
                peer.drain();
                co_await peer.wait_ready();
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(50));
            }
            for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 0; ++i) {
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            }
            EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
            EXPECT_EQ(client.client().stats().buffered_requests_, 0U);
            EXPECT_TRUE(!held.stream().complete());
        } catch (...) {
            failure = std::current_exception();
        }
        peer.stop();
        try {
            co_await client.shutdown();
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void exercise(response_mode mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    // The peer's signal borrows this handle through completion and join.
    const auto worker = attachment.loop().worker();
    early_final_peer peer(io, worker, mode);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(2),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            if (mode == response_mode::refused_always) {
                bool rejected = false;
                try {
                    (void)co_await client.client().get("/next").send();
                } catch (const ruvia::http_client_error& error) {
                    rejected = true;
                    EXPECT_EQ(error.code(), ruvia::http_client_errc::protocol_error);
                    EXPECT_TRUE(error.request_written());
                    EXPECT_NE(error.connection_info(), nullptr);
                }
                EXPECT_TRUE(rejected);
                EXPECT_EQ(peer.requests_, 2U);
            } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                // These cases start with the request below.
            } else if (mode == response_mode::connection_lost_upload || mode == response_mode::goaway_upload) {
                auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                co_await exchange.body().write("payload");
                co_await exchange.body().end();
                auto response = co_await exchange.response();
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "ok");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, 1U);
                EXPECT_EQ(peer.received_, "payload");
                EXPECT_TRUE(peer.request_ended_);
            } else if (mode == response_mode::abandoned_pending) {
                std::optional<ruvia::http_client_response> retained;
                {
                    auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                    retained.emplace(co_await exchange.response());
                    EXPECT_TRUE(retained->status() == ruvia::http_status::ok);
                    EXPECT_TRUE(!retained->stream().complete());
                }
                bool cancelled = false;
                try {
                    (void)co_await retained->stream().read_all();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(cancelled);
                bool completion_cancelled = false;
                try {
                    (void)retained->stream().complete();
                } catch (const ruvia::http_client_error& error) {
                    completion_cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(completion_cancelled);
            } else if (mode == response_mode::buffered) {
                const std::string payload(65536, 'b');
                auto response = co_await client.client().post("/upload").body(payload).send();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
            } else if (mode == response_mode::streaming) {
                const std::string payload(streaming_payload_bytes, 's');
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = payload.size(), .max_chunk_bytes_ = payload.size()});
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
                co_await exchange.body().write(payload);
                co_await exchange.body().end();
                EXPECT_TRUE(exchange.body().complete());
            } else {
                const bool expect = mode == response_mode::continued || mode == response_mode::rejected;
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = 7, .expectation_ = expect ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), mode == response_mode::rejected ? 413 : 200);
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                    mode == response_mode::rejected ? "no" : "ok");
                EXPECT_TRUE(response.stream().complete());
                if (mode == response_mode::disconnected) {
                    // Start a write with no stream credit, then close the peer.
                    // The write must report its I/O error without replacing a
                    // response EOF that the application has already observed.
                    ruvia::worker_signal started(worker);
                    auto waiting = started.wait();
                    std::optional<std::error_code> write_error;
                    ruvia::task_scope writers(worker);
                    const auto write = [&]() -> ruvia::task<void> {
                        started.notify();
                        try {
                            co_await exchange.body().write("payload");
                        } catch (const ruvia::http_client_error& error) {
                            write_error = error.code();
                        }
                    };
                    writers.spawn(write());
                    co_await std::move(waiting);
                    peer.stop();
                    co_await writers.join();
                    EXPECT_TRUE(write_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
                    co_await client.shutdown();
                    EXPECT_TRUE(!(co_await response.stream().text()).has_value());
                    EXPECT_TRUE(response.stream().complete());
                } else if (mode == response_mode::ordinary || mode == response_mode::continued) {
                    co_await exchange.body().write("payload");
                    co_await exchange.body().end();
                    EXPECT_TRUE(exchange.body().complete());
                } else if (mode != response_mode::abandoned) {
                    bool cancelled = false;
                    try {
                        co_await exchange.body().write("payload");
                    } catch (const ruvia::http_client_error& error) {
                        cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(cancelled);
                }
            }
            if (mode != response_mode::disconnected && mode != response_mode::connection_lost_upload &&
                mode != response_mode::goaway_upload && mode != response_mode::refused_always) {
                auto next = co_await client.client().get("/next").send();
                const auto bytes = co_await next.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "next");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, mode == response_mode::connection_lost ? 1U : 2U);
                if (mode == response_mode::ordinary || mode == response_mode::continued ||
                    mode == response_mode::buffered || mode == response_mode::streaming) {
                    const auto expected = mode == response_mode::buffered
                                              ? std::string(65536, 'b')
                                          : mode == response_mode::streaming
                                              ? std::string(streaming_payload_bytes, 's')
                                              : std::string("payload");
                    EXPECT_EQ(peer.received_, expected);
                    EXPECT_TRUE(peer.request_ended_);
                } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(!peer.request_reset_);
                } else {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(mode == response_mode::reset || peer.request_reset_);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.stop();
        try {
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_client_upload_test

TEST(Ruvia, http2_client_early_final_keeps_ordinary_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::ordinary);
}
TEST(Ruvia, http2_client_early_final_after_continue_keeps_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::continued);
}
TEST(Ruvia, http2_client_early_final_before_continue_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::rejected);
}
TEST(Ruvia, http2_client_early_final_no_error_reset_preserves_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::reset);
}
TEST(Ruvia, http2_client_early_final_abandoned_upload_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned);
}
TEST(Ruvia, http2_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned_pending);
}

TEST(Ruvia, http2_client_early_final_drains_window_blocked_buffered_upload_before_reuse) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::buffered);
}

TEST(Ruvia, http2_client_streaming_upload_drains_one_large_chunk_across_flow_control_windows) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::streaming);
}

TEST(Ruvia, http2_client_upload_transport_failure_preserves_completed_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::disconnected);
}

TEST(Ruvia, http2_client_retries_request_refused_with_refused_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused);
}

TEST(Ruvia, http2_client_retries_unsubmitted_request_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost_upload);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_goaway) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::goaway_upload);
}

TEST(Ruvia, http2_abandoned_unsubmitted_upload_retires_while_older_stream_drains) {
    http2_client_upload_test::exercise_abandon_during_drain();
}

TEST(Ruvia, http2_client_refused_request_preserves_possible_delivery) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused_always);
}
