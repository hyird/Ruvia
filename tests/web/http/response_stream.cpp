#include <gtest/gtest.h>
#include <zlib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "app.h"
#include "content_decoding_fixture.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_multipart.h"
#include "http_response.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "middleware.h"
#include "streaming.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace response_stream_test {

struct stream_lane_observation final {
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> end_overlap_rejected_{};
    std::atomic<bool> completed_{};
    std::atomic<bool> uncommitted_write_rejected_{};
    std::atomic<bool> repeated_commit_rejected_{};
    std::atomic<int> committed_metadata_rejected_{};
    std::atomic<bool> endless_write_failed_{};
    std::atomic<int> uaf_phase_{};
    std::atomic<bool> uaf_route_done_{};
    std::atomic<bool> uaf_failed_{};
    std::atomic<std::size_t> uaf_decoded_bytes_{};
};

class stream_early_respond_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next&) {
        const auto body = co_await context_value.req().text();
        context_value.respond(context_value.text("early:" + std::string(body)));
    }
};

const auto stream_lane_observed = std::make_shared<stream_lane_observation>();
[[maybe_unused]] const bool stream_lane_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<stream_lane_observation>>([] {
        return stream_lane_observed;
    });
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
        auto draft = context_value.draft();
        auto policy = context_value.policy();
        auto selected = context_value.edit_response();
        try {
            co_await writer.write("not-committed");
        } catch (const std::logic_error&) {
            observed.uncommitted_write_rejected_ = true;
        }
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        try {
            static_cast<void>(co_await writer.commit());
        } catch (const std::logic_error&) {
            observed.repeated_commit_rejected_ = true;
        }
        for (auto editor : {draft, policy, selected}) {
            try {
                editor.header("X-Too-Late", "forbidden");
            } catch (const std::logic_error&) {
                ++observed.committed_metadata_rejected_;
            }
        }
        std::string payload{"first"};
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
        observed.completed_ = !writer.aborted() && !context_value.stop_token().stop_requested();
    }

    // Streams until the peer goes away, like an SSE feed a browser tab closes.
    ruvia::task<void> endless(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
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
        if (co_await context_value.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("partial");
        throw std::system_error(asio::error::make_error_code(asio::error::timed_out), "upstream request");
    }

    // Starts the response before reading a body the client sent with
    // Expect: 100-continue.
    ruvia::task<void> stream_then_read_body(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("x");
        const auto body = co_await context_value.req().text();
        co_await writer.write(body);
    }

    ruvia::task<void> never_stream(ruvia::context& context_value) {
        if (co_await context_value.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("never");
    }

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain-ok");
    }

    ruvia::task<void> uaf_head_stream(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& out = context_value.stream_text();
        auto& reader = context_value.req().body_reader();
        try {
            const auto phase = co_await out.commit();
            if (phase == ruvia::stream_commit_result::body) {
                co_await out.write("head");
            }
            observed.uaf_phase_.store(1);
            static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(25)));

            auto r1 = co_await reader.read();
            auto r2 = co_await reader.read();

            if (phase == ruvia::stream_commit_result::body) {
                co_await out.write("body1");
            }
            observed.uaf_phase_.store(2);
            static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(25)));

            auto r3 = co_await reader.read();

            if (phase == ruvia::stream_commit_result::body) {
                co_await out.write("body2");
            }
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
TEST(Ruvia, response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    auto plan = ruvia::plan_http_response_stream_commit(ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get, ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http_response_stream_head(std::move(response), std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, response_stream_cold_outputs_share_a_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    stream_lane_observed->write_overlap_rejected_ = false;
    stream_lane_observed->end_overlap_rejected_ = false;
    stream_lane_observed->completed_ = false;
    std::string response{};
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view request = "GET /server-lazy-stream-output HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
    }
    server.finish();
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    const auto first = response.find("first");
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(response.find("second\n", first) != std::string::npos);
    EXPECT_TRUE(stream_lane_observed->write_overlap_rejected_);
    EXPECT_TRUE(stream_lane_observed->end_overlap_rejected_);
    EXPECT_TRUE(stream_lane_observed->completed_);
    EXPECT_TRUE(stream_lane_observed->uncommitted_write_rejected_);
    EXPECT_TRUE(stream_lane_observed->repeated_commit_rejected_);
    EXPECT_EQ(stream_lane_observed->committed_metadata_rejected_.load(), 3);
}

// A peer that resets a streaming response (a closed SSE tab) ends the
// connection normally: it is neither reported nor counted as a failure.
TEST(Ruvia, response_stream_peer_reset_is_not_a_connection_failure) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    stream_lane_observed->endless_write_failed_ = false;
    const auto failures_before = ruvia::app().stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view request = "GET /server-endless-stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::array<char, 10> head{};
        asio::read(client, asio::buffer(head));
        EXPECT_EQ(std::string_view(head.data(), head.size()), std::string_view("HTTP/1.1 2"));
        // SO_LINGER 0 turns close() into a reset.
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stream_lane_observed->endless_write_failed_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    server.finish();
    EXPECT_TRUE(stream_lane_observed->endless_write_failed_);
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

// A handler failure after commit is reported even when it carries a code an
// ending inbound connection also produces: an upstream timeout says nothing
// about the peer, so the truncated response is a real fault.
TEST(Ruvia, response_stream_handler_failure_with_a_transport_code_is_a_connection_failure) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        const std::string_view request = "GET /server-upstream-timeout-stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(client, asio::buffer(request));
        // The server closes the connection of the truncated response.
        std::string response{};
        std::array<char, 1024> input{};
        std::error_code ec;
        while (!ec) {
            const auto read = client.read_some(asio::buffer(input), ec);
            response.append(input.data(), read);
        }
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    }
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before + 1);
}

// RFC 9110 §15.2: no 1xx may follow the start of the final response. A stream
// that committed before its handler read the body already answered the
// expectation, so the body read writes no 100 Continue into the stream.
TEST(Ruvia, response_stream_body_read_after_commit_sends_no_continue) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string response{};
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
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.find("100 Continue") == std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n1\r\nx\r\n4\r\nabcd\r\n0\r\n\r\n"));
}

