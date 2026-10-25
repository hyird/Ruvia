#include <zlib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace response_stream_test {

struct stream_lane_observation final {
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> end_overlap_rejected_{};
    std::atomic<bool> completed_{};
    std::atomic<bool> endless_write_failed_{};
    std::atomic<int> uaf_phase_{};
    std::atomic<bool> uaf_route_done_{};
    std::atomic<bool> uaf_failed_{};
    std::atomic<std::size_t> uaf_decoded_bytes_{};
};

class stream_early_respond_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next&) {
        const auto body = co_await context_value.req().text();
        context_value.respond(context_value.text("early:" + std::string(body)));
    }
};

const auto stream_lane_observed = std::make_shared<stream_lane_observation>();
[[maybe_unused]] const bool stream_lane_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<stream_lane_observation>>([] { return stream_lane_observed; });
    return true;
}();

class stream_lane_routes final {
    RUVIA_ROUTES_BEGIN(stream_lane_routes)
    RUVIA_GET("/server-lazy-stream-output", output);
    RUVIA_GET("/server-endless-stream", endless);
    RUVIA_GET("/server-upstream-timeout-stream", upstream_timeout);
    RUVIA_GET("/server-stream-then-read-body", stream_then_read_body);
    RUVIA_GET("/server-uaf-head-stream", uaf_head_stream, ruvia::stream_body);
    RUVIA_POST("/server-stream-early-respond", never_stream, stream_early_respond_middleware);
    RUVIA_GET("/server-stream-plain", plain);
    RUVIA_ROUTES_END

    ruvia::task<void> output(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& writer = context_value.stream_text();
        std::string payload = "first";
        auto first = writer.write(payload);
        auto last = writer.writeln("second");
        auto end = writer.end();
        payload.assign("changed");
        {
            auto discarded = writer.write("discarded");
            static_cast<void>(discarded);
        }
        ruvia::task_scope writes(context_value.worker());
        const auto send = [&]() -> ruvia::task<void> {
            co_await std::move(first);
        };
        writes.spawn(send());
        try {
            auto overlap = writer.writeln("overlap");
            co_await std::move(overlap);
        } catch (const std::logic_error&) {
            observed.write_overlap_rejected_ = true;
        }
        try {
            auto overlap = writer.end();
            co_await std::move(overlap);
        } catch (const std::logic_error&) {
            observed.end_overlap_rejected_ = true;
        }
        co_await writes.join();
        co_await std::move(last);
        co_await std::move(end);
        observed.completed_ = true;
    }

    // Streams until the peer goes away, like an SSE feed a browser tab closes.
    ruvia::task<void> endless(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& writer = context_value.stream_text();
        const std::string chunk(16384, 'x');
        try {
            for (;;) {
                co_await writer.write(chunk);
            }
        } catch (...) {
            observed.endless_write_failed_ = true;
            throw;
        }
    }

    // Commits the response, then fails the way an awaited upstream request
    // that timed out does.
    ruvia::task<void> upstream_timeout(ruvia::context& context_value) {
        co_await context_value.stream_text().write("partial");
        throw std::system_error(asio::error::make_error_code(asio::error::timed_out), "upstream request");
    }

    // Starts the response before reading a body the client sent with
    // Expect: 100-continue.
    ruvia::task<void> stream_then_read_body(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        co_await writer.write("x");
        const auto body = co_await context_value.req().text();
        co_await writer.write(body);
    }

    ruvia::task<void> never_stream(ruvia::context& context_value) {
        co_await context_value.stream_text().write("never");
    }

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain-ok");
    }

    ruvia::task<void> uaf_head_stream(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& out = context_value.stream_text();
        auto& reader = context_value.req().body_reader();
        try {
            co_await out.write("head");
            observed.uaf_phase_.store(1);
            static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(25)));

            auto r1 = co_await reader.read();
            auto r2 = co_await reader.read();

            co_await out.write("discarded1");
            observed.uaf_phase_.store(2);
            static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(25)));

            auto r3 = co_await reader.read();

            co_await out.write("discarded2");
            observed.uaf_phase_.store(3);
            static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(25)));

            std::size_t total = (r1 ? r1->size() : 0) + (r2 ? r2->size() : 0) + (r3 ? r3->size() : 0);
            while (auto chunk = co_await reader.read()) {
                total += chunk->size();
            }
            observed.uaf_decoded_bytes_.store(total);
        } catch (...) {
            observed.uaf_failed_.store(true);
        }
        observed.uaf_route_done_.store(true);
    }
};

}  // namespace response_stream_test

using response_stream_test::stream_lane_observed;

// Public stream response planning rejects a head that disagrees with its status.
RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::http_response response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::created);
    auto plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http_response_stream_head(std::move(response), std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(response_stream_cold_outputs_share_a_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    stream_lane_observed->write_overlap_rejected_ = false;
    stream_lane_observed->end_overlap_rejected_ = false;
    stream_lane_observed->completed_ = false;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = server.endpoint().port(),
                                                         .request_timeout_ = std::chrono::seconds(5)});
        try {
            auto response = co_await client.handle().send({.target_ = "/server-lazy-stream-output"});
            RUVIA_CHECK(response.status() == ruvia::http_status::ok);
            auto body = co_await response.body().read_all();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), "firstsecond\n");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(stream_lane_observed->write_overlap_rejected_);
    RUVIA_CHECK(stream_lane_observed->end_overlap_rejected_);
    RUVIA_CHECK(stream_lane_observed->completed_);
}