TEST(Ruvia, response_stream_completed_head_preserves_transfer_coded_request_consumption) {
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
    ruvia::test::http_server_fixture server(io, {.worker_count_ = 1, .connection_scan_interval_ = std::chrono::milliseconds(5)});

    asio::ip::tcp::socket sock(io);
    sock.connect(server.endpoint());
    std::string initial_wire{"HEAD /server-uaf-head-stream HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"};
    initial_wire.append(chunk1a);
    asio::write(sock, asio::buffer(initial_wire));

    std::string resp_head{};
    asio::read_until(sock, asio::dynamic_buffer(resp_head), "\r\n\r\n");
    EXPECT_TRUE(resp_head.starts_with("HTTP/1.1 200 "));

    while (stream_lane_observed->uaf_phase_.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(chunk1b));

    while (stream_lane_observed->uaf_phase_.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(std::string(chunk2) + chunk3a));

    while (stream_lane_observed->uaf_phase_.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    asio::write(sock, asio::buffer(chunk3b));

    while (!stream_lane_observed->uaf_route_done_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sock.close();
    server.finish();

    EXPECT_TRUE(!stream_lane_observed->uaf_failed_.load());
    EXPECT_EQ(stream_lane_observed->uaf_decoded_bytes_.load(), std::size_t{4 * 8000 + 100000});
}

TEST(Ruvia, response_stream_middleware_short_circuit_after_body_read_preserves_pipelined_request) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const std::string_view wire =
        "POST /server-stream-early-respond HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        "GET /server-stream-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(client, asio::buffer(wire));
    std::string response{};
    std::error_code ec;
    asio::read(client, asio::dynamic_buffer(response), ec);
    client.close();
    server.finish();

    EXPECT_TRUE(response.find("early:hello") != std::string::npos);
    EXPECT_TRUE(response.find("plain-ok") != std::string::npos);
}

// File bodies are read on the blocking pool on every platform; Windows plain
// TCP sends a single range above 16 KiB with TransmitFile. Pipelined responses
// must still arrive complete and in order, whatever path each one takes.

namespace file_response_transfer_test {

namespace fs = std::filesystem;

// Below and around the kernel-transfer threshold, and above one transfer
// slice.
constexpr std::size_t small_size = 100;
constexpr std::size_t medium_size = 20 * 1024;
constexpr std::size_t large_size = 300 * 1024 + 17;

[[nodiscard]] const fs::path& transfer_dir() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-file-response-transfer");
    static const fs::path active = directory.path() / "active";
    return active;
}

[[nodiscard]] std::string file_bytes(std::size_t size, char seed) {
    std::string bytes(size, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<char>(seed + static_cast<char>(index % 23));
    }
    return bytes;
}

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::unique_ptr<ruvia::static_root> indexed_root;

// A handler that framed its file response signals `held_response_framed`,
// then holds the response until the test reports the file rewritten
// (bounded at 5 s).
std::binary_semaphore held_response_framed{0};
std::atomic<bool> rewritten{false};

class file_transfer_routes final {
    RUVIA_ROUTES_BEGIN(file_transfer_routes)
    RUVIA_GET("/file-transfer/small", small);
    RUVIA_GET("/file-transfer/medium", medium);
    RUVIA_GET("/file-transfer/large", large);
    RUVIA_GET("/file-transfer/text", text);
    RUVIA_GET("/file-transfer/indexed-small", indexed_small);
    RUVIA_GET("/file-transfer/indexed-large", indexed_large);
    RUVIA_GET("/file-transfer/held/:name", held);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        static const fs::path path = transfer_dir() / "small.bin";
        co_return co_await c.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> medium(ruvia::context& c) {
        static const fs::path path = transfer_dir() / "medium.bin";
        co_return co_await c.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        static const fs::path path = transfer_dir() / "large.bin";
        co_return co_await c.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> text(ruvia::context& c) {
        co_return c.text("between");
    }

    ruvia::task<ruvia::http_response> indexed_small(ruvia::context& c) {
        co_return co_await c.static_file(*indexed_root, {.relative_path_ = "small.bin"});
    }

    ruvia::task<ruvia::http_response> indexed_large(ruvia::context& c) {
        co_return co_await c.static_file(*indexed_root, {.relative_path_ = "large.bin"});
    }

    ruvia::task<ruvia::http_response> held(ruvia::context& c) {
        const auto path = transfer_dir() / std::string(c.req().param("name").value_or(""));
        auto response = co_await c.file({.path_ = path});
        held_response_framed.release();
        for (int wait = 0; wait < 1000 && !rewritten.load(); ++wait) {
            (void)co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(5));
        }
        co_return response;
    }
};

struct parsed_response final {
    std::string status_{};
    std::string head_{};
    std::string body_{};
};

[[nodiscard]] std::optional<std::size_t> content_length(std::string_view head) {
    constexpr std::string_view name = "\r\nContent-Length: ";
    const auto position = head.find(name);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (auto index = position + name.size(); index < head.size() && head[index] >= '0' && head[index] <= '9'; ++index) {
        value = value * 10 + static_cast<std::size_t>(head[index] - '0');
    }
    return value;
}

// Splits a stream of Content-Length framed responses. `bodyless` marks the
// responses to HEAD requests, whose Content-Length carries no body.
[[nodiscard]] std::vector<parsed_response> split_responses(std::string_view stream, std::initializer_list<bool> bodyless) {
    std::vector<parsed_response> responses{};
    while (!stream.empty() && responses.size() < bodyless.size()) {
        const auto head_end = stream.find("\r\n\r\n");
        if (head_end == std::string_view::npos) {
            break;
        }
        parsed_response response;
        response.head_.assign(stream.substr(0, head_end + 2));
        response.status_.assign(stream.substr(9, 3));
        stream.remove_prefix(head_end + 4);
        const auto length = *(bodyless.begin() + responses.size()) ? std::optional<std::size_t>(0) : content_length(response.head_);
        if (!length.has_value() || *length > stream.size()) {
            break;
        }
        response.body_.assign(stream.substr(0, *length));
        stream.remove_prefix(*length);
        responses.push_back(std::move(response));
    }
    return responses;
}

using ruvia::test::exchange;

[[nodiscard]] std::string get(std::string_view target, std::string_view extra = {}) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: localhost\r\n");
    request.append(extra);
    request.append("\r\n");
    return request;
}

}  // namespace file_response_transfer_test

using namespace file_response_transfer_test;

TEST(Ruvia, pipelined_file_responses_arrive_complete_and_in_order) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    const auto small_bytes = file_bytes(small_size, 'a');
    const auto medium_bytes = file_bytes(medium_size, 'b');
    const auto large_bytes = file_bytes(large_size, 'c');
    write_file(transfer_dir() / "small.bin", small_bytes);
    write_file(transfer_dir() / "medium.bin", medium_bytes);
    write_file(transfer_dir() / "large.bin", large_bytes);
    indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);

    std::string requests{};
    requests += get("/file-transfer/small");
    requests += get("/file-transfer/large");
    requests += get("/file-transfer/text");
    requests += get("/file-transfer/medium");
    requests += get("/file-transfer/large", "Range: bytes=10-19\r\n");
    requests += get("/file-transfer/large", "Range: bytes=100000-299999\r\n");
    requests += get("/file-transfer/medium", "Range: bytes=0-4,10-14\r\n");
    requests += "HEAD /file-transfer/large HTTP/1.1\r\nHost: localhost\r\n\r\n";
    requests += get("/file-transfer/indexed-small");
    requests += get("/file-transfer/indexed-large", "Connection: close\r\n");
    const auto stream = exchange(io, server.endpoint(), requests);
    const auto responses = split_responses(stream, {false, false, false, false, false, false, false, true, false, false});

    EXPECT_EQ(responses.size(), std::size_t{10});
    if (responses.size() == 10) {
        EXPECT_EQ(responses[0].status_, std::string("200"));
        EXPECT_TRUE(responses[0].body_ == small_bytes);
        EXPECT_EQ(responses[1].status_, std::string("200"));
        EXPECT_TRUE(responses[1].body_ == large_bytes);
        EXPECT_EQ(responses[2].body_, std::string("between"));
        EXPECT_TRUE(responses[3].body_ == medium_bytes);
        EXPECT_EQ(responses[4].status_, std::string("206"));
        EXPECT_TRUE(responses[4].body_ == std::string_view(large_bytes).substr(10, 10));
        EXPECT_EQ(responses[5].status_, std::string("206"));
        EXPECT_TRUE(responses[5].body_ == std::string_view(large_bytes).substr(100000, 200000));
        EXPECT_EQ(responses[6].status_, std::string("206"));
        EXPECT_TRUE(responses[6].head_.find("multipart/byteranges") != std::string::npos);
        EXPECT_TRUE(responses[6].body_.find(std::string_view(medium_bytes).substr(0, 5)) != std::string::npos);
        EXPECT_TRUE(responses[6].body_.find(std::string_view(medium_bytes).substr(10, 5)) != std::string::npos);
        EXPECT_EQ(responses[7].status_, std::string("200"));
        EXPECT_EQ(content_length(responses[7].head_), std::optional<std::size_t>(large_size));
        EXPECT_TRUE(responses[8].body_ == small_bytes);
        EXPECT_TRUE(responses[9].body_ == large_bytes);
    }
    // Nothing follows the last response: every body was exactly its length.
    std::size_t framed = 0;
    for (const auto& response : responses) {
        framed += response.head_.size() + 2 + response.body_.size();
    }
    EXPECT_EQ(framed, stream.size());

    server.finish();
    indexed_root.reset();
    fs::remove_all(transfer_dir(), ignored);
}

TEST(Ruvia, empty_file_responses_preserve_pipeline_framing) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "small.bin", "");
    indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string requests = get("/file-transfer/small");
    requests += get("/file-transfer/indexed-small");
    requests += "HEAD /file-transfer/small HTTP/1.1\r\nHost: localhost\r\n\r\n";
    requests += "HEAD /file-transfer/indexed-small HTTP/1.1\r\nHost: localhost\r\n\r\n";
    requests += get("/file-transfer/text", "Connection: close\r\n");
    const auto wire = exchange(io, server.endpoint(), requests);
    const auto responses = split_responses(wire, {false, false, true, true, false});
    EXPECT_EQ(responses.size(), std::size_t{5});
    if (responses.size() == 5) {
        for (std::size_t index = 0; index != 4; ++index) {
            EXPECT_EQ(responses[index].status_, std::string("200"));
            EXPECT_EQ(content_length(responses[index].head_), std::optional<std::size_t>(0));
            EXPECT_TRUE(responses[index].body_.empty());
        }
        EXPECT_EQ(responses.back().status_, std::string("200"));
        EXPECT_EQ(responses.back().body_, std::string("between"));
    }
    std::size_t framed = 0;
    for (const auto& response : responses) {
        framed += response.head_.size() + 2 + response.body_.size();
    }
    EXPECT_EQ(framed, wire.size());

    server.finish();
    indexed_root.reset();
    fs::remove_all(transfer_dir(), ignored);
}

TEST(Ruvia, changed_or_deleted_indexed_empty_file_never_commits_success) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const bool remove : {false, true}) {
        const auto path = transfer_dir() / "small.bin";
        write_file(path, "");
        indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
            ruvia::static_root_options{
                .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            });
        if (remove) {
            EXPECT_TRUE(fs::remove(path));
        } else {
            // A size change is deterministic even on coarse timestamp clocks.
            write_file(path, "changed");
        }
        const auto get_wire = exchange(io, server.endpoint(), get("/file-transfer/indexed-small", "Connection: close\r\n"));
        EXPECT_TRUE(get_wire.starts_with("HTTP/1.1 500 "));
        const auto head_wire = exchange(io, server.endpoint(), "HEAD /file-transfer/indexed-small HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(head_wire.starts_with("HTTP/1.1 500 "));
        server.finish();
        indexed_root.reset();
    }
    fs::remove_all(transfer_dir(), ignored);
}

// c.file retains the empty file's segment until its first read on the pool.
TEST(Ruvia, empty_file_changed_after_framing_does_not_commit_a_pipeline_head) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto path = transfer_dir() / "empty.bin";
    write_file(path, "");
    rewritten = false;
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto requests = get("/file-transfer/held/empty.bin") + get("/file-transfer/text", "Connection: close\r\n");
    asio::write(socket, asio::buffer(requests));
    const bool held = held_response_framed.try_acquire_for(std::chrono::seconds(5));
    EXPECT_TRUE(held);
    write_file(path, "changed");
    rewritten = true;
    std::string wire{};
    asio::read(socket, asio::dynamic_buffer(wire), ignored);
    // The stale zero-length 200 never leaves: the first response becomes an
    // error response that closes the connection, so the pipelined request
    // behind it is not answered.
    const auto responses = split_responses(wire, {false});
    EXPECT_EQ(responses.size(), std::size_t{1});
    if (responses.size() == 1) {
        EXPECT_EQ(responses.front().status_, std::string("500"));
    }
    EXPECT_TRUE(wire.find("between") == std::string::npos);
    server.finish();
    fs::remove_all(transfer_dir(), ignored);
}