// A peer that resets a streaming response (a closed SSE tab) ends the
// connection normally: it is neither reported nor counted as a failure.
RUVIA_TEST(response_stream_peer_reset_is_not_a_connection_failure) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    stream_lane_observed->endless_write_failed_ = false;
    const auto failures_before = ruvia::app().http_stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view request = "GET /server-endless-stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::array<char, 10> head{};
        asio::read(client, asio::buffer(head));
        RUVIA_CHECK_EQ(std::string_view(head.data(), head.size()), std::string_view("HTTP/1.1 2"));
        // SO_LINGER 0 turns close() into a reset.
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stream_lane_observed->endless_write_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.finish();
    RUVIA_CHECK(stream_lane_observed->endless_write_failed_);
    RUVIA_CHECK_EQ(ruvia::app().http_stats().connection_failures_, failures_before);
}

// A handler failure after commit is reported even when it carries a code an
// ending inbound connection also produces: an upstream timeout says nothing
// about the peer, so the truncated response is a real fault.
RUVIA_TEST(response_stream_handler_failure_with_a_transport_code_is_a_connection_failure) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().http_stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view request = "GET /server-upstream-timeout-stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        // The server closes the connection of the truncated response.
        std::string response;
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    }
    server.finish();
    RUVIA_CHECK_EQ(ruvia::app().http_stats().connection_failures_, failures_before + 1);
}

// RFC 9110 §15.2: no 1xx may follow the start of the final response. A stream
// that committed before its handler read the body already answered the
// expectation, so the body read writes no 100 Continue into the stream.
RUVIA_TEST(response_stream_body_read_after_commit_sends_no_continue) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string response;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view head =
            "GET /server-stream-then-read-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n"
            "Expect: 100-continue\r\nConnection: close\r\n\r\n";
        asio::write(client, asio::buffer(head));
        std::array<char, 1024> input{};
        std::error_code ec;
        // The body goes out only once the first chunk shows the response has
        // started, so the handler's read has to wait for it on the socket.
        while (!ec && response.find("\r\n\r\n1\r\nx\r\n") == std::string::npos) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        asio::write(client, asio::buffer(std::string_view("abcd")), ec);
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
    }
    server.finish();
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(response.find("100 Continue") == std::string::npos);
    RUVIA_CHECK(response.ends_with("\r\n\r\n1\r\nx\r\n4\r\nabcd\r\n0\r\n\r\n"));
}

RUVIA_TEST(response_stream_head_transfer_coded_chunked_body_survives_peer_monitor_reallocation) {
    const auto gzip_member = [](std::string_view input, int level) {
        z_stream zs{};
        if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::runtime_error("deflateInit2 failed");
        }
        std::string out(deflateBound(&zs, static_cast<uLong>(input.size())) + 64, '\0');
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());
        zs.next_out = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
            deflateEnd(&zs);
            throw std::runtime_error("deflate failed");
        }
        out.resize(zs.total_out);
        deflateEnd(&zs);
        return out;
    };
    const auto make_chunk = [](std::string_view data) {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%zx\r\n", data.size());
        std::string out(hex);
        out.append(data);
        out.append("\r\n");
        return out;
    };

    const std::string payload1(8000, 'X');
    const std::string payload2(100000, 'A');
    const std::string chunk1a = make_chunk(gzip_member(payload1, Z_NO_COMPRESSION));
    const std::string chunk1b = make_chunk(gzip_member(payload1, Z_NO_COMPRESSION));
    const std::string chunk2 = make_chunk(gzip_member(payload2, Z_BEST_COMPRESSION));
    const std::string chunk3a = make_chunk(gzip_member(payload1, Z_NO_COMPRESSION));
    const std::string chunk3b = make_chunk(gzip_member(payload1, Z_NO_COMPRESSION)) + "0\r\n\r\n";

    stream_lane_observed->uaf_phase_.store(0);
    stream_lane_observed->uaf_route_done_.store(false);
    stream_lane_observed->uaf_failed_.store(false);
    stream_lane_observed->uaf_decoded_bytes_.store(0);

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io,
        {.worker_count_ = 1,
            .connection_scan_interval_ = std::chrono::milliseconds(5),
            .memory_pool_ = {.retained_block_bytes_ = 0}});

    asio::ip::tcp::socket sock(io);
    sock.connect(server.endpoint());
    std::string initial_wire =
        "HEAD /server-uaf-head-stream HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    initial_wire.append(chunk1a);
    asio::write(sock, asio::buffer(initial_wire));

    std::string resp_head;
    asio::read_until(sock, asio::dynamic_buffer(resp_head), "\r\n\r\n");
    RUVIA_CHECK(resp_head.starts_with("HTTP/1.1 200 "));

    while (stream_lane_observed->uaf_phase_.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(chunk1b));

    while (stream_lane_observed->uaf_phase_.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(chunk2 + chunk3a));

    while (stream_lane_observed->uaf_phase_.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(chunk3b));

    while (!stream_lane_observed->uaf_route_done_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sock.close();
    server.finish();

    RUVIA_CHECK(!stream_lane_observed->uaf_failed_.load());
    RUVIA_CHECK_EQ(stream_lane_observed->uaf_decoded_bytes_.load(), std::size_t{4 * 8000 + 100000});
}

RUVIA_TEST(response_stream_middleware_short_circuit_after_body_read_preserves_pipelined_request) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const std::string_view wire =
        "POST /server-stream-early-respond HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        "GET /server-stream-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(client, asio::buffer(wire));
    std::string response;
    std::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);
    client.close();
    server.finish();

    RUVIA_CHECK(response.find("early:hello") != std::string::npos);
    RUVIA_CHECK(response.find("plain-ok") != std::string::npos);
}