TEST(Ruvia, changed_indexed_file_is_never_sent_under_a_stale_head) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "small.bin", file_bytes(small_size, 'a'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size, 'c'));
    indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
        });
    // The index framed both files at their old size and identity.
    write_file(transfer_dir() / "small.bin", file_bytes(small_size + 1, 'x'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size + 1, 'y'));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view target : {"/file-transfer/indexed-small", "/file-transfer/indexed-large"}) {
        const auto response = exchange(io, server.endpoint(), get(target, "Connection: close\r\n"));
        // The file is checked before the head is sent, so the connection
        // gets an error response instead of a 200 for bytes it cannot honour.
        EXPECT_TRUE(response.starts_with("HTTP/1.1 500 "));
    }

    server.finish();
    indexed_root.reset();
    fs::remove_all(transfer_dir(), ignored);
}

TEST(Ruvia, file_changed_in_place_after_framing_is_never_sent_under_its_head) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "small.bin", file_bytes(small_size, 'a'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size, 'c'));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const auto& [name, size] : {std::pair<std::string_view, std::size_t>{"small.bin", small_size}, std::pair<std::string_view, std::size_t>{"large.bin", large_size}}) {
        rewritten = false;
        asio::ip::tcp::socket socket(io);
        socket.connect(server.endpoint());
        const auto request = get("/file-transfer/held/" + std::string(name), "Connection: close\r\n");
        asio::write(socket, asio::buffer(request));
        // The handler framed the response from the file's current size and
        // identity and still holds it.
        const bool held = held_response_framed.try_acquire_for(std::chrono::seconds(5));
        EXPECT_TRUE(held);
        // Outlast coarse change-time clocks, then rewrite the bytes in place:
        // same size, same file, so only its change time tells them apart.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto replacement = file_bytes(size, 'x');
        {
            std::fstream output(transfer_dir() / std::string(name), std::ios::binary | std::ios::in | std::ios::out);
            output.write(replacement.data(), static_cast<std::streamsize>(replacement.size()));
        }
        rewritten = true;
        std::string response{};
        std::error_code error;
        asio::read(socket, asio::dynamic_buffer(response), error);
        // Every platform reads (or, for a kernel transfer, re-checks) the
        // framing handle before the head: the rewritten bytes never leave and
        // the client gets an error response.
        EXPECT_TRUE(response.find(std::string_view(replacement).substr(0, 64)) == std::string::npos);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 500 "));
    }

    server.finish();
    fs::remove_all(transfer_dir(), ignored);
}

#if !defined(_WIN32)
// Renaming a directory that still contains the server's open handle succeeds on
// POSIX and fails on Windows, so this identity check is POSIX-only.
TEST(Ruvia, file_response_keeps_its_opened_file_after_path_replacement) {
    std::error_code ignored;
    const auto previous_dir = transfer_dir().parent_path() / "previous";
    fs::remove_all(transfer_dir(), ignored);
    fs::remove_all(previous_dir, ignored);
    fs::create_directories(transfer_dir());
    const auto original = file_bytes(large_size, 'a');
    const auto path = transfer_dir() / "owned.bin";
    write_file(path, original);

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    rewritten = false;
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto request = get("/file-transfer/held/owned.bin", "Connection: close\r\n");
    asio::write(socket, asio::buffer(request));
    const bool held = held_response_framed.try_acquire_for(std::chrono::seconds(5));
    EXPECT_TRUE(held);
    // 移动父目录不改写文件本身的身份；原路径现在指向另一份内容。
    fs::rename(transfer_dir(), previous_dir);
    fs::create_directories(transfer_dir());
    write_file(path, file_bytes(large_size, 'x'));
    rewritten = true;
    std::string wire{};
    asio::read(socket, asio::dynamic_buffer(wire), ignored);
    const auto responses = split_responses(wire, {false});
    EXPECT_EQ(responses.size(), std::size_t{1});
    if (responses.size() == 1) {
        EXPECT_EQ(responses.front().status_, std::string("200"));
        EXPECT_TRUE(responses.front().body_ == original);
    }
    server.finish();
    fs::remove_all(transfer_dir(), ignored);
    fs::remove_all(previous_dir, ignored);
}
#endif

TEST(Ruvia, abandoned_file_response_does_not_hold_the_worker) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "large.bin", file_bytes(4 * 1024 * 1024, 'a'));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket socket(io);
    socket.open(asio::ip::tcp::v4());
    socket.set_option(asio::socket_base::receive_buffer_size(1024));
    socket.connect(server.endpoint());
    const auto request = get("/file-transfer/large");
    asio::write(socket, asio::buffer(request));
    std::array<char, 1024> prefix{};
    const auto received = socket.read_some(asio::buffer(prefix));
    EXPECT_TRUE(received != 0);
    socket.close();

    const auto wire = exchange(io, server.endpoint(), get("/file-transfer/text", "Connection: close\r\n"));
    const auto responses = split_responses(wire, {false});
    EXPECT_EQ(responses.size(), std::size_t{1});
    if (responses.size() == 1) {
        EXPECT_EQ(responses.front().body_, std::string("between"));
    }
    server.finish();
    fs::remove_all(transfer_dir(), ignored);
}

namespace ruvia::test {

struct context_body_read_observation final {
    std::string body_{};
    std::optional<ruvia::http_status_code> error_status_;
};

inline std::unique_ptr<ruvia::task<std::string_view>> make_expired_context_text_read() {
    std::unique_ptr<ruvia::task<std::string_view>> operation;
    (void)with_context(ruvia::test_request::post("/").body("body"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        operation.reset(new ruvia::task<std::string_view>(context_value.req().text()));
        co_return;
    });
    return operation;
}

inline ruvia::task<void> await_expired_context_text_read(ruvia::task<std::string_view>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline context_body_read_observation read_context_gzip_body(std::string_view encoded) {
    context_body_read_observation observation;
    (void)with_context(ruvia::test_request::post("/").header("Content-Encoding", "gzip").body(encoded), [&](ruvia::context& context_value) -> ruvia::task<void> {
        try {
            observation.body_ = co_await context_value.req().text();
        } catch (const ruvia::http_protocol_error& error) {
            observation.error_status_ = error.status();
        }
        co_return;
    });
    return observation;
}

}  // namespace ruvia::test

using namespace ruvia::test;

namespace request_body_limits_test {

struct body_lane_observation final {
    std::atomic<bool> first_read_{};
    std::atomic<bool> eof_{};
    std::atomic<bool> overlap_rejected_{};
    std::atomic<bool> multipart_eof_{};
    std::atomic<bool> multipart_overlap_rejected_{};
    std::atomic<bool> truncated_read_failed_{};
    std::atomic<bool> truncated_reread_rejected_{};
    std::atomic<bool> truncated_reread_yielded_{};
    std::atomic<unsigned> multipart_reader_parts_{};
    std::atomic<bool> multipart_rejected_{};
    std::atomic<bool> multipart_prestopped_{};
    std::atomic<bool> multipart_waiting_{};
    std::atomic<unsigned> buffered_rejected_{};
    std::atomic<unsigned> buffered_prestopped_{};
    std::atomic<bool> buffered_waiting_{};
};

const auto body_lane_observed = std::make_shared<body_lane_observation>();
[[maybe_unused]] const bool body_lane_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<body_lane_observation>>([] {
        return body_lane_observed;
    });
    return true;
}();

class body_lane_routes final {
    RUVIA_ROUTES_BEGIN(body_lane_routes)
    RUVIA_POST("/server-lazy-body-read", read, ruvia::stream_body);
    RUVIA_POST("/server-lazy-multipart-read", read_multipart, ruvia::stream_body);
    RUVIA_POST("/server-multipart-reader", read_multipart_reader, ruvia::stream_body);
    RUVIA_POST("/server-multipart-interrupted", read_interrupted_multipart, ruvia::stream_body);
    RUVIA_POST("/server-buffered-interrupted", read_interrupted_buffered);
    RUVIA_POST("/server-truncated-body-reread", reread_truncated, ruvia::stream_body);
    RUVIA_POST("/server-small-chunk-body", read_small_chunks, ruvia::stream_body);
    RUVIA_GET("/server-small-chunk-next", small_chunk_next);
    RUVIA_POST("/server-pipelined-echo", pipelined_echo);
    RUVIA_POST("/server-buffered-body/:operation", consume_buffered);
    RUVIA_POST("/server-buffered-body-failure/:operation", fail_buffered);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> read_multipart_reader(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto reader = context_value.req().multipart_reader();
        std::string body;
        while (auto part = co_await reader.read()) {
            observed.multipart_reader_parts_.fetch_add(1);
            body.append(part->body());
        }
        co_return context_value.text(std::move(body));
    }

    // A read that times out or was stopped before it started leaves the parser
    // where it was; the next read resumes the same part.
    ruvia::task<ruvia::http_response> read_interrupted_multipart(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        ruvia::multipart_reader reader(context_value.req().body_reader(), {.boundary_ = ruvia::test::make_multipart_boundary("cut")});
        try {
            (void)reader.read({.timeout_ = std::chrono::milliseconds(0)});
        } catch (const std::invalid_argument&) {
            observed.multipart_rejected_ = true;
        }
        ruvia::stop_source stopped;
        stopped.request_stop();
        try {
            (void)co_await reader.read({.stop_token_ = stopped.token()});
        } catch (const std::system_error& error) {
            observed.multipart_prestopped_ = error.code() == std::errc::operation_canceled;
        }
        std::string body;
        for (;;) {
            ruvia::operation_options operation;
            if (!observed.multipart_waiting_) {
                operation.timeout_ = std::chrono::milliseconds(100);
            }
            try {
                auto part = co_await reader.read(operation);
                if (!part) {
                    break;
                }
                body.append(part->body());
            } catch (const std::system_error& error) {
                if (error.code() != std::errc::timed_out || observed.multipart_waiting_) {
                    throw;
                }
                observed.multipart_waiting_ = true;
            }
        }
        co_return context_value.text(std::move(body));
    }

    // A buffered read that times out keeps what arrived; the next read
    // returns the whole body.
    ruvia::task<ruvia::http_response> read_interrupted_buffered(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        const auto request = context_value.req();
        try {
            (void)request.text({.timeout_ = std::chrono::milliseconds(0)});
        } catch (const std::invalid_argument&) {
            ++observed.buffered_rejected_;
        }
        ruvia::stop_source stopped;
        stopped.request_stop();
        try {
            (void)co_await request.text({.stop_token_ = stopped.token()});
        } catch (const std::system_error& error) {
            observed.buffered_prestopped_ += error.code() == std::errc::operation_canceled;
        }
        try {
            (void)co_await request.text({.timeout_ = std::chrono::milliseconds(100)});
        } catch (const std::system_error& error) {
            if (error.code() != std::errc::timed_out) {
                throw;
            }
            observed.buffered_waiting_ = true;
        }
        co_return context_value.text(co_await request.text());
    }

    ruvia::task<ruvia::http_response> read(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto& reader = context_value.req().body_reader();
        {
            auto discarded = reader.read();
            static_cast<void>(discarded);
        }
        auto first = reader.text();
        auto last = reader.read();
        ruvia::task_scope reads(context_value.worker());
        const auto receive = [&]() -> ruvia::task<void> {
            const auto chunk = co_await std::move(first);
            observed.first_read_ = chunk && *chunk == "body";
        };
        reads.spawn(receive());
        try {
            auto overlap = reader.read();
            static_cast<void>(co_await std::move(overlap));
        } catch (const std::logic_error&) {
            observed.overlap_rejected_ = true;
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_await reads.join();
        observed.eof_ = !(co_await std::move(last));
        co_return context_value.text("read-complete");
    }

    ruvia::task<ruvia::http_response> read_multipart(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        ruvia::multipart_reader reader(context_value.req().body_reader(), {.boundary_ = ruvia::test::make_multipart_boundary("lazy")});
        auto first = reader.read();
        auto last = reader.read();
        std::string body;
        ruvia::task_scope reads(context_value.worker());
        const auto receive = [&]() -> ruvia::task<void> {
            const auto part = co_await std::move(first);
            if (part) {
                body.append(part->name());
                body.push_back(':');
                body.append(part->body());
            }
        };
        reads.spawn(receive());
        try {
            auto overlap = reader.read();
            static_cast<void>(co_await std::move(overlap));
        } catch (const std::logic_error&) {
            observed.multipart_overlap_rejected_ = true;
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_await reads.join();
        auto part = co_await std::move(last);
        while (part) {
            body.append(part->body());
            part = co_await reader.read();
        }
        observed.multipart_eof_ = true;
        co_return context_value.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> reread_truncated(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto& reader = context_value.req().body_reader();
        try {
            static_cast<void>(co_await reader.read());
        } catch (const ruvia::http_protocol_error&) {
            observed.truncated_read_failed_ = true;
        }
        try {
            observed.truncated_reread_yielded_ = (co_await reader.read()).has_value();
        } catch (const std::logic_error&) {
            observed.truncated_reread_rejected_ = true;
        }
        co_return context_value.text("reread-observed");
    }

    ruvia::task<ruvia::http_response> read_small_chunks(ruvia::context& context_value) {
        auto& reader = context_value.req().body_reader();
        std::string body;
        while (const auto chunk = co_await reader.text()) {
            body.append(*chunk);
        }
        body.push_back(';');
        body.append(context_value.req().trailer("X-Sum").value_or("missing"));
        co_return context_value.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> small_chunk_next(ruvia::context& context_value) {
        co_return context_value.text("next");
    }

    ruvia::task<ruvia::http_response> pipelined_echo(ruvia::context& context_value) {
        co_return context_value.text(co_await context_value.req().text());
    }

    ruvia::task<ruvia::http_response> consume_buffered(ruvia::context& context_value) {
        const bool discard = context_value.req().param("operation") == "discard";
        std::string_view buffered;
        ruvia::task_scope consumers(context_value.worker());
        const auto consume = [&]() -> ruvia::task<void> {
            if (discard) {
                co_await context_value.req().discard_body();
            } else {
                buffered = co_await context_value.req().text();
            }
        };
        consumers.spawn(consume());
        unsigned overlap_rejected{};
        try {
            (void)co_await context_value.req().text();
        } catch (const std::logic_error&) {
            ++overlap_rejected;
        }
        try {
            co_await context_value.req().discard_body();
        } catch (const std::logic_error&) {
            ++overlap_rejected;
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_await consumers.join();
        co_await context_value.req().discard_body();
        if (discard) {
            bool reread_rejected{};
            try {
                (void)co_await context_value.req().text();
            } catch (const std::logic_error&) {
                reread_rejected = true;
            }
            co_return context_value.text(overlap_rejected == 2 && reread_rejected ? "discarded" : "invalid-discard");
        }
        const auto retained = co_await context_value.req().text();
        co_return context_value.text(overlap_rejected == 2 && retained == buffered ? retained : std::string_view("invalid-read"));
    }

    ruvia::task<ruvia::http_response> fail_buffered(ruvia::context& context_value) {
        bool failed{};
        try {
            if (context_value.req().param("operation") == "discard") {
                co_await context_value.req().discard_body();
            } else {
                (void)co_await context_value.req().text();
            }
        } catch (const ruvia::http_protocol_error&) {
            failed = true;
        }
        unsigned rejected{};
        try {
            (void)co_await context_value.req().text();
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            co_await context_value.req().discard_body();
        } catch (const std::logic_error&) {
            ++rejected;
        }
        co_return context_value.text(failed && rejected == 2 ? "failed" : "invalid-failure");
    }
};

void exercise_lazy_body_reads(std::string_view target, std::string_view body, std::string_view expected) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const std::string head = "POST " + std::string(target) + std::move(std::string(" HTTP/1.1\r\nHost: localhost\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nConnection: close\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(head), std::move(handler));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::string informational{};
            char byte{};
            while (!informational.ends_with("\r\n\r\n")) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(&byte, 1), std::move(handler));
                });
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                informational.push_back(byte);
            }
            EXPECT_TRUE(informational.starts_with("HTTP/1.1 103 "));
            const auto body_sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(body), std::move(handler));
            });
            if (body_sent.error_code()) {
                throw std::system_error(body_sent.error_code());
            }
            std::string response{};
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
            EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
            EXPECT_TRUE(response.ends_with(expected));
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    std::move(done).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Writes `head`, waits for `100 Continue` when `await_continue` is set, writes
// `body`, half-closes when `shutdown_send` is set, and returns every byte the
// server sends until it closes the connection.
std::string exchange_raw_request(std::string_view head, std::string_view body, bool await_continue, bool shutdown_send) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::string response{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const auto write_all = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(client, asio::buffer(bytes), std::move(handler));
                });
                if (sent.error_code()) {
                    throw std::system_error(sent.error_code());
                }
            };
            co_await write_all(head);
            if (await_continue) {
                std::string informational{};
                char byte{};
                while (!informational.ends_with("\r\n\r\n")) {
                    const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        client.async_read_some(asio::buffer(&byte, 1), std::move(handler));
                    });
                    if (received.error_code()) {
                        throw std::system_error(received.error_code());
                    }
                    informational.push_back(byte);
                }
                if (!informational.starts_with("HTTP/1.1 100 ")) {
                    throw std::runtime_error("expected 100 Continue before the request body");
                }
            }
            co_await write_all(body);
            if (shutdown_send) {
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    std::move(done).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return response;
}

}  // namespace request_body_limits_test

using request_body_limits_test::body_lane_observed;
using request_body_limits_test::exchange_raw_request;
using request_body_limits_test::exercise_lazy_body_reads;

TEST(Ruvia, web_request_decode_accepts_gzip_content) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzip_compress(plain);
    EXPECT_TRUE(!encoded.empty());
    const auto observation_value = read_context_gzip_body(encoded);
    EXPECT_TRUE(!observation_value.error_status_.has_value());
    EXPECT_EQ(observation_value.body_, plain);
}

TEST(Ruvia, web_request_decode_accepts_empty_encoded_representation) {
    const auto observation_value = read_context_gzip_body({});
    EXPECT_TRUE(!observation_value.error_status_.has_value());
    EXPECT_TRUE(observation_value.body_.empty());
}

TEST(Ruvia, context_request_cold_operation_rejects_after_request_scope_closes) {
    auto operation = make_expired_context_text_read();
    bool rejected = false;
    asio::io_context io(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(await_expired_context_text_read(*operation, rejected)), asio::use_future);
    io.run();
    future.get();
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, streamed_request_cold_reads_share_a_lane_only_when_started) {
    body_lane_observed->first_read_ = false;
    body_lane_observed->eof_ = false;
    body_lane_observed->overlap_rejected_ = false;
    exercise_lazy_body_reads("/server-lazy-body-read", "body", "read-complete");
    EXPECT_TRUE(body_lane_observed->first_read_);
    EXPECT_TRUE(body_lane_observed->eof_);
    EXPECT_TRUE(body_lane_observed->overlap_rejected_);
}

TEST(Ruvia, multipart_request_cold_reads_preserve_parser_state_after_started_overlap) {
    body_lane_observed->multipart_eof_ = false;
    body_lane_observed->multipart_overlap_rejected_ = false;
    constexpr std::string_view body = "--lazy\r\nContent-Disposition: form-data; name=field\r\n\r\nvalue\r\n--lazy--\r\n";
    exercise_lazy_body_reads("/server-lazy-multipart-read", body, "field:value");
    EXPECT_TRUE(body_lane_observed->multipart_eof_);
    EXPECT_TRUE(body_lane_observed->multipart_overlap_rejected_);
}

TEST(Ruvia, multipart_read_interrupted_by_its_operation_resumes_parsing) {
    body_lane_observed->multipart_rejected_ = false;
    body_lane_observed->multipart_prestopped_ = false;
    body_lane_observed->multipart_waiting_ = false;
    constexpr std::string_view first = "--cut\r\nContent-Disposition: form-data; name=field\r\n\r\nval";
    constexpr std::string_view rest = "ue\r\n--cut--\r\n";
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string request =
        "POST /server-multipart-interrupted HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
        "Content-Length: " +
        std::to_string(first.size() + rest.size()) + "\r\n\r\n";
    request.append(first);
    std::string response;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        asio::write(client, asio::buffer(request));
        ruvia::test::wait_until([] {
            return body_lane_observed->multipart_waiting_.load();
        }, std::chrono::seconds(5));
        asio::write(client, asio::buffer(rest));
        std::array<char, 1024> buffer;
        asio::error_code error;
        while (!error) {
            const auto count = client.read_some(asio::buffer(buffer), error);
            response.append(buffer.data(), count);
        }
    }
    server.finish();
    EXPECT_TRUE(body_lane_observed->multipart_rejected_);
    EXPECT_TRUE(body_lane_observed->multipart_prestopped_);
    EXPECT_TRUE(body_lane_observed->multipart_waiting_);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.ends_with("\r\n\r\nvalue"));
}

TEST(Ruvia, buffered_body_read_interrupted_by_its_operation_resumes) {
    for (const bool chunked : {false, true}) {
        SCOPED_TRACE(chunked ? "chunked" : "known length");
        body_lane_observed->buffered_rejected_ = 0;
        body_lane_observed->buffered_prestopped_ = 0;
        body_lane_observed->buffered_waiting_ = false;
        std::string request = "POST /server-buffered-interrupted HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
        std::string first;
        std::string rest;
        if (chunked) {
            request.append("Transfer-Encoding: chunked\r\n\r\n");
            first = "5\r\nhello\r\n";
            rest = "5\r\nworld\r\n0\r\n\r\n";
        } else {
            request.append("Content-Length: 10\r\n\r\n");
            first = "hello";
            rest = "world";
        }
        request.append(first);
        auto& io = ruvia::test::new_test_io_context();
        ruvia::test::http_server_fixture server(io);
        std::string response;
        {
            asio::ip::tcp::socket client(io);
            client.connect(server.endpoint());
            asio::write(client, asio::buffer(request));
            ruvia::test::wait_until([] {
                return body_lane_observed->buffered_waiting_.load();
            }, std::chrono::seconds(5));
            asio::write(client, asio::buffer(rest));
            std::array<char, 1024> buffer;
            asio::error_code error;
            while (!error) {
                const auto count = client.read_some(asio::buffer(buffer), error);
                response.append(buffer.data(), count);
            }
        }
        server.finish();
        EXPECT_EQ(body_lane_observed->buffered_rejected_.load(), 1U);
        EXPECT_EQ(body_lane_observed->buffered_prestopped_.load(), 1U);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("\r\n\r\nhelloworld"));
    }
}

TEST(Ruvia, streamed_request_body_stays_failed_after_truncated_read) {
    body_lane_observed->truncated_read_failed_ = false;
    body_lane_observed->truncated_reread_rejected_ = false;
    body_lane_observed->truncated_reread_yielded_ = false;
    const auto response = exchange_raw_request("POST /server-truncated-body-reread HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n", {}, false, true);
    EXPECT_TRUE(body_lane_observed->truncated_read_failed_);
    EXPECT_TRUE(body_lane_observed->truncated_reread_rejected_);
    EXPECT_TRUE(!body_lane_observed->truncated_reread_yielded_);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    // The body was never consumed, so the connection cannot be reused.
    EXPECT_TRUE(response.find("\r\nConnection: close\r\n") != std::string::npos);
    EXPECT_TRUE(response.ends_with("reread-observed"));
}

TEST(Ruvia, buffered_body_read_excludes_discard_and_retains_completed_content) {
    exercise_lazy_body_reads("/server-buffered-body/read", "retained-body", "retained-body");
}

TEST(Ruvia, buffered_body_discard_excludes_read_and_permanently_rejects_content_reads) {
    exercise_lazy_body_reads("/server-buffered-body/discard", "discarded-body", "discarded");
}

TEST(Ruvia, buffered_body_consumption_failure_rejects_both_read_and_discard) {
    for (const std::string_view operation : {"read", "discard"}) {
        SCOPED_TRACE(operation);
        std::string head("POST /server-buffered-body-failure/");
        head.append(operation).append(" HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n");
        const auto response = exchange_raw_request(head, {}, false, true);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("\r\n\r\nfailed"));
    }
}

TEST(Ruvia, streamed_chunked_body_of_small_chunks_keeps_trailers_and_pipeline) {
    // The body follows 100 Continue, so every chunk arrives through socket
    // reads after the head, with chunks split across read boundaries.
    constexpr std::size_t chunk_count = 3000;
    std::string expected{};
    std::string body{};
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const auto value = static_cast<char>('a' + index % 26);
        expected.push_back(value);
        body.append("1\r\n");
        body.push_back(value);
        body.append("\r\n");
    }
    body.append("0\r\nX-Sum: ok\r\n\r\n");
    body.append("GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto response = exchange_raw_request(
        "POST /server-small-chunk-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
        "Expect: 100-continue\r\n\r\n",
        body, true, false);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    const auto first_end = response.find(std::string(expected) + ";ok");
    EXPECT_TRUE(first_end != std::string::npos);
    const auto second = response.find("HTTP/1.1 200 ", first_end);
    EXPECT_TRUE(second != std::string::npos);
    EXPECT_TRUE(response.ends_with("next"));
}

TEST(Ruvia, buffered_routes_answer_a_pipelined_burst_in_order) {
    // One write carries every request, so each buffered-route response is
    // prepared while the next requests still sit behind it in the read buffer,
    // and the echoed bodies borrow that buffer until the response owns them.
    const auto response = exchange_raw_request(
        "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\nfirst!"
        "POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 7\r\n\r\nsecond!"
        "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        {}, false, false);
    std::size_t position = 0;
    for (const std::string_view body : {"next", "first!", "second!", "next"}) {
        position = response.find("HTTP/1.1 200 ", position);
        EXPECT_TRUE(position != std::string::npos);
        if (position == std::string::npos) {
            return;
        }
        position = response.find("\r\n\r\n", position);
        EXPECT_TRUE(position != std::string::npos);
        if (position == std::string::npos) {
            return;
        }
        position += 4;
        EXPECT_EQ(std::string_view(response).substr(position, body.size()), body);
    }
    EXPECT_EQ(position + 4, response.size());
}

TEST(Ruvia, buffered_known_length_body_read_from_socket_keeps_the_pipelined_request) {
    // The body follows 100 Continue, so all of it arrives through socket reads
    // after the head, across several reads and capacity growth steps, with the
    // next request directly behind its last byte in the same write.
    std::string body(200 * 1024 + 7, '\0');
    for (std::size_t index = 0; index < body.size(); ++index) {
        body[index] = static_cast<char>('a' + index % 23);
    }
    const auto response = exchange_raw_request(std::move(std::string("POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nExpect: 100-continue\r\n\r\n", std::string(body) + "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", true, false);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    const auto echoed = response.find("\r\n\r\n");
    EXPECT_TRUE(echoed != std::string::npos);
    if (echoed == std::string::npos) {
        return;
    }
    EXPECT_TRUE(std::string_view(response).substr(echoed + 4, body.size()) == body);
    const auto second = response.find("HTTP/1.1 200 ", echoed + 4 + body.size());
    EXPECT_EQ(second, echoed + 4 + body.size());
    EXPECT_TRUE(response.ends_with("next"));
}

TEST(Ruvia, server_rejects_absolute_form_targets_outside_http_schemes) {
    // A front proxy that only recognises http(s) absolute URIs must not see a
    // different target than the one routed here, so other schemes are a 400.
    for (const std::string_view head : {"GET ftp://evil/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET javascript:/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n", "GET x:80 HTTP/1.1\r\nHost: localhost\r\n\r\n"}) {
        const auto response = exchange_raw_request(head, {}, false, false);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 400 "));
        EXPECT_TRUE(response.find("next") == std::string::npos);
    }
    const auto accepted = exchange_raw_request("GET http://localhost/server-small-chunk-next HTTP/1.1\r\nHost: other\r\nConnection: close\r\n\r\n", {}, false, false);
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(accepted.ends_with("next"));
}

TEST(Ruvia, web_request_decodes_content_coding_stacks_in_reverse_order) {
    const std::string plain(8192, 'm');
    // Applied in Content-Encoding order: gzip first, then deflate.
    const auto gzipped = ruvia::encode_http_content(ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(gzipped.has_value());
    if (!gzipped.has_value()) {
        return;
    }
    const auto encoded = ruvia::encode_http_content(ruvia::http_content_coding::deflate, *gzipped, {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(encoded.has_value());
    if (!encoded.has_value()) {
        return;
    }
    (void)ruvia::test::with_context(ruvia::test_request::post("/").header("Content-Encoding", "gzip, deflate").body(*encoded), [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto body = co_await context_value.req().text();
        EXPECT_EQ(body, std::string_view(plain));
    });
}

// The streaming multipart reader decodes no content coding, so it refuses a
// coded body before any part reaches the handler, advertising only identity
// (RFC 9110 §15.5.16); a malformed Content-Encoding field is a 400.
TEST(Ruvia, streamed_multipart_reader_refuses_content_coded_bodies) {
    constexpr std::string_view body = "--lazy\r\nContent-Disposition: form-data; name=field\r\n\r\nvalue\r\n--lazy--\r\n";
    const auto exchange = [](std::string_view coding_field, std::string_view payload) {
        const std::string head =
            "POST /server-multipart-reader HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: multipart/form-data; boundary=lazy\r\n" +
            std::string(coding_field) + std::move(std::string("Content-Length: ").append(std::to_string(payload.size()))) + "\r\nConnection: close\r\n\r\n";
        return exchange_raw_request(head, payload, false, false);
    };
    body_lane_observed->multipart_reader_parts_ = 0;
    const auto identity = exchange("Content-Encoding: identity\r\n", body);
    EXPECT_TRUE(identity.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(identity.ends_with("\r\n\r\nvalue"));
    EXPECT_EQ(body_lane_observed->multipart_reader_parts_.load(), 1U);

    const auto gzip = exchange("Content-Encoding: gzip\r\n", gzip_compress(std::string(body)));
    EXPECT_TRUE(gzip.starts_with("HTTP/1.1 415 "));
    EXPECT_TRUE(gzip.find("\r\nAccept-Encoding: identity\r\n") != std::string::npos);
    const auto unknown = exchange("Content-Encoding: x-unknown\r\n", body);
    EXPECT_TRUE(unknown.starts_with("HTTP/1.1 415 "));
    EXPECT_TRUE(unknown.find("\r\nAccept-Encoding: identity\r\n") != std::string::npos);
    EXPECT_TRUE(exchange("Content-Encoding: gzip;level=9\r\n", body).starts_with("HTTP/1.1 400 "));
    EXPECT_EQ(body_lane_observed->multipart_reader_parts_.load(), 1U);
}
