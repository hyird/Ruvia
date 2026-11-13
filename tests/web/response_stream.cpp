#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "content_decoding_fixture.h"
#include "http_server_fixture.h"
#include "worker_task_fixture.h"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#include <charconv>
#include <future>
#include <iterator>
#include <new>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/streambuf.hpp>

#include "ruvia/core/task.h"
#include "ruvia/web/model.h"

#include "worker_task_fixture.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

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
    auto plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
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
        const std::string_view request =
            "GET /server-lazy-stream-output HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
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
    ruvia::test::http_server_fixture server(io,
        {.worker_count_ = 1,
            .connection_scan_interval_ = std::chrono::milliseconds(5)});

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
    for (auto index = position + name.size(); index < head.size() && head[index] >= '0' && head[index] <= '9';
        ++index) {
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
    const auto responses =
        split_responses(stream, {false, false, false, false, false, false, false, true, false, false});

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
        const auto get_wire = exchange(
            io, server.endpoint(), get("/file-transfer/indexed-small", "Connection: close\r\n"));
        EXPECT_TRUE(get_wire.starts_with("HTTP/1.1 500 "));
        const auto head_wire = exchange(io, server.endpoint(),
            "HEAD /file-transfer/indexed-small HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
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
    const auto requests = get("/file-transfer/held/empty.bin") +
                          get("/file-transfer/text", "Connection: close\r\n");
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
    for (const auto& [name, size] : {std::pair<std::string_view, std::size_t>{"small.bin", small_size},
             std::pair<std::string_view, std::size_t>{"large.bin", large_size}}) {
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
    (void)with_context(ruvia::test_request::post("/").body("body"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            operation.reset(new ruvia::task<std::string_view>(context_value.req().text()));
            co_return;
        });
    return operation;
}

inline ruvia::task<void> await_expired_context_text_read(
    ruvia::task<std::string_view>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline context_body_read_observation read_context_gzip_body(std::string_view encoded) {
    context_body_read_observation observation;
    (void)with_context(
        ruvia::test_request::post("/").header("Content-Encoding", "gzip").body(encoded),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
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
    ruvia::app().use_worker_state<std::shared_ptr<body_lane_observation>>([] { return body_lane_observed; });
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
        ruvia::multipart_reader reader(context_value.req().body_reader(),
            {.boundary_ = ruvia::test::make_multipart_boundary("cut")});
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
        ruvia::multipart_reader reader(context_value.req().body_reader(),
            {.boundary_ = ruvia::test::make_multipart_boundary("lazy")});
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

void exercise_lazy_body_reads(
    std::string_view target, std::string_view body, std::string_view expected) {
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
std::string exchange_raw_request(
    std::string_view head, std::string_view body, bool await_continue, bool shutdown_send) {
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
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(await_expired_context_text_read(*operation, rejected)),
        asio::use_future);
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
        ruvia::test::wait_until([] { return body_lane_observed->multipart_waiting_.load(); }, std::chrono::seconds(5));
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
            ruvia::test::wait_until([] { return body_lane_observed->buffered_waiting_.load(); }, std::chrono::seconds(5));
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
    const auto response = exchange_raw_request(
        "POST /server-truncated-body-reread HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n", {},
        false, true);
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
    const auto response = exchange_raw_request(
        std::move(std::string("POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: ").append(std::to_string(body.size()))) +
            "\r\nExpect: 100-continue\r\n\r\n",
        std::string(body) + "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", true, false);
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
    for (const std::string_view head : {
             "GET ftp://evil/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n",
             "GET javascript:/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n",
             "GET x:80 HTTP/1.1\r\nHost: localhost\r\n\r\n"}) {
        const auto response = exchange_raw_request(head, {}, false, false);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 400 "));
        EXPECT_TRUE(response.find("next") == std::string::npos);
    }
    const auto accepted = exchange_raw_request(
        "GET http://localhost/server-small-chunk-next HTTP/1.1\r\nHost: other\r\nConnection: close\r\n\r\n", {},
        false, false);
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(accepted.ends_with("next"));
}

TEST(Ruvia, web_request_decodes_content_coding_stacks_in_reverse_order) {
    const std::string plain(8192, 'm');
    // Applied in Content-Encoding order: gzip first, then deflate.
    const auto gzipped = ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(gzipped.has_value());
    if (!gzipped.has_value()) {
        return;
    }
    const auto encoded = ruvia::encode_http_content(
        ruvia::http_content_coding::deflate, *gzipped, {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(encoded.has_value());
    if (!encoded.has_value()) {
        return;
    }
    (void)ruvia::test::with_context(
        ruvia::test_request::post("/").header("Content-Encoding", "gzip, deflate").body(*encoded),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
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
            std::string(coding_field) + std::move(std::string("Content-Length: ").append(std::to_string(payload.size()))) +
            "\r\nConnection: close\r\n\r\n";
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

// Pipelined HTTP/1.1 requests: the server may coalesce the responses of
// requests already buffered behind each other, but every response still
// leaves in request order, whatever kind it is, and nothing waits on input the
// client has not sent yet.

namespace http1_pipelining_test {

namespace fs = std::filesystem;

[[nodiscard]] const fs::path& pipelined_file_path() {
    static const ruvia::test::scoped_temp_directory directory("ruvia_http1_pipelining");
    static const fs::path path = directory.path() / "file.txt";
    return path;
}

inline constexpr std::size_t long_hint_value_bytes = 2000;

class pipelining_routes final {
    RUVIA_ROUTES_BEGIN(pipelining_routes)
    RUVIA_GET("/pipelining/item/:id", item);
    RUVIA_GET("/pipelining/large", large);
    RUVIA_GET("/pipelining/hint", hint);
    RUVIA_GET("/pipelining/long-hint", long_hint);
    RUVIA_GET("/pipelining/too-many-fields", too_many_fields);
    RUVIA_GET("/pipelining/file", file);
    RUVIA_POST("/pipelining/echo", echo);
    RUVIA_GET("/pipelining/stream", stream);
    RUVIA_GET("/pipelining/stream-echo", stream_echo);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        const auto body = "item-" + std::string(c.req().param("id").value_or(""));
        co_return c.text(std::string_view(body));
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        // Larger than one coalesced write, so it is written with the
        // responses held before it rather than held itself.
        const auto body = std::string(64 * 1024, 'L') + "large-end";
        co_return c.text(std::string_view(body));
    }

    ruvia::task<ruvia::http_response> hint(ruvia::context& c) {
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_return c.text("hinted");
    }

    ruvia::task<ruvia::http_response> long_hint(ruvia::context& c) {
        // Longer than an interim head usually is, so it does not fit the
        // head buffer's inline storage.
        const std::string value(long_hint_value_bytes, 'h');
        const ruvia::http_header_view headers[] = {{"X-Hint", value}};
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
        co_return c.text("long-hinted");
    }

    // More header bytes than an HTTP/1 response head may carry, so its head
    // cannot be serialized.
    ruvia::task<ruvia::http_response> too_many_fields(ruvia::context& c) {
        auto response = c.text("unsendable");
        const std::string value(1024, 'v');
        for (int index = 0; index < 70; ++index) {
            response.header("X-Field-" + std::string(std::to_string(index)), value);
        }
        co_return response;
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        co_return co_await c.file({.path_ = pipelined_file_path()});
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto first = co_await c.req().text();
        const auto second = co_await c.req().text();
        if (first != second) {
            co_return c.text("mismatch");
        }
        const auto body = "echo-" + std::string(second);
        co_return c.text(std::string_view(body));
    }

    ruvia::task<void> stream(ruvia::context& c) {
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("streamed-chunk");
    }

    ruvia::task<void> stream_echo(ruvia::context& c) {
        const auto body = "stream-echo-" + std::string(co_await c.req().text());
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write(body);
    }
};

// Writes `requests`, then reads until the server closes; nullopt when it is
// still open after 10 s.
[[nodiscard]] std::optional<std::string> exchange_pipelined(
    const asio::ip::tcp::endpoint& endpoint, std::string_view requests) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(requests));
    std::string output{};
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&closed](const std::error_code&, std::size_t) {
        closed = true;
    });
    io.run_for(std::chrono::seconds(10));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

// Checks that every marker occurs in `output`, each after the previous one.
[[nodiscard]] bool appear_in_order(std::string_view output, std::initializer_list<std::string_view> markers) {
    std::size_t position = 0;
    for (const auto marker : markers) {
        const auto found = output.find(marker, position);
        if (found == std::string_view::npos) {
            return false;
        }
        position = found + marker.size();
    }
    return true;
}

[[nodiscard]] std::size_t count_occurrences(std::string_view output, std::string_view marker) {
    std::size_t count = 0;
    for (auto position = output.find(marker); position != std::string_view::npos;
        position = output.find(marker, position + marker.size())) {
        ++count;
    }
    return count;
}

}  // namespace http1_pipelining_test

using namespace http1_pipelining_test;

TEST(Ruvia, pipelined_responses_of_every_kind_leave_in_request_order) {
    {
        std::ofstream output(pipelined_file_path(), std::ios::binary | std::ios::trunc);
        output << "file-body";
    }
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string requests{};
    requests += get("/pipelining/item/1");
    requests += get("/pipelining/item/2");
    requests += "HEAD /pipelining/item/3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    requests += get("/pipelining/item/4");
    requests += get("/pipelining/large");
    requests += get("/pipelining/item/5");
    requests += get("/pipelining/stream");
    requests += get("/pipelining/item/6");
    requests += "POST /pipelining/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nbody";
    requests += get("/pipelining/item/7");
    requests += get("/pipelining/hint");
    requests += get("/pipelining/item/8");
    requests += get("/pipelining/file");
    requests += get("/pipelining/item/9");
    requests += get("/pipelining/missing");
    requests += get("/pipelining/item/10", "Connection: close\r\n");
    const auto response = exchange_pipelined(server.endpoint(), requests);
    server.finish();

    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    EXPECT_TRUE(output.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(appear_in_order(output,
        {"item-1", "item-2", "HTTP/1.1 200 ", "item-4", "large-end", "item-5", "streamed-chunk", "item-6",
            "echo-body", "item-7", "HTTP/1.1 103 ", "hinted", "item-8", "file-body", "item-9", "HTTP/1.1 404 ",
            "item-10"}));
    // The HEAD response carries no body.
    EXPECT_TRUE(output.find("item-3") == std::string::npos);
    // Exactly one final response per request, plus the one interim head.
    EXPECT_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{17});
    EXPECT_TRUE(output.ends_with("item-10"));
}

TEST(Ruvia, a_long_pipelined_burst_is_answered_completely_and_in_order) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    constexpr std::size_t request_count = 400;
    std::string requests{};
    for (std::size_t index = 0; index < request_count; ++index) {
        requests += get("/pipelining/item/" + std::string(std::to_string(index)),
            index + 1 == request_count ? std::string_view("Connection: close\r\n") : std::string_view{});
    }
    const auto response = exchange_pipelined(server.endpoint(), requests);
    server.finish();

    EXPECT_TRUE(response.has_value());
    const auto text = (response ? std::string_view(*response) : std::string_view{});
    const std::string_view output = text;
    EXPECT_EQ(count_occurrences(output, "HTTP/1.1 200 "), request_count);
    std::size_t position = 0;
    bool ordered = true;
    for (std::size_t index = 0; index < request_count && ordered; ++index) {
        const auto body = "item-" + std::string(std::to_string(index));
        const auto head_end = output.find("\r\n\r\n", position);
        ordered = head_end != std::string_view::npos && output.substr(head_end + 4, body.size()) == body;
        position = head_end + 4 + body.size();
    }
    EXPECT_TRUE(ordered);
    EXPECT_EQ(position, output.size());
}

TEST(Ruvia, a_long_interim_head_is_sent_whole_between_pipelined_responses) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto response = exchange_pipelined(server.endpoint(),
        get("/pipelining/item/before") + get("/pipelining/long-hint") +
            get("/pipelining/item/after", "Connection: close\r\n"));
    server.finish();

    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    const auto hint_field = "X-Hint: " + std::string(long_hint_value_bytes, 'h') + "\r\n";
    EXPECT_TRUE(appear_in_order(
        output, {"item-before", "HTTP/1.1 103 ", hint_field, "\r\n\r\n", "long-hinted", "item-after"}));
    EXPECT_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{4});
}

TEST(Ruvia, an_unserializable_pipelined_response_becomes_a_500_after_the_held_ones) {
    // The item response is held for a coalesced write when the next head
    // fails to serialize. The held response still leaves first, the failed
    // one is answered with the generic error response, and the connection
    // closes without that counting as a connection failure.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto failures_before = ruvia::app().stats().connection_failures_;
    const auto response = exchange_pipelined(server.endpoint(),
        get("/pipelining/item/before") + get("/pipelining/too-many-fields") + get("/pipelining/item/after"));
    server.finish();

    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    EXPECT_TRUE(appear_in_order(output, {"HTTP/1.1 200 ", "item-before", "HTTP/1.1 500 "}));
    EXPECT_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{2});
    EXPECT_TRUE(output.find("unsendable") == std::string::npos);
    EXPECT_TRUE(output.find("item-after") == std::string::npos);
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, a_stream_route_that_consumed_its_request_body_keeps_the_connection) {
    // The body bytes are read before the stream head commits, so nothing of
    // the request is left in the socket and the pipelined request after it
    // is served on the same connection.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto response = exchange_pipelined(server.endpoint(),
        "GET /pipelining/stream-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nbody" +
            get("/pipelining/item/after", "Connection: close\r\n"));
    server.finish();

    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    EXPECT_TRUE(appear_in_order(output, {"HTTP/1.1 200 ", "stream-echo-body", "HTTP/1.1 200 ", "item-after"}));
    EXPECT_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{2});
    EXPECT_TRUE(output.ends_with("item-after"));
}

TEST(Ruvia, held_pipelined_responses_are_sent_before_waiting_for_more_input) {
    // The burst ends inside the next request head: the server must deliver
    // what it has answered before it waits for the rest of that head, or a
    // client that sends the rest only after reading would wait forever.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto first = get("/pipelining/item/first") + get("/pipelining/item/second") + "GET /pipelining/item/third";
    asio::write(socket, asio::buffer(first));
    std::string output{};
    bool answered = false;
    asio::async_read_until(socket, asio::dynamic_buffer(output), "item-second",
        [&answered](const std::error_code& error, std::size_t) { answered = !error; });
    io.run_for(std::chrono::seconds(10));
    EXPECT_TRUE(answered);
    EXPECT_TRUE(appear_in_order(output, {"item-first", "item-second"}));
    EXPECT_TRUE(output.find("item-third") == std::string::npos);

    io.restart();
    asio::write(socket, asio::buffer(std::string_view(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&closed](const std::error_code&, std::size_t) {
        closed = true;
    });
    io.run_for(std::chrono::seconds(10));
    EXPECT_TRUE(closed);
    EXPECT_TRUE(output.ends_with("item-third"));
    server.finish();
}

TEST(Ruvia, a_stalled_reader_does_not_block_other_connections_on_its_worker) {
    // The test server has one worker. A client that never reads makes the
    // server's non-blocking write fall back to an asynchronous one; a
    // blocking write would stall the worker and starve the second client.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket stalled(io);
    stalled.connect(server.endpoint());
    stalled.set_option(asio::socket_base::receive_buffer_size(4096));
    std::string stalled_requests{};
    for (int index = 0; index < 64; ++index) {
        stalled_requests += get("/pipelining/large");
    }
    asio::write(stalled, asio::buffer(stalled_requests));

    const auto response =
        exchange_pipelined(server.endpoint(), get("/pipelining/item/live", "Connection: close\r\n"));
    EXPECT_TRUE(response.has_value());
    EXPECT_TRUE((response ? std::string_view(*response) : std::string_view{}).ends_with("item-live"));

    std::error_code ignored;
    stalled.close(ignored);
    server.finish();
}

#if defined(RUVIA_ENABLE_HTTP2)

namespace {
void exercise_client(bool reset_peer, bool secure = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::optional<ruvia::test::tls_identity> identity;
    if (secure) {
        identity.emplace("localhost");
        SSL_CTX_set_alpn_select_cb(identity->context_.native_handle(), [](SSL*, const unsigned char** selected, unsigned char* selected_size, const unsigned char* offered, unsigned int offered_size, void*) {
                static constexpr unsigned char protocols[]{2, 'h', '2'};
                unsigned char* protocol = nullptr;
                if (SSL_select_next_proto(&protocol, selected_size, protocols, sizeof(protocols),
                        offered, offered_size) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *selected = protocol;
                return SSL_TLSEXT_ERR_OK; }, nullptr);
    }
    std::exception_ptr peer_failure;
    bool saw_extended_connect = false;
    bool saw_client_end = false;
    unsigned messages = 0;
    const std::string large(2200000, 'a');
    const auto serve_stream = [&](auto& socket) -> asio::awaitable<void> {
        auto connection = ruvia::http2_connection::server();
        ruvia::http_io_buffer websocket_input;
        std::optional<ruvia::websocket_connection> websocket;
        std::optional<ruvia::http2_request_head_event> request_lease;
        std::string outbound{};
        bool finish_value = false;
        bool ended = false;
        std::array<char, 16384> bytes_value{};
        std::string wire;
        for (;;) {
            while (!outbound.empty()) {
                const auto count = std::min<std::size_t>(outbound.size(), 16384);
                const auto result_value = connection.submit_data(1, std::string_view(outbound.data(), count), ruvia::http2_end_stream::keep_open, count);
                EXPECT_TRUE(result_value.status_ == ruvia::http2_data_submit_status::complete ||
                            result_value.status_ == ruvia::http2_data_submit_status::backpressured);
                outbound.erase(0, result_value.consumed_bytes_);
                if (result_value.status_ != ruvia::http2_data_submit_status::complete) {
                    break;
                }
            }
            if (finish_value && outbound.empty() && !ended) {
                const auto result_value = connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 0);
                EXPECT_TRUE(result_value.status_ == ruvia::http2_data_submit_status::complete);
                EXPECT_EQ(result_value.consumed_bytes_, std::size_t{0});
                ended = result_value.status_ == ruvia::http2_data_submit_status::complete;
            }
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, wire);
                co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            }
            if (saw_client_end || (reset_peer && websocket)) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            const auto input = std::string_view(bytes_value.data(), count);
            for (;;) {
                const auto fed = connection.feed(input);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto built_request = request->build_request();
                        EXPECT_TRUE(built_request.header("x-client") == "unified");
                        const auto request_view = request->request_view();
                        saw_extended_connect = request_view.method_ == "CONNECT" &&
                                               request_view.protocol_ == "websocket";
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, request->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(request->stream_id(), built_request, validation, {.deflate_ = {.enabled_ = true}});
                        const auto* negotiated = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted);
                        if (negotiated == nullptr) {
                            throw std::runtime_error("HTTP/2 WebSocket handshake failed");
                        }
                        websocket.emplace(websocket_input, ruvia::websocket_connection_options{.compression_ = negotiated->compression()});
                        if (reset_peer) {
                            EXPECT_TRUE(connection.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
                        }
                        request_lease.emplace(std::move(*request));
                    } else if (auto* data = event->tunnel_data()) {
                        websocket_input.append(data->bytes());
                        (void)data->take_credit();
                        while (auto ws_event = websocket->next_event()) {
                            if (auto* message = ws_event->message()) {
                                ++messages;
                                EXPECT_EQ(message->payload(), std::string_view(large));
                                EXPECT_TRUE(websocket->submit_frame(ruvia::websocket_opcode::binary, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                            } else if (ws_event->close()) {
                                finish_value = true;
                            } else if (ws_event->protocol_error()) {
                                throw std::runtime_error("invalid tunneled WebSocket frame");
                            }
                            const auto output = websocket->output_plan();
                            outbound.append(output.bytes());
                            (void)websocket->consume_output(output.bytes().size());
                            if (finish_value) {
                                break;
                            }
                        }
                    } else if (event->tunnel_end()) {
                        saw_client_end = true;
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        if (identity) {
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), identity->context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            const unsigned char* selected = nullptr;
            unsigned int selected_size = 0;
            SSL_get0_alpn_selected(stream.native_handle(), &selected, &selected_size);
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(selected), selected_size), std::string_view("h2"));
            co_await serve_stream(stream);
        } else {
            co_await serve_stream(socket);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool reset_observed = false;
    const auto run = [&]() -> ruvia::task<void> {
        const std::array<ruvia::http_header_view, 1> headers{{{"X-Client", "unified"}}};
        const std::string base_url(std::string(secure ? "https://127.0.0.1:" : "http://127.0.0.1:") +
                                   std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client_config config{
            .base_url_ = base_url,
            .write_timeout_ = std::chrono::milliseconds(2000),
            .read_timeout_ = std::chrono::milliseconds(2000),
            .versions_ = ruvia::http_client_versions{.http1_ = false}};
        if (identity) {
            config.tls_.ca_file_ = identity->ca_file_;
            config.tls_.server_name_ = "localhost";
        }
        ruvia::http_client client(attachment.loop(), config);
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").headers(headers).deflate({.compression_ = {.enabled_ = true}}).connect();
            auto& client_handle = websocket;
            if (reset_peer) {
                (void)co_await client_handle.read();
            } else {
                for (const bool compress : {false, true}) {
                    co_await client_handle.binary(std::as_bytes(std::span(std::string_view(large))), {.compress_ = compress});
                    const auto echo = co_await client_handle.read();
                    EXPECT_TRUE(echo.has_value());
                    if (echo) {
                        EXPECT_EQ(echo->payload(), std::string_view(large));
                    }
                }
                co_await client_handle.close({});
            }
        } catch (const std::system_error&) {
            reset_observed = true;
            if (!reset_peer) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(saw_extended_connect);
    if (reset_peer) {
        EXPECT_TRUE(reset_observed);
    } else {
        EXPECT_EQ(messages, 2U);
        EXPECT_TRUE(saw_client_end);
    }
}

enum class closing_peer { clean_release,
    no_close,
    no_close_after_end,
    cancel_after_close,
    error_after_close,
    error_after_end,
    end_without_close };

void exercise_closing_peer(closing_peer behavior) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool received_close = false;
    bool released_request = false;
    std::vector<ruvia::http2_frame_type> terminal_frames{};
    bool received_end_stream = false;
    std::optional<ruvia::http2_error_code> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        ruvia::http_io_buffer websocket_input;
        std::optional<ruvia::websocket_connection> websocket;
        std::array<char, 16384> input{};
        std::string output{};
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        const auto built_request = head->build_request();
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, head->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(head->stream_id(), built_request, validation);
                        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
                        request.emplace(std::move(*head));
                        websocket.emplace(websocket_input);
                    } else if (auto* data = event->tunnel_data()) {
                        websocket_input.append(data->bytes());
                        (void)data->take_credit();
                        while (auto frame = websocket->next_event()) {
                            if (const auto* close = frame->close()) {
                                received_close = true;
                                EXPECT_EQ(close->close_code(), std::uint16_t{1000});
                                EXPECT_EQ(close->reason(), std::string_view("complete"));
                                break;
                            }
                            EXPECT_TRUE(frame->protocol_error() == nullptr);
                        }
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
            if (!received_close) {
                continue;
            }
            if (behavior != closing_peer::no_close && behavior != closing_peer::no_close_after_end &&
                behavior != closing_peer::end_without_close) {
                const auto close = websocket->output_plan();
                EXPECT_TRUE(!close.bytes().empty());
                EXPECT_TRUE(connection.submit_data(1, close.bytes(), ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
                (void)websocket->consume_output(close.bytes().size());
            }
            if (behavior == closing_peer::clean_release || behavior == closing_peer::end_without_close ||
                behavior == closing_peer::no_close_after_end || behavior == closing_peer::error_after_end) {
                EXPECT_TRUE(connection.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
            }
            // Publish the DATA/END_STREAM before releasing the real server lease.
            // Its half-open retirement emits NO_ERROR, in the same socket write.
            ruvia::test::drain_output(connection, output);
            if (behavior == closing_peer::clean_release || behavior == closing_peer::no_close ||
                behavior == closing_peer::no_close_after_end) {
                if (behavior == closing_peer::no_close) {
                    EXPECT_TRUE(connection.submit_reset(1, ruvia::http2_error_code::no_error) == ruvia::http2_submit_status::accepted);
                }
                EXPECT_TRUE(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                released_request = true;
            } else if (behavior != closing_peer::end_without_close) {
                const auto error = behavior == closing_peer::cancel_after_close ? ruvia::http2_error_code::cancel : ruvia::http2_error_code::protocol_error;
                EXPECT_TRUE(connection.submit_reset(1, error) == ruvia::http2_submit_status::accepted);
            }
            std::string retirement{};
            ruvia::test::drain_output(connection, retirement);
            output.append(retirement);
            for (std::size_t offset = 0; offset < output.size();) {
                const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(output.data() + offset, ruvia::http2_frame_header_bytes));
                EXPECT_TRUE(header_value.has_value());
                if (header_value->stream_id_ == 1) {
                    terminal_frames.push_back(static_cast<ruvia::http2_frame_type>(header_value->type_));
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 0x1U) != 0) {
                        received_end_stream = true;
                    }
                    if (header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        EXPECT_EQ(header_value->length_, std::uint32_t{4});
                        const auto* code = output.data() + offset + ruvia::http2_frame_header_bytes;
                        const auto error = (std::uint32_t(static_cast<unsigned char>(code[0])) << 24) |
                                           (std::uint32_t(static_cast<unsigned char>(code[1])) << 16) |
                                           (std::uint32_t(static_cast<unsigned char>(code[2])) << 8) |
                                           std::uint32_t(static_cast<unsigned char>(code[3]));
                        reset_error = static_cast<ruvia::http2_error_code>(error);
                    }
                }
                offset += ruvia::http2_frame_header_bytes + header_value->length_;
            }
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            co_return;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool protocol_error = false;
    bool close_completed = false;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            auto& client_handle = websocket;
            co_await client_handle.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "complete"});
            close_completed = client_handle.termination() == ruvia::websocket_termination::local_close;
            protocol_error = client_handle.termination() == ruvia::websocket_termination::eof;
        } catch (const std::system_error&) {
            protocol_error = true;
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received_close);
    EXPECT_TRUE(close_completed == (behavior == closing_peer::clean_release));
    EXPECT_TRUE(protocol_error == (behavior != closing_peer::clean_release));
    if (behavior == closing_peer::clean_release) {
        EXPECT_TRUE(released_request);
        EXPECT_EQ(terminal_frames.size(), std::size_t{3});
        EXPECT_TRUE(terminal_frames[0] == ruvia::http2_frame_type::data);
        EXPECT_TRUE(terminal_frames[1] == ruvia::http2_frame_type::data);
        EXPECT_TRUE(terminal_frames[2] == ruvia::http2_frame_type::rst_stream);
        EXPECT_TRUE(received_end_stream);
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
    }
    if (behavior == closing_peer::cancel_after_close) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::cancel);
    } else if (behavior == closing_peer::error_after_close || behavior == closing_peer::error_after_end) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
    } else if (behavior == closing_peer::no_close) {
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
        EXPECT_TRUE(!received_end_stream);
    } else if (behavior == closing_peer::no_close_after_end) {
        EXPECT_TRUE(released_request);
        EXPECT_TRUE(reset_error == ruvia::http2_error_code::no_error);
        EXPECT_TRUE(received_end_stream);
    }
    if (behavior == closing_peer::error_after_end) {
        EXPECT_TRUE(received_end_stream);
    }
}

enum class handshake_end { close_on_data,
    close_without_end_stream,
    end_on_headers };

// The peer answers the Extended CONNECT in one write: either 200 HEADERS
// followed by DATA carrying its Close frame and END_STREAM (RFC 8441 section 5
// orderly close), or 200 HEADERS that themselves carry END_STREAM. It then
// waits for the client connection to retire after shutdown.
void exercise_handshake_end(handshake_end mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto worker = attachment.loop().worker();
    ruvia::worker_signal peer_done(worker);
    bool reached_eof = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::string output;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                reached_eof = true;
                co_return;
            }
            if (count == 0) {
                reached_eof = true;
                co_return;
            }
            if (request) {
                continue;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    auto* head = event->request_head();
                    if (head == nullptr) {
                        continue;
                    }
                    if (mode == handshake_end::end_on_headers) {
                        ruvia::test::drain_output(connection, output);
                        std::string block;
                        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
                        std::array<char, ruvia::http2_frame_header_bytes> header{};
                        EXPECT_TRUE(ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(block.size()),
                            ruvia::http2_frame_type::headers, 0x5, head->stream_id()));
                        output.append(header.data(), header.size());
                        output.append(block);
                        co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
                    } else {
                        const auto built_request = head->build_request();
                        const auto validation = ruvia::validate_http2_websocket_handshake(connection, head->stream_id(), built_request);
                        const auto submitted = connection.submit_websocket_handshake(head->stream_id(), built_request, validation);
                        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
                        ruvia::http_io_buffer websocket_input;
                        ruvia::websocket_connection websocket(websocket_input);
                        EXPECT_TRUE(websocket.submit_close(ruvia::websocket_close_status::normal_closure, "bye") == ruvia::websocket_close_submit_status::accepted);
                        const auto close = websocket.output_plan();
                        const auto end_flag = mode == handshake_end::close_without_end_stream
                                                  ? ruvia::http2_end_stream::keep_open
                                                  : ruvia::http2_end_stream::end_stream;
                        EXPECT_TRUE(connection.submit_data(head->stream_id(), close.bytes(), end_flag, 65535).status_ ==
                                    ruvia::http2_data_submit_status::complete);
                    }
                    request.emplace(std::move(*head));
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    bool connected = false;
    bool closed_by_peer = false;
    std::optional<std::error_code> connect_error;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.close_handshake_timeout_ = std::chrono::milliseconds(50)}).connect();
            connected = true;
            auto& client_handle = websocket;
            closed_by_peer = !(co_await client_handle.read()) &&
                             client_handle.peer_close_code() == std::uint16_t{1000} &&
                             client_handle.peer_close_reason() == std::string_view("bye") &&
                             websocket.peer_close_code() == std::uint16_t{1000} &&
                             websocket.peer_close_reason() == std::string_view("bye");
        } catch (const ruvia::http_client_error& error) {
            connect_error = error.code();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await peer_done.wait();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (mode != handshake_end::end_on_headers) {
        EXPECT_TRUE(connected);
        EXPECT_TRUE(closed_by_peer);
        EXPECT_TRUE(!connect_error.has_value());
    } else {
        EXPECT_TRUE(!connected);
        EXPECT_TRUE(connect_error == ruvia::make_error_code(ruvia::http_client_errc::handshake_rejected));
    }
    EXPECT_TRUE(reached_eof);
}
}  // namespace

TEST(Ruvia, http2_websocket_client_drives_flow_control_deflate_and_end_stream) {
    exercise_client(false);
}

TEST(Ruvia, http2_websocket_wss_authenticates_and_exchanges_deflated_messages) {
    exercise_client(false, true);
}

TEST(Ruvia, http2_websocket_client_observes_peer_reset_and_joins_drivers) {
    exercise_client(true);
}

TEST(Ruvia, http2_websocket_normal_close_consumes_peer_close_before_clean_stream_retirement) {
    exercise_closing_peer(closing_peer::clean_release);
}

TEST(Ruvia, http2_websocket_no_error_reset_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(closing_peer::no_close);
}

TEST(Ruvia, http2_websocket_no_error_reset_after_end_stream_still_requires_peer_close) {
    exercise_closing_peer(closing_peer::no_close_after_end);
}

TEST(Ruvia, http2_websocket_cancel_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(closing_peer::cancel_after_close);
}

TEST(Ruvia, http2_websocket_protocol_reset_after_buffered_close_remains_an_error) {
    exercise_closing_peer(closing_peer::error_after_close);
}

TEST(Ruvia, http2_websocket_protocol_reset_after_close_and_end_stream_remains_an_error) {
    exercise_closing_peer(closing_peer::error_after_end);
}

TEST(Ruvia, http2_websocket_end_stream_without_peer_close_is_not_a_close_handshake) {
    exercise_closing_peer(closing_peer::end_without_close);
}

TEST(Ruvia, http2_websocket_handshake_accepts_close_and_end_stream_in_the_same_read) {
    exercise_handshake_end(handshake_end::close_on_data);
}

TEST(Ruvia, http2_websocket_handshake_rejects_end_stream_on_response_headers_and_sends_goaway) {
    exercise_handshake_end(handshake_end::end_on_headers);
}

TEST(Ruvia, http2_websocket_close_handshake_timeout_sends_end_stream_before_goaway) {
    exercise_handshake_end(handshake_end::close_without_end_stream);
}

#endif

namespace static_file_test {

[[nodiscard]] std::string read_file_response_body(const ruvia::http_response_file_view& file) {
    std::ifstream input(file.to_path(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("file response path could not be opened");
    }
    input.seekg(static_cast<std::streamoff>(file.offset()));
    std::string body{};
    body.resize(static_cast<std::size_t>(file.length()));
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (static_cast<std::size_t>(input.gcount()) != body.size()) {
        throw std::runtime_error("file response path did not contain the selected slice");
    }
    return body;
}

class static_file_test_request final {
public:
    void set_method(std::string_view method) {
        method_ = method;
    }

    void set_target(std::string_view target) {
        target_.assign(target);
    }

    void add_header(ruvia::http_header_view header) {
        headers_.emplace_back(header.name(), header.value());
    }

    [[nodiscard]] ruvia::test_request request() const {
        auto request_value = ruvia::test_request::method(method_, target_);
        for (const auto& [name, value] : headers_) {
            request_value.header(name, value);
        }
        return request_value;
    }

private:
    std::string method_{"GET"};
    std::string target_{"/"};
    std::vector<std::pair<std::string, std::string>> headers_{};
};

template <typename callback_type>
decltype(auto) with_static_context(const static_file_test_request& request, callback_type&& callback) {
    using result_type = typename std::invoke_result_t<callback_type, ruvia::context&>::value_type;
    std::optional<std::conditional_t<std::is_void_v<result_type>, bool, result_type>> result;
    std::exception_ptr error;
    static_cast<void>(ruvia::test::with_context(
        request.request(), [&](ruvia::context& context_value) -> ruvia::task<void> {
            try {
                if constexpr (std::is_void_v<result_type>) {
                    co_await callback(context_value);
                    result.emplace(true);
                } else {
                    result.emplace(co_await callback(context_value));
                }
            } catch (...) {
                error = std::current_exception();
            }
            context_value.respond(context_value.text(""));
            co_return;
        }));
    if (error) {
        std::rethrow_exception(error);
    }
    if (!result) {
        throw std::runtime_error("static file request callback was not invoked");
    }
    if constexpr (!std::is_void_v<result_type>) {
        return result_type(std::move(*result));
    }
}

[[nodiscard]] ruvia::http_status_code static_file_status(
    const ruvia::static_root& root, std::string_view path) {
    static_file_test_request request;
    return with_static_context(request, [&](ruvia::context& ctx) -> ruvia::task<ruvia::http_status_code> {
        try {
            co_return (co_await ctx.static_file(root, {.relative_path_ = path})).status();
        } catch (const ruvia::http_error& error) {
            co_return error.info().status();
        }
    });
}

}  // namespace static_file_test

using static_file_test::read_file_response_body;
using static_file_test::static_file_status;
using static_file_test::static_file_test_request;
using static_file_test::with_static_context;

TEST(Ruvia, static_root_copies_public_mime_configuration_into_owned_storage) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_static_mime_resource_dir");
    const auto& dir = directory.path();
    std::ofstream(dir / "payload.custom-resource") << "content";

    {
        ruvia::static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        options.mime_types_.push_back(ruvia::static_mime_type{
            .extension_ = ".custom-resource",
            .content_type_ = "application/x-custom-resource-type",
        });
        ruvia::static_root root(dir, options);
        options.mime_types_.front().content_type_ = "application/x-changed";
        static_file_test_request request;
        with_static_context(request, [&](ruvia::context& ctx) -> ruvia::task<void> {
            const auto response = co_await ctx.static_file(root, {.relative_path_ = "payload.custom-resource"});
            EXPECT_EQ(response.header("Content-Type"),
                std::string_view("application/x-custom-resource-type"));
        });
    }
}

TEST(Ruvia, static_root_serves_utf8_file_and_directory_urls) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory temporary("ruvia_static_utf8");
    const auto& dir = temporary.path();

    const auto filename = fs::path(u8"\u6587\u6863-\U0001F4C4.txt");
    const auto directory = fs::path(u8"\u76EE\u5F55-\U0001F4C1");
    fs::create_directory(dir / directory);
    for (const auto& [path, body] : std::array{
             std::pair{dir / filename, std::string_view("unicode-file-body")},
             std::pair{dir / directory / "index.html", std::string_view("unicode-index-body")}}) {
        std::ofstream output(path, std::ios::binary);
        output << body;
        if (!output) {
            throw std::runtime_error("failed to write Unicode static file");
        }
    }

    ruvia::static_root root(dir, {.index_file_ = "index.html"});
    struct case_value final {
        std::string_view relative_url_;
        std::string_view relative_path_;
        std::string_view body_;
        std::string_view content_type_;
    };
    // static_file takes the decoded path the encoded URL names.
    const case_value cases[]{
        {"%E6%96%87%E6%A1%A3-%F0%9F%93%84.txt", "\xE6\x96\x87\xE6\xA1\xA3-\xF0\x9F\x93\x84.txt", "unicode-file-body",
            "text/plain; charset=utf-8"},
        {"%E7%9B%AE%E5%BD%95-%F0%9F%93%81/", "\xE7\x9B\xAE\xE5\xBD\x95-\xF0\x9F\x93\x81/", "unicode-index-body",
            "text/html; charset=utf-8"},
    };
    for (const auto& item : cases) {
        static_file_test_request request;
        const std::string target = "/" + std::string(item.relative_url_);
        request.set_target(target);
        with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            const auto response = co_await context_value.static_file(root, {.relative_path_ = item.relative_path_});
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(response.header("Content-Type"), std::optional<std::string_view>{item.content_type_});
            const auto file = response.file_body();
            EXPECT_TRUE(file.has_value());
            if (file) {
                EXPECT_EQ(file->length(), static_cast<std::uint64_t>(item.body_.size()));
                EXPECT_EQ(read_file_response_body(*file), item.body_);
            }
        });
    }
}

TEST(Ruvia, static_root_serves_every_default_file_type_with_its_media_type) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_static_default_types");
    const auto& dir = directory.path();

    const std::pair<std::string_view, std::string_view> cases[]{
        {"apng", "image/apng"},
        {"avif", "image/avif"},
        {"bmp", "image/bmp"},
        {"css", "text/css; charset=utf-8"},
        {"cur", "image/vnd.microsoft.icon"},
        {"eot", "application/vnd.ms-fontobject"},
        {"gif", "image/gif"},
        {"htm", "text/html; charset=utf-8"},
        {"html", "text/html; charset=utf-8"},
        {"ico", "image/vnd.microsoft.icon"},
        {"jpeg", "image/jpeg"},
        {"jpg", "image/jpeg"},
        {"js", "text/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"map", "application/json; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"otf", "font/otf"},
        {"png", "image/png"},
        {"svg", "image/svg+xml"},
        {"ttf", "font/ttf"},
        {"txt", "text/plain; charset=utf-8"},
        {"wasm", "application/wasm"},
        {"webmanifest", "application/manifest+json; charset=utf-8"},
        {"webp", "image/webp"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"xml", "text/xml; charset=utf-8"},
        {"xsl", "text/xml; charset=utf-8"},
    };
    for (const auto& [extension, content_type] : cases) {
        std::ofstream(dir / ("file." + std::string(extension)), std::ios::binary) << "body";
    }

    ruvia::static_root root(dir, {});
    for (const auto& [extension, content_type] : cases) {
        const auto relative_path = "file." + std::string(extension);
        static_file_test_request request;
        request.set_target(std::string("/") + relative_path);
        with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            const auto response = co_await context_value.static_file(root, {.relative_path_ = relative_path});
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(response.header("Content-Type"), std::optional<std::string_view>{content_type});
        });
    }
}

TEST(Ruvia, file_response_preserves_normal_status_before_evaluating_request_conditions) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_file_normal_status");
    const auto& dir = directory.path();
    const auto path = dir / "payload.txt";
    {
        std::ofstream output(path, std::ios::binary);
        output << "0123456789";
    }
    struct case_value final {
        ruvia::http_status_code status_;
        ruvia::http_header_view condition_;
    };
    const case_value cases[]{
        {ruvia::http_status::not_found, {"If-None-Match", "*"}},
        {ruvia::http_status::temporary_redirect, {"If-Match", "\"different\""}},
        {ruvia::http_status::created, {"Range", "bytes=0-1"}},
    };
    ruvia::static_root root(dir,
        {.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all}});
    for (const auto& [indexed, item] : std::array{
             std::pair{false, cases[0]}, std::pair{true, cases[0]},
             std::pair{false, cases[1]}, std::pair{true, cases[1]},
             std::pair{false, cases[2]}, std::pair{true, cases[2]}}) {
        static_file_test_request request;
        request.add_header(item.condition_);
        with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            context_value.draft().status(item.status_);
            context_value.draft().header("X-Context", "preserved");
            std::optional<ruvia::http_response> response;
            try {
                if (indexed) {
                    response.emplace(co_await context_value.static_file(root, {.relative_path_ = "payload.txt"}));
                } else {
                    response.emplace(co_await context_value.file({.path_ = path}));
                }
            } catch (const ruvia::http_error&) {
            }
            EXPECT_TRUE(response.has_value());
            if (response) {
                EXPECT_EQ(response->status(), item.status_);
                EXPECT_EQ(response->header("X-Context"), std::optional<std::string_view>{"preserved"});
                EXPECT_TRUE(!response->header("Content-Range").has_value());
                // Every platform keeps the body file-backed for the pooled
                // reader.
                const auto file = response->file_body();
                EXPECT_TRUE(file.has_value());
                if (file) {
                    EXPECT_EQ(file->offset(), std::uint64_t{0});
                    EXPECT_EQ(file->length(), std::uint64_t{10});
                } else {
                    EXPECT_EQ(response->body_bytes().size(), std::size_t{10});
                }
            }
        });
    }
}

TEST(Ruvia, static_file_response_owns_path_after_handler_local_root_is_destroyed) {
    namespace fs = std::filesystem;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_local_root");
    const auto& dir = directory.path();
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << "owned-static-path";
    }

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // The root is borrowed until the task completes; it is destroyed only
        // after the awaited response exists.
        std::optional<ruvia::http_response> response;
        {
            ruvia::static_root_options options;
            options.file_types_ =
                ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
            ruvia::static_root handler_local_root(dir, std::move(options));
            response.emplace(co_await context_value.static_file(
                handler_local_root, {.relative_path_ = "payload.txt", .content_type_ = "text/plain"}));
        }

        const auto file = response->file_body();
        EXPECT_TRUE(file.has_value());
        if (file.has_value()) {
            EXPECT_EQ(read_file_response_body(*file), std::string("owned-static-path"));
        }
    });
}

TEST(Ruvia, static_file_type_policy_has_closed_exact_alternatives) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_static_file_type_policy");
    const auto& dir = directory.path();
    std::ofstream(dir / "index.html") << "html";
    std::ofstream(dir / "asset.custom") << "custom";

    bool empty_only_threw = false;
    try {
        ruvia::static_root rejected_root(dir,
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::only}});
    } catch (const std::invalid_argument&) {
        empty_only_threw = true;
    }
    EXPECT_TRUE(empty_only_threw);

    for (const std::string_view invalid : {"", ".", "..", "a/b", "a\\b"}) {
        bool invalid_type_threw = false;
        try {
            ruvia::static_root rejected_root(dir,
                {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::only,
                     .extensions_ = {std::string(invalid)}}});
        } catch (const std::invalid_argument&) {
            invalid_type_threw = true;
        }
        EXPECT_TRUE(invalid_type_threw);
    }

    ruvia::static_root default_root(dir);
    EXPECT_TRUE(static_file_status(default_root, "index.html") == ruvia::http_status::ok);
    EXPECT_TRUE(static_file_status(default_root, "asset.custom") != ruvia::http_status::ok);

    ruvia::static_root_options all_options;
    all_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::static_root all_root(dir, std::move(all_options));
    EXPECT_TRUE(static_file_status(all_root, "index.html") == ruvia::http_status::ok);
    EXPECT_TRUE(static_file_status(all_root, "asset.custom") == ruvia::http_status::ok);

    ruvia::static_root_options only_options;
    only_options.file_types_ = ruvia::static_file_type_policy{
        .kind_ = ruvia::static_file_type_policy::kind::only, .extensions_ = {".CUSTOM"}};
    ruvia::static_root only_root(dir, std::move(only_options));
    EXPECT_TRUE(static_file_status(only_root, "index.html") != ruvia::http_status::ok);
    EXPECT_TRUE(static_file_status(only_root, "asset.custom") == ruvia::http_status::ok);
}

TEST(Ruvia, static_file_range_serving_status_and_content_range) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_range_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "empty.txt", std::ios::binary | std::ios::trunc);
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    const auto serve_method = [&root](std::string_view method, std::string_view path,
                                  std::string_view range) {
        static_file_test_request request;
        request.set_method(method);
        if (!range.empty()) {
            request.add_header(http_header_view{"Range", range});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            const auto response =
                co_await context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"});
            co_return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Content-Range").value_or("")));
        });
    };
    const auto serve_file = [&serve_method](std::string_view path, std::string_view range) {
        return serve_method("GET", path, range);
    };
    const auto serve = [&serve_file](
                           std::string_view range) { return serve_file("data.txt", range); };

    // A valid single range -> 206 with the byte range echoed.
    const auto ok = serve("bytes=0-4");
    EXPECT_EQ(ok.first, ruvia::http_status::partial_content);
    EXPECT_EQ(ok.second, std::string("bytes 0-4/100"));

    // Multiple ranges produce a multipart representation without a top-level Content-Range.
    const auto multi = serve("bytes=0-9,20-29");
    EXPECT_EQ(multi.first, ruvia::http_status::partial_content);
    EXPECT_TRUE(multi.second.empty());

    // A wholly unsatisfiable range -> 416 with "bytes */size".
    const auto bad = serve("bytes=1000-2000");
    EXPECT_EQ(bad.first, ruvia::http_status::range_not_satisfiable);
    EXPECT_EQ(bad.second, std::string("bytes */100"));

    // An unknown range unit MUST be ignored (RFC 9110 §14.2) -> full 200, not 416.
    const auto unknown_unit = serve("items=0-9");
    EXPECT_EQ(unknown_unit.first, ruvia::http_status::ok);

    // A syntactically malformed byte range is likewise ignored -> full 200.
    const auto malformed = serve("bytes=abc");
    EXPECT_EQ(malformed.first, ruvia::http_status::ok);

    // Range units are case-insensitive (RFC 9110 §14.1); this is still a 206.
    const auto case_insensitive_unit = serve("Bytes=5-9");
    EXPECT_EQ(case_insensitive_unit.first, ruvia::http_status::partial_content);
    EXPECT_EQ(case_insensitive_unit.second, std::string("bytes 5-9/100"));

    // RFC 9110 §14.2 defines Range handling only for GET. A HEAD request
    // carrying the same field must describe the full representation with 200,
    // not invent a partial 206 response with Content-Range metadata.
    const auto head = serve_method("HEAD", "data.txt", "bytes=0-4");
    EXPECT_EQ(head.first, ruvia::http_status::ok);
    EXPECT_TRUE(head.second.empty());

    // RFC 9110 §14.1.2: 0-byte representation cannot satisfy `bytes=0-0` (first-pos >= complete-length).
    const auto empty = serve_file("empty.txt", "bytes=0-0");
    EXPECT_EQ(empty.first, ruvia::http_status::range_not_satisfiable);
    EXPECT_EQ(empty.second, std::string("bytes */0"));
}

TEST(Ruvia, static_file_takes_decoded_path_and_stays_traversal_safe) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_pct_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "my report.txt", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    fs::create_directories(dir / "sub");
    {
        std::ofstream out(dir / "sub" / "inner.txt", std::ios::binary | std::ios::trunc);
        out << "inner";
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view path) {
        static_file_test_request request;
        request.set_method("GET");
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<ruvia::http_status_code> {
            ruvia::http_status_code status = ruvia::http_status::internal_server_error;
            try {
                status = (co_await context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"}))
                             .status();
            } catch (const ruvia::http_error& error) {
                status = error.info().status();
            }
            co_return status;
        });
    };

    // relative_path_ is already a decoded file path: the real name resolves,
    // and an escape is the literal bytes of a name, never decoded a second time.
    EXPECT_EQ(serve("my report.txt"), ruvia::http_status::ok);
    EXPECT_EQ(serve("my%20report.txt"), ruvia::http_status::not_found);
    EXPECT_EQ(serve("sub%2Finner.txt"), ruvia::http_status::not_found);
    EXPECT_EQ(serve("sub/inner.txt"), ruvia::http_status::ok);
    EXPECT_EQ(serve("sub\\inner.txt"), ruvia::http_status::ok);

    // A "." or ".." segment is rejected (403), never resolved: a decoded
    // parameter can carry one behind an encoded separator that no route or
    // path scope saw, so even a path that would stay inside the root fails.
    EXPECT_EQ(serve("../etc/passwd"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub/../../etc"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub\\..\\..\\etc"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub/../my report.txt"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub\\..\\my report.txt"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("./my report.txt"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub/./inner.txt"), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub/inner.txt/.."), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("."), ruvia::http_status::forbidden);
    EXPECT_EQ(serve("sub//inner.txt"), ruvia::http_status::forbidden);

    // A NUL cannot occur in a filename and is rejected outright.
    EXPECT_EQ(serve(std::string_view("my\0report.txt", 13)), ruvia::http_status::forbidden);
}

TEST(Ruvia, static_file_declares_vary_accept_encoding_but_context_file_does_not) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_vary_dir");
    const auto& dir = directory.path();
    const auto file_path = dir / "app.js";
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        const std::string content(50, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // No sidecar and no Accept-Encoding -> the identity file is served, but it must
        // STILL declare Vary: Accept-Encoding: the same URL would serve a compressed
        // variant to a capable client, so a shared cache keyed only on the URL must not
        // reuse this identity body for everyone (RFC 9110 12.5.5 / RFC 9111 4.1). The
        // identity body carries no Content-Encoding.
        const auto served =
            co_await context_value.static_file(root, {.relative_path_ = "app.js", .content_type_ = "text/javascript"});
        EXPECT_EQ(served.status(), ruvia::http_status::ok);
        EXPECT_TRUE(
            (served.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos));
        EXPECT_TRUE(!served.header("Content-Encoding").has_value());

        // context::file serves a single path with no encoding negotiation, so it must
        // NOT declare Vary: Accept-Encoding (which would needlessly fragment caches).
        const auto direct = co_await context_value.file({.path_ = file_path});
        EXPECT_EQ(direct.status(), ruvia::http_status::ok);
        EXPECT_TRUE(!direct.header("Vary").has_value());
        EXPECT_EQ(direct.header("Content-Type").value_or(""),
            std::string_view("text/javascript; charset=utf-8"));
        // Every platform keeps the body file-backed for the pooled reader.
        EXPECT_TRUE(direct.file_body().has_value());
        if (direct.file_body()) {
            EXPECT_EQ(direct.file_body()->length(), static_cast<std::uint64_t>(fs::file_size(file_path)));
        }
    });
}

TEST(Ruvia, context_file_serves_borrowed_utf8_text_paths) {
    namespace fs = std::filesystem;

    const ruvia::test::scoped_temp_directory directory("ruvia_context_file_text_path_dir");
    const auto& dir = directory.path();
    // A non-ASCII name: Windows must widen the UTF-8 text, not read it in the
    // ANSI code page.
    const auto file_path = dir / fs::path(u8"caf\u00e9.txt");
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        out << "borrowed";
    }
    const auto utf8 = file_path.u8string();
    const std::string text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    // A view that is not NUL-terminated: the bytes after it must not be read.
    const std::string padded = std::string(text) + "-not-part-of-the-path";

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto status_of = [&](ruvia::borrowed_file_path path) -> ruvia::task<ruvia::http_status_code> {
            try {
                co_return (co_await context_value.file({.path_ = path})).status();
            } catch (const ruvia::http_error& error) {
                co_return error.info().status();
            }
        };
        const auto matches_borrowed_body = [](const ruvia::http_response& response) {
            return response.file_body().has_value() &&
                   response.file_body()->length() == std::string_view("borrowed").size();
        };

        const auto from_text = co_await context_value.file({.path_ = text});
        EXPECT_EQ(from_text.status(), ruvia::http_status::ok);
        EXPECT_TRUE(matches_borrowed_body(from_text));
        EXPECT_EQ(from_text.header("Content-Type").value_or(""),
            std::string_view("text/plain; charset=utf-8"));

        const auto from_view = co_await context_value.file({.path_ = std::string_view(padded).substr(0, text.size())});
        EXPECT_EQ(from_view.status(), ruvia::http_status::ok);
        EXPECT_TRUE(matches_borrowed_body(from_view));

        const auto from_c_string = co_await context_value.file({.path_ = text.c_str()});
        EXPECT_EQ(from_c_string.status(), ruvia::http_status::ok);
        EXPECT_TRUE(matches_borrowed_body(from_c_string));

        const auto from_borrowed_text = co_await context_value.file({.path_ = ruvia::borrowed_text(text)});
        EXPECT_EQ(from_borrowed_text.status(), ruvia::http_status::ok);
        EXPECT_TRUE(matches_borrowed_body(from_borrowed_text));

        const auto from_path = co_await context_value.file({.path_ = file_path});
        EXPECT_EQ(from_path.status(), ruvia::http_status::ok);
        EXPECT_TRUE(matches_borrowed_body(from_path));

        // Text that cannot name the file is a miss, not a truncated lookup.
        const std::string with_nul = text + std::string(1, '\0') + "x";
        EXPECT_EQ(co_await status_of(with_nul), ruvia::http_status::not_found);
        EXPECT_EQ(co_await status_of(""), ruvia::http_status::not_found);
        EXPECT_EQ(co_await status_of(nullptr), ruvia::http_status::not_found);
        EXPECT_EQ(co_await status_of(ruvia::borrowed_file_path{}), ruvia::http_status::not_found);
        const std::string invalid_utf8 = std::string(text) + "\xff";
        EXPECT_EQ(co_await status_of(invalid_utf8), ruvia::http_status::not_found);
    });
}

// static_file() borrows its root until the returned task completes, so a
// temporary root is rejected at compile time.
template <typename root_type>
concept static_file_accepts_root = requires(ruvia::context& context_value, root_type&& root) {
    context_value.static_file(std::forward<root_type>(root), ruvia::static_file_response_options{});
};
static_assert(static_file_accepts_root<const ruvia::static_root&>);
static_assert(!static_file_accepts_root<ruvia::static_root>);
static_assert(!static_file_accepts_root<const ruvia::static_root>);

// file() and static_file() own their options before returning: a stored task
// awaited after its path, relative path and content type sources were
// overwritten and destroyed still serves what the call named.
TEST(Ruvia, file_and_static_file_own_their_options_before_returning) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_file_owned_options");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        out << "owned-options";
    }
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        std::optional<ruvia::task<ruvia::http_response>> from_path;
        std::optional<ruvia::task<ruvia::http_response>> from_text;
        std::optional<ruvia::task<ruvia::http_response>> from_root;
        {
            auto path = dir / "data.txt";
            const auto utf8 = path.u8string();
            std::string text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            std::string path_type("text/x-owned-path");
            std::string text_type("text/x-owned-text");
            std::string relative("data.txt");
            std::string root_type("text/x-owned-root");
            from_path.emplace(context_value.file({.path_ = path, .content_type_ = path_type}));
            from_text.emplace(context_value.file({.path_ = text, .content_type_ = text_type}));
            from_root.emplace(
                context_value.static_file(root, {.relative_path_ = relative, .content_type_ = root_type}));
            // Overwrite every source before it is destroyed, so a task still
            // reading one would name a different file or type.
            path = dir / "missing.txt";
            text.assign(text.size(), 'x');
            path_type.assign(path_type.size(), 'x');
            text_type.assign(text_type.size(), 'x');
            relative.assign(relative.size(), 'x');
            root_type.assign(root_type.size(), 'x');
        }
        const auto path_response = co_await std::move(*from_path);
        const auto text_response = co_await std::move(*from_text);
        const auto root_response = co_await std::move(*from_root);
        const auto serves = [](const ruvia::http_response& response, std::string_view content_type) {
            const auto file = response.file_body();
            return response.status() == ruvia::http_status::ok &&
                   response.header("Content-Type").value_or("") == content_type && file.has_value() &&
                   read_file_response_body(*file) == "owned-options";
        };
        EXPECT_TRUE(serves(path_response, "text/x-owned-path"));
        EXPECT_TRUE(serves(text_response, "text/x-owned-text"));
        EXPECT_TRUE(serves(root_response, "text/x-owned-root"));
    });
}

TEST(Ruvia, static_file_preserves_context_vary_when_adding_accept_encoding) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_vary_merge_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "app.js", std::ios::binary | std::ios::trunc);
        out << "console.log('ok');";
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().header("Vary", "Origin");

        const auto response =
            co_await context_value.static_file(root, {.relative_path_ = "app.js", .content_type_ = "text/javascript"});
        const auto vary = response.header("Vary").value_or("");
        // context response metadata is applied after the file's base headers. It
        // must not erase the negotiation dimension, or a shared cache can reuse an
        // identity/encoded representation for the wrong Accept-Encoding request.
        EXPECT_TRUE(vary.find("Origin") != std::string_view::npos);
        EXPECT_TRUE(vary.find("Accept-Encoding") != std::string_view::npos);
    });
}

TEST(Ruvia, static_file_if_range_dates_do_not_authorize_partial_responses) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_if_range_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    // Serve with a Range plus an optional If-Range; returns (status, Last-Modified).
    const auto serve = [&root](std::optional<std::string_view> if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        return with_static_context(request, [&](ruvia::context& ctx) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            const auto response =
                co_await ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            co_return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve(std::nullopt);
    EXPECT_EQ(base.first, ruvia::http_status::partial_content);
    EXPECT_TRUE(!base.second.empty());
    const auto last_modified = ruvia::parse_http_date(base.second);
    EXPECT_TRUE(last_modified.has_value());
    // Matching whole seconds do not establish a strong file validator.
    EXPECT_EQ(serve(base.second).first, ruvia::http_status::ok);

    // A present empty If-Range is not an entity-tag or HTTP-date. Its condition
    // is therefore false, so it must suppress the Range rather than being
    // confused with an absent field and producing a partial response.
    EXPECT_EQ(serve(std::string_view{}).first, ruvia::http_status::ok);
}

TEST(Ruvia, file_if_range_same_second_updates_require_an_etag) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    const ruvia::test::scoped_temp_directory directory("ruvia_if_range_same_second");
    const auto& dir = directory.path();
    const auto path = dir / "data.txt";
    const auto base_time = floor<seconds>(fs::file_time_type::clock::now()) - hours(24);
    const auto write_version = [&](std::string_view content, milliseconds fraction) {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        fs::last_write_time(path, base_time + fraction);
    };
    // The bytes a response carries: in memory, or the file range it names.
    const auto served_bytes = [](const ruvia::http_response& response) {
        const auto file = response.file_body();
        return file ? static_file_test::read_file_response_body(*file) : std::string(response.body_bytes());
    };
    struct observation {
        ruvia::http_status_code status{ruvia::http_status::internal_server_error};
        std::string last_modified{};
        std::string etag{};
        std::string bytes{};
    };
    for (const bool use_static : {false, true}) {
        std::optional<ruvia::static_root> root;
        const auto serve = [&](bool range, std::optional<std::string_view> if_range) {
            auto request = ruvia::test_request::get("/data.txt");
            if (range) {
                request.header("Range", "bytes=4-8");
            }
            if (if_range) {
                request.header("If-Range", *if_range);
            }
            observation result{};
            (void)ruvia::test::with_context(request, [&](ruvia::context& context) -> ruvia::task<void> {
                const auto response = use_static
                                          ? co_await context.static_file(*root, {.relative_path_ = "data.txt"})
                                          : co_await context.file({.path_ = path});
                result = observation{
                    response.status(),
                    std::string(response.header("Last-Modified").value_or("")),
                    std::string(response.header("ETag").value_or("")),
                    served_bytes(response),
                };
                co_return;
            });
            return result;
        };
        write_version("abcdefghij", milliseconds(100));
        if (use_static) {
            root.emplace(dir);
        }
        const auto before = serve(false, std::nullopt);
        root.reset();
        write_version("0123456789", milliseconds(500));
        if (use_static) {
            root.emplace(dir);
        }
        const auto after = serve(false, std::nullopt);
        EXPECT_EQ(before.status, ruvia::http_status::ok);
        EXPECT_EQ(after.status, ruvia::http_status::ok);
        EXPECT_EQ(before.last_modified, after.last_modified);
        EXPECT_TRUE(before.etag != after.etag);
        const auto date_range = serve(true, before.last_modified);
        EXPECT_EQ(date_range.status, ruvia::http_status::ok);
        EXPECT_EQ(date_range.bytes, std::string("0123456789"));
        EXPECT_EQ(serve(true, before.etag).status, ruvia::http_status::ok);
        const auto etag_range = serve(true, after.etag);
        EXPECT_EQ(etag_range.status, ruvia::http_status::partial_content);
        EXPECT_EQ(etag_range.bytes, std::string("45678"));
    }
}

// RFC 9110 §15.3.7: a 206 answering a matched If-Range SHOULD NOT repeat
// representation fields beyond the required ones; without If-Range it MUST
// carry every representation field a 200 would.
TEST(Ruvia, file_partial_responses_to_if_range_omit_repeated_representation_fields) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_if_range_partial_fields");
    const auto& dir = directory.path();
    const auto path = dir / "data.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "0123456789";
    }
    struct observation {
        ruvia::http_status_code status{ruvia::http_status::internal_server_error};
        std::optional<std::string> content_type;
        std::optional<std::string> last_modified;
        std::optional<std::string> etag;
        std::optional<std::string> content_range;
    };
    const auto copy = [](std::optional<std::string_view> value) {
        return value ? std::optional<std::string>(std::string(*value)) : std::nullopt;
    };
    std::optional<ruvia::static_root> root;
    root.emplace(dir);
    for (const bool use_static : {false, true}) {
        const auto serve = [&](std::string_view range, std::optional<std::string_view> if_range) {
            auto request = ruvia::test_request::get("/data.txt");
            request.header("Range", range);
            if (if_range) {
                request.header("If-Range", *if_range);
            }
            observation result{};
            (void)ruvia::test::with_context(request, [&](ruvia::context& context) -> ruvia::task<void> {
                const auto response = use_static ? co_await context.static_file(*root, {.relative_path_ = "data.txt"})
                                                 : co_await context.file({.path_ = path, .content_type_ = "text/plain"});
                result = observation{response.status(), copy(response.header("Content-Type")),
                    copy(response.header("Last-Modified")), copy(response.header("ETag")),
                    copy(response.header("Content-Range"))};
                co_return;
            });
            return result;
        };
        const auto plain = serve("bytes=4-8", std::nullopt);
        EXPECT_EQ(plain.status, ruvia::http_status::partial_content);
        EXPECT_TRUE(plain.content_type.has_value());
        EXPECT_TRUE(plain.last_modified.has_value());
        EXPECT_TRUE(plain.etag.has_value());
        const auto etag = plain.etag ? std::string_view(*plain.etag) : std::string_view{};

        const auto conditional = serve("bytes=4-8", etag);
        EXPECT_EQ(conditional.status, ruvia::http_status::partial_content);
        EXPECT_TRUE(!conditional.content_type.has_value());
        EXPECT_TRUE(!conditional.last_modified.has_value());
        EXPECT_TRUE(conditional.etag == std::optional<std::string_view>(etag));
        EXPECT_TRUE(conditional.content_range == std::optional<std::string>(std::string("bytes 4-8/10")));

        const auto multipart = serve("bytes=0-1,4-5", etag);
        EXPECT_EQ(multipart.status, ruvia::http_status::partial_content);
        EXPECT_TRUE(multipart.content_type.has_value() && multipart.content_type->starts_with("multipart/byteranges; boundary="));
        EXPECT_TRUE(!multipart.last_modified.has_value());
        EXPECT_TRUE(multipart.etag == std::optional<std::string_view>(etag));
    }
    root.reset();
}

TEST(Ruvia, static_file_historical_last_modified_supports_date_preconditions) {
    namespace fs = std::filesystem;
    using namespace std::chrono;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_historical_mtime_dir");
    const auto& dir = directory.path();
    const auto path = dir / "data.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "historical";
    }
    // Relate the two native clocks without depending on vendor-specific
    // file_clock::from_sys/from_utc APIs. Sub-second sampling is immaterial.
    const auto offset = seconds{-315619200} - duration_cast<seconds>(system_clock::now().time_since_epoch());
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() + offset, ec);
    EXPECT_TRUE(!ec);
    ruvia::static_root_options options;
    options.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::static_root root(dir, std::move(options));

    const auto serve = [&](bool snapshot, std::optional<std::string_view> since) {
        static_file_test_request request;
        request.set_method("GET");
        if (since) {
            request.add_header(ruvia::http_header_view{"If-Modified-Since", *since});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            const auto response = snapshot
                                      ? co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
                                      : co_await context_value.file({.path_ = path, .content_type_ = "text/plain"});
            co_return std::pair(response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };
    for (const auto snapshot : {false, true}) {
        const auto base = serve(snapshot, std::nullopt);
        EXPECT_EQ(base.first, ruvia::http_status::ok);
        const auto date = ruvia::parse_http_date(base.second);
        EXPECT_TRUE(date.has_value());
        if (date) {
            EXPECT_TRUE(*date >= -315619260 && *date <= -315619140);
        }
        EXPECT_EQ(serve(snapshot, base.second).first, ruvia::http_status::not_modified);
        EXPECT_EQ(serve(snapshot, "Thu, 01 Jan 1959 00:00:00 GMT").first, ruvia::http_status::ok);
    }
}

TEST(Ruvia, static_file_clamps_future_last_modified_and_rejects_it_for_if_range) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_future_mtime_dir");
    const auto& dir = directory.path();
    const auto path = dir / "data.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "future";
    }
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::hours(24), ec);
    EXPECT_TRUE(!ec);

    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::optional<std::string_view> if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-1"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            const auto response =
                co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            co_return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };

    const auto before = std::time(nullptr);
    const auto base = serve(std::nullopt);
    const auto after = std::time(nullptr);
    EXPECT_EQ(base.first, ruvia::http_status::partial_content);
    const auto last_modified = ruvia::parse_http_date(base.second);
    EXPECT_TRUE(last_modified.has_value());
    EXPECT_TRUE(last_modified.value_or(after + 1) >= before);
    EXPECT_TRUE(last_modified.value_or(after + 1) <= after);

    // The clamped wire date is the response time, not the file's actual
    // validator. It is therefore weak and cannot authorize stitching a partial
    // response into the client's stored representation.
    EXPECT_EQ(serve(base.second).first, ruvia::http_status::ok);
}

TEST(Ruvia, static_file_without_response_validators_still_enforces_preconditions) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_range_request_policy;
    using ruvia::static_response_validator_policy;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_ifrange_novalidator_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    options.range_requests_ = static_range_request_policy::honor;
    options.response_validators_ =
        static_response_validator_policy::omit;  // no ETag / Last-Modified on responses
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (!if_range.empty()) {
            request.add_header(http_header_view{"If-Range", if_range});
        }
        return with_static_context(request, [&](ruvia::context& ctx) -> ruvia::task<ruvia::http_status_code> {
            co_return (co_await ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"}))
                .status();
        });
    };

    // A plain range with no If-Range is still honored without validators -> 206.
    EXPECT_EQ(serve(""), ruvia::http_status::partial_content);
    // A range WITH If-Range but no server validator cannot be confirmed, so the
    // Range MUST be ignored and the full representation served (RFC 9110 13.1.5) --
    // not a 206 stitched from bytes the client cannot verify it still holds.
    // (Gating the If-Range check on response validator emission skipped it and returned 206.)
    EXPECT_EQ(serve("\"stale-etag\""), ruvia::http_status::ok);
    EXPECT_EQ(serve("Wed, 21 Oct 2015 07:28:00 GMT"), ruvia::http_status::ok);

    struct conditional_result final {
        ruvia::http_status_code status_;
        bool has_etag_;
        bool has_last_modified_;
    };
    const auto serve_conditional = [&root](std::string_view method,
                                       std::string_view name,
                                       std::string_view value) {
        static_file_test_request request;
        request.set_method(method);
        if (!name.empty()) {
            request.add_header(http_header_view{name, value});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<conditional_result> {
            try {
                const auto response =
                    co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
                co_return conditional_result{response.status(), response.header("ETag").has_value(),
                    response.header("Last-Modified").has_value()};
            } catch (const ruvia::http_error& error) {
                co_return conditional_result{error.info().status(), false, false};
            }
        });
    };

    const auto plain = serve_conditional("GET", {}, {});
    EXPECT_EQ(plain.status_, ruvia::http_status::ok);
    EXPECT_TRUE(!plain.has_etag_);
    EXPECT_TRUE(!plain.has_last_modified_);

    // Disabling response validator fields does not disable request
    // preconditions. Wildcard conditions test whether a current representation
    // exists and therefore require no ETag at all; date conditions use the
    // origin's file metadata even when Last-Modified is not emitted.
    const auto get_existing =
        serve_conditional("GET", "If-None-Match", "*");
    EXPECT_EQ(get_existing.status_, ruvia::http_status::not_modified);
    EXPECT_TRUE(!get_existing.has_etag_);
    EXPECT_TRUE(!get_existing.has_last_modified_);
    EXPECT_EQ(
        serve_conditional("POST", "If-None-Match", "*").status_,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(serve_conditional("POST", "If-Match", "*").status_,
        ruvia::http_status::ok);
    EXPECT_EQ(
        serve_conditional("POST", "If-Match", "\"stale\"").status_,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(serve_conditional("POST", "If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT")
                  .status_,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(serve_conditional("GET", "If-Modified-Since", "Fri, 31 Dec 9999 23:59:59 GMT")
                  .status_,
        ruvia::http_status::not_modified);
}

TEST(Ruvia, static_file_if_match_takes_precedence_over_if_unmodified_since) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_precedence_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    struct header {
        std::string_view name_;
        std::string_view value_;
    };
    const auto serve_method = [&root](
                                  std::string_view method, std::initializer_list<header> headers) {
        static_file_test_request request;
        request.set_method(method);
        for (const auto& header : headers) {
            request.add_header(http_header_view{header.name_, header.value_});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            ruvia::http_status_code status = ruvia::http_status::internal_server_error;
            std::string etag{};
            try {
                const auto response =
                    co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
                status = response.status();
                etag.assign(response.header("ETag").value_or(""));
            } catch (const ruvia::http_error& error) {
                status = error.info().status();
            }
            co_return std::pair<ruvia::http_status_code, std::string>(status, std::move(etag));
        });
    };
    const auto serve = [&serve_method](std::initializer_list<header> headers) {
        return serve_method("GET", headers);
    };

    // Discover the current strong ETag with a bare request.
    const auto base = serve({});
    EXPECT_EQ(base.first, ruvia::http_status::ok);
    const std::string etag(base.second);
    EXPECT_TRUE(!etag.empty());
    // A compact strong validator: a quoted 64-bit digest, not the raw size,
    // timestamp and volume/file index it is derived from.
    EXPECT_EQ(etag.size(), std::size_t{18});
    EXPECT_TRUE(etag.front() == '"' && etag.back() == '"');
    EXPECT_TRUE(etag.find_first_not_of("0123456789abcdef", 1) == etag.size() - 1);
    // A date well before the file's mtime -> If-Unmodified-Since fails on its own.
    constexpr std::string_view old_date = "Thu, 01 Jan 1970 00:00:00 GMT";

    // If-Unmodified-Since alone (stale date) is a 412 precondition failure.
    EXPECT_EQ(
        serve({{"If-Unmodified-Since", old_date}}).first,
        ruvia::http_status::precondition_failed);

    // With a matching If-Match present, RFC 9110 §13.2.2 requires If-Unmodified-Since
    // to be ignored -- the strong validator matched, so serve 200 rather than 412.
    EXPECT_EQ(
        serve({{"If-Match", etag},
                  {"If-Unmodified-Since", old_date}})
            .first,
        ruvia::http_status::ok);

    // Presence is distinct from a non-empty field value. The empty #entity-tag
    // list matches no current representation, so an empty If-Match fails rather
    // than being treated as if the field were absent.
    EXPECT_EQ(serve({{"If-Match", ""}}).first,
        ruvia::http_status::precondition_failed);

    constexpr std::string_view future_date = "Fri, 31 Dec 9999 23:59:59 GMT";
    EXPECT_EQ(
        serve({{"If-Modified-Since", future_date}}).first,
        ruvia::http_status::not_modified);
    // Even an empty If-None-Match is present and therefore takes precedence over
    // If-Modified-Since. Its empty list does not match, so the response is 200.
    EXPECT_EQ(
        serve({{"If-None-Match", ""},
                  {"If-Modified-Since", future_date}})
            .first,
        ruvia::http_status::ok);

    // If-Match and If-None-Match are list fields. Repeated field lines are
    // equivalent to one comma-joined value, so a match on the first line must
    // not be lost when the known-header cache records the second line.
    EXPECT_EQ(serve({{"If-Match", etag},
                        {"If-Match", "\"stale\""}})
                  .first,
        ruvia::http_status::ok);
    EXPECT_EQ(serve({{"If-None-Match", etag},
                        {"If-None-Match", "\"stale\""}})
                  .first,
        ruvia::http_status::not_modified);

    // Preconditions protect unsafe methods too. A matching If-None-Match or a
    // stale If-Match / If-Unmodified-Since on POST must fail with 412 instead of
    // serving the file as an unconditional 200.
    EXPECT_EQ(
        serve_method("POST", {{"If-None-Match", etag}}).first,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(
        serve_method("POST", {{"If-Match", "\"stale\""}}).first,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(serve_method("POST",
                  {{"If-Unmodified-Since", old_date}})
                  .first,
        ruvia::http_status::precondition_failed);
    EXPECT_EQ(serve_method("POST", {{"If-Match", etag}}).first,
        ruvia::http_status::ok);
    EXPECT_EQ(
        serve_method("POST", {{"If-None-Match", "\"stale\""}})
            .first,
        ruvia::http_status::ok);
    EXPECT_EQ(serve_method("POST",
                  {{"If-Modified-Since", future_date}})
                  .first,
        ruvia::http_status::ok);

    // OPTIONS does not select or modify a representation, so RFC 9110 §13.2.1
    // requires these conditional fields to be ignored for that method.
    EXPECT_EQ(
        serve_method("OPTIONS", {{"If-Match", "\"stale\""}}).first,
        ruvia::http_status::ok);
}

TEST(Ruvia, static_file_conditional_request_serving) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_conditional_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view header_name,
                           std::string_view header_value) {
        static_file_test_request request;
        request.set_method("GET");
        if (!header_name.empty()) {
            request.add_header(http_header_view{header_name, header_value});
        }
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
            const auto response =
                co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            co_return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("ETag").value_or("")));
        });
    };

    // An unconditional GET yields 200 and a strong ETag validator.
    const auto plain = serve("", "");
    EXPECT_EQ(plain.first, ruvia::http_status::ok);
    EXPECT_TRUE(!plain.second.empty());
    const std::string etag(plain.second);

    // If-None-Match with the current ETag -> 304; a stale one falls through to 200.
    EXPECT_EQ(
        serve("If-None-Match", etag).first,
        ruvia::http_status::not_modified);
    EXPECT_EQ(
        serve("If-None-Match", "\"stale\"").first,
        ruvia::http_status::ok);
    // RFC 9110 15.4.5: the 304 keeps its validator but no representation-only
    // metadata such as Content-Type or Accept-Ranges.
    {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"If-None-Match", etag});
        with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            const auto response =
                co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            EXPECT_EQ(response.status(), ruvia::http_status::not_modified);
            EXPECT_EQ(response.header("ETag").value_or(""), std::string_view(etag));
            EXPECT_TRUE(!response.header("Content-Type").has_value());
            EXPECT_TRUE(!response.header("Accept-Ranges").has_value());
            EXPECT_TRUE(!response.header("Last-Modified").has_value());
        });
    }

    // A comma inside an opaque tag is data, not a list separator. This malformed
    // value closes that tag immediately before the current ETag and must not let
    // the apparent suffix satisfy the condition.
    const std::string malformed_list = std::string("\"stale, ") + etag;
    EXPECT_EQ(
        serve("If-None-Match", malformed_list)
            .first,
        ruvia::http_status::ok);

    // If-Match against a non-matching ETag is a 412 precondition failure (thrown).
    bool precondition = false;
    try {
        (void)serve("If-Match", "\"stale\"");
    } catch (const ruvia::http_error& error) {
        precondition = error.info().status() == ruvia::http_status::precondition_failed;
    }
    EXPECT_TRUE(precondition);

    precondition = false;
    try {
        (void)serve("If-Match", malformed_list);
    } catch (const ruvia::http_error& error) {
        precondition = error.info().status() == ruvia::http_status::precondition_failed;
    }
    EXPECT_TRUE(precondition);
}

TEST(Ruvia, static_root_rejects_empty_custom_mime_type) {
    namespace fs = std::filesystem;
    using ruvia::static_mime_type;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_empty_mime_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.custom", std::ios::binary | std::ios::trunc);
        out << "content";
    }

    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_mime_type mime;
    mime.extension_ = ".custom";
    options.mime_types_.push_back(std::move(mime));

    bool rejected = false;
    try {
        static_root root(dir, std::move(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, static_root_rejects_invalid_static_options_at_construction) {
    namespace fs = std::filesystem;
    using ruvia::static_mime_type;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_invalid_header_options_dir");
    const auto& dir = directory.path();
    std::ofstream(dir / "data.custom") << "content";

    const auto rejects = [&dir](static_root_options options) {
        bool rejected = false;
        try {
            static_root root(dir, std::move(options));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    };

    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        options.cache_control_ = " private";
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        options.default_content_type_ = "text plain";
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        static_mime_type mime;
        mime.extension_ = ".custom";
        mime.content_type_ = "text plain";
        options.mime_types_.push_back(std::move(mime));
        rejects(std::move(options));
    }
    for (const std::string_view invalid_extension :
        {"", ".", "..", "nested/custom", "nested\\custom"}) {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        static_mime_type mime;
        mime.extension_ = invalid_extension;
        mime.content_type_ = "text/plain";
        options.mime_types_.push_back(std::move(mime));
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.mime_types_ = {
            {.extension_ = "CUSTOM", .content_type_ = "application/x-first"},
            {.extension_ = ".custom", .content_type_ = "application/x-second"},
        };
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        options.dotfiles_ = std::bit_cast<ruvia::static_dotfile_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.range_requests_ = std::bit_cast<ruvia::static_range_request_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.response_validators_ =
            std::bit_cast<ruvia::static_response_validator_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }
}

TEST(Ruvia, static_file_if_modified_since_serving) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_ims_dir");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(50, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view if_modified_since) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"If-Modified-Since", if_modified_since});
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<ruvia::http_status_code> {
            co_return (co_await context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"}))
                .status();
        });
    };

    // The file was just written, so an If-Modified-Since far in the future means
    // "not modified since then" -> 304; one far in the past means it HAS changed
    // -> 200.
    EXPECT_EQ(serve("Fri, 01 Jan 2100 00:00:00 GMT"), ruvia::http_status::not_modified);
    EXPECT_EQ(serve("Sat, 01 Jan 2000 00:00:00 GMT"), ruvia::http_status::ok);
}

TEST(Ruvia, static_file_directory_root_index_and_403) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const ruvia::test::scoped_temp_directory directory("ruvia_static_dir_index");
    const auto& dir = directory.path();
    {
        std::ofstream out(dir / "other.txt", std::ios::binary | std::ios::trunc);
        out << "x";
    }

    const auto serve_root = [](static_root& root) -> ruvia::http_status_code {
        static_file_test_request request;
        request.set_method("GET");
        return with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<ruvia::http_status_code> {
            try {
                co_return (co_await context_value.static_file(root, {.relative_path_ = "", .content_type_ = "text/html"}))
                    .status();
            } catch (const ruvia::http_error& error) {
                co_return error.info().status();
            }
        });
    };

    // A directory root with no configured index is forbidden (never a listing).
    {
        static_root_options options;
        options.index_file_ = "";
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        static_root root(dir, std::move(options));
        EXPECT_EQ(serve_root(root), ruvia::http_status::forbidden);
    }

    // With an index file configured (and present), the directory root serves it.
    {
        std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
        out << "<h1>i</h1>";
        out.close();
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
        options.index_file_ = "index.html";
        static_root root(dir, std::move(options));
        EXPECT_EQ(serve_root(root), ruvia::http_status::ok);
    }
}

TEST(Ruvia, static_file_directory_indexes_survive_path_normalization_storage_growth) {
    namespace fs = std::filesystem;
    constexpr std::string_view index = "directory-index-document.html";
    constexpr std::string_view long_directory = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz/nested-directory";
    const ruvia::test::scoped_temp_directory directory("ruvia_static_normalized_directory_indexes");
    const auto& dir = directory.path();
    fs::create_directories(dir / "nested" / "dir");
    fs::create_directories(dir / long_directory);
    std::ofstream(dir / "nested" / "dir" / index) << "short-index";
    std::ofstream(dir / long_directory / index) << "long-index";

    {
        ruvia::static_root_options options;
        options.index_file_ = index;
        options.resident_max_bytes_ = 64;
        options.resident_budget_bytes_ = 128;
        const ruvia::static_root root(dir, options);
        const std::array<std::pair<std::string_view, std::string_view>, 3> cases{{
            {"nested/dir", "short-index"},
            {"nested\\dir", "short-index"},
            {"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\\nested-directory\\", "long-index"},
        }};
        for (const auto& [path, expected] : cases) {
            SCOPED_TRACE(path);
            static_file_test_request request;
            const auto response = with_static_context(request,
                [&](ruvia::context& ctx) -> ruvia::task<std::pair<ruvia::http_status_code, std::string>> {
                    try {
                        const auto result = co_await ctx.static_file(root, {.relative_path_ = path});
                        co_return std::pair(result.status(), std::string(result.body_bytes()));
                    } catch (const ruvia::http_error& error) {
                        co_return std::pair(error.info().status(), std::string{});
                    }
                });
            EXPECT_EQ(response.first, ruvia::http_status::ok);
            EXPECT_EQ(response.second, expected);
        }
    }
}

#if defined(__linux__)
TEST(Ruvia, file_response_rejects_a_fifo_without_blocking_the_worker) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia_file_fifo");
    const auto& dir = directory.path();
    const auto fifo = dir / "payload.txt";
    EXPECT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

    // Opening a writerless FIFO for reading blocks until a writer appears. The
    // rescuer becomes that writer after a generous delay, so a blocking open
    // fails this case instead of hanging the suite.
    std::mutex mutex;
    std::condition_variable served_signal;
    bool served = false;
    bool rescued = false;
    std::thread rescuer([&] {
        std::unique_lock lock(mutex);
        if (served_signal.wait_for(lock, std::chrono::seconds(5), [&served] { return served; })) {
            return;
        }
        rescued = true;
        lock.unlock();
        const int writer = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
        if (writer >= 0) {
            ::close(writer);
        }
    });

    static_file_test_request request;
    const auto status = with_static_context(request, [&](ruvia::context& context_value) -> ruvia::task<ruvia::http_status_code> {
        try {
            co_return (co_await context_value.file({.path_ = fifo})).status();
        } catch (const ruvia::http_error& error) {
            co_return error.info().status();
        }
    });
    {
        std::lock_guard lock(mutex);
        served = true;
    }
    served_signal.notify_one();
    rescuer.join();

    EXPECT_TRUE(!rescued);
    EXPECT_EQ(status, ruvia::http_status::not_found);
}
#endif

#if !defined(_WIN32)
// A request opens its indexed entry beneath the root without following any
// symlink. A directory replaced by a symlink after indexing is refused even
// when it leads to the very file that was indexed (a hard link made before
// the index, so the identity still matches).
TEST(Ruvia, static_file_refuses_a_directory_swapped_for_a_symlink_after_indexing) {
    namespace fs = std::filesystem;
    using static_file_test::static_file_status;
    const ruvia::test::scoped_temp_directory directory("ruvia_static_symlink_swap");
    const auto& base = directory.path();
    const auto root_dir = base / "root";
    const auto outside = base / "outside";
    fs::create_directories(root_dir / "assets");
    fs::create_directories(outside);
    {
        std::ofstream output(root_dir / "assets" / "a.txt", std::ios::binary);
        output << "indexed";
    }
    fs::create_hard_link(root_dir / "assets" / "a.txt", outside / "a.txt");
    ruvia::static_root root(root_dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    EXPECT_TRUE(static_file_status(root, "assets/a.txt") == ruvia::http_status::ok);

    fs::rename(root_dir / "assets", base / "moved_assets");
    fs::create_directory_symlink(outside, root_dir / "assets");
    EXPECT_TRUE(static_file_status(root, "assets/a.txt") == ruvia::http_status::internal_server_error);
}
#endif

using ruvia::http_header_view;

namespace {

template <typename headers_type = std::initializer_list<http_header_view>>
ruvia::test_request make_request(std::string_view target, const headers_type& headers = {}, std::string_view method = "GET") {
    auto request = ruvia::test_request::method(method, target);
    for (const auto& header : headers) {
        request.header(header.name(), header.value());
    }
    return request;
}

}  // namespace

namespace {

std::optional<std::string_view> negotiate_encoding(
    std::initializer_list<http_header_view> headers,
    std::initializer_list<std::string_view> supported) {
    auto request = make_request("/", headers);
    std::optional<std::string_view> chosen;
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
        co_return;
    });
    return chosen;
}

}  // namespace

TEST(Ruvia, context_request_priority_combines_repeated_field_values_before_parsing) {
    auto request = make_request("/",
        {http_header_view{"Priority", "extra=\"alpha"}, http_header_view{"X-Other", "ignored"},
            http_header_view{"priority", "beta\", u=1, i"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_TRUE(context_value.req().priority().urgency_ == 1);
        EXPECT_TRUE(context_value.req().priority().incremental_);
        co_return;
    });
}

TEST(Ruvia, context_request_cookie_single_lookup_uses_last_duplicate) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1; b=2; a=3"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        EXPECT_TRUE(cookie.has_value());
        EXPECT_EQ(cookie.value_or("missing"), std::string_view("3"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        EXPECT_TRUE(cookie.has_value());
        EXPECT_EQ(cookie.value_or("missing"), std::string_view("1"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_include_repeated_cookie_headers) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2; a=3"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& cookies = context_value.req().cookie_fields();
        EXPECT_EQ(cookies.size(), std::size_t{3});
        if (cookies.size() == 3) {
            EXPECT_EQ(cookies[0].name(), std::string_view("a"));
            EXPECT_EQ(cookies[0].value(), std::string_view("1"));
            EXPECT_EQ(cookies[1].name(), std::string_view("b"));
            EXPECT_EQ(cookies[1].value(), std::string_view("2"));
            EXPECT_EQ(cookies[2].name(), std::string_view("a"));
            EXPECT_EQ(cookies[2].value(), std::string_view("3"));
        }
        const auto latest = cookies.get("a");
        EXPECT_TRUE(latest.has_value());
        EXPECT_EQ(latest.value_or("missing"), std::string_view("3"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_preserve_literal_syntax_and_case_sensitive_names) {
    auto request = make_request("/",
        {http_header_view{"Cookie", "x=first; ignored; = unnamed; q=\"a;b\""},
            http_header_view{"X-Other", "unrelated=value"},
            http_header_view{"cOoKiE", "X=upper; x = last ; empty=;"}});
    const auto response = ruvia::test::with_context(request,
        [](ruvia::context& context) -> ruvia::task<void> {
            EXPECT_EQ(context.req().cookie("x").value_or("missing"), "last");
            EXPECT_EQ(context.req().cookie("X").value_or("missing"), "upper");
            const auto& fields = context.req().cookie_fields();
            const http_header_view expected[] = {
                {"x", "first"}, {"", "unnamed"}, {"q", "\"a"},
                {"X", "upper"}, {"x", "last"}, {"empty", ""}};
            EXPECT_EQ(fields.size(), std::size_t{6});
            if (fields.size() == 6) {
                for (std::size_t i = 0; i != fields.size(); ++i) {
                    EXPECT_EQ(fields[i].name(), expected[i].name());
                    EXPECT_EQ(fields[i].value(), expected[i].value());
                }
            }
            EXPECT_EQ(fields.count("x"), std::size_t{2});
            EXPECT_EQ(fields.get("x").value_or("missing"), "last");
            EXPECT_EQ(fields.get("X").value_or("missing"), "upper");
            EXPECT_EQ(fields.get("").value_or("missing"), "unnamed");
            EXPECT_EQ(context.req().cookie("q").value_or("missing"), "\"a");
            EXPECT_EQ(context.req().cookie("empty").value_or("missing"), "");
            context.respond(context.text(ruvia::static_text("ok")));
            co_return;
        });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_keep_all_values_across_growth_and_cache_reads) {
    std::string value{};
    for (std::size_t i = 0; i != 128; ++i) {
        if (!value.empty()) {
            value.append("; ");
        }
        value.append(std::move(std::string("c").append(std::to_string(i))) + std::move(std::string("=v").append(std::to_string(i))));
    }
    auto request = make_request("/",
        {http_header_view{"Cookie", value}, http_header_view{"Cookie", "c0=latest; tail=end"}});
    const auto response = ruvia::test::with_context(request,
        [](ruvia::context& context) -> ruvia::task<void> {
            {
                const auto before = context.req().cookie("c0");
                const auto& fields = context.req().cookie_fields();
                EXPECT_EQ(fields.size(), std::size_t{130});
                if (fields.size() == 130) {
                    for (std::size_t i = 0; i != 128; ++i) {
                        EXPECT_EQ(fields[i].name(), std::move(std::string("c").append(std::to_string(i))));
                        EXPECT_EQ(fields[i].value(), std::move(std::string("v").append(std::to_string(i))));
                    }
                    EXPECT_EQ(fields[128].name(), "c0");
                    EXPECT_EQ(fields[128].value(), "latest");
                    EXPECT_EQ(fields[129].name(), "tail");
                    EXPECT_EQ(fields[129].value(), "end");
                    const auto first = fields[0].value();
                    EXPECT_EQ(context.req().cookie("c0"), before);
                    EXPECT_EQ(context.req().cookie_fields().get("tail").value_or("missing"), "end");
                    EXPECT_EQ(first, "v0");
                }
                EXPECT_EQ(fields.count("c0"), std::size_t{2});
                EXPECT_EQ(fields.get("c0").value_or("missing"), "latest");
            }
            context.respond(context.text(ruvia::static_text("ok")));
            co_return;
        });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_distinguish_no_pairs_from_missing_values) {
    for (const bool include_header : {false, true}) {
        auto request = ruvia::test_request::get("/");
        if (include_header) {
            request.header("Cookie", "; ignored ; flag ;");
        }
        const auto response = ruvia::test::with_context(request,
            [](ruvia::context& context) -> ruvia::task<void> {
                EXPECT_TRUE(context.req().cookie_fields().empty());
                EXPECT_FALSE(context.req().cookie("missing").has_value());
                EXPECT_FALSE(context.req().cookie_fields().get("flag").has_value());
                context.respond(context.text(ruvia::static_text("empty")));
                co_return;
            });
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body(), "empty");
    }
}

TEST(Ruvia, context_request_query_accessors_agree_on_decoded_values) {
    auto request = make_request("/?a=first&b=2&a=one+two");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto query = context_value.req().query("a");
        EXPECT_TRUE(query.has_value());
        EXPECT_EQ(*query, std::string_view("one two"));

        const auto all = context_value.req().queries("a");
        EXPECT_EQ(all.size(), std::size_t{2});
        EXPECT_EQ(all.back(), std::string_view("one two"));
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(*fields_value.get("a"), std::string_view("one two"));
        const auto repeated = context_value.req().query("a");
        EXPECT_TRUE(repeated.has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_query_views_survive_multivalue_lookups) {
    auto request = make_request("/?z=first&%61=one+two&z=last&a=three&empty=&=unnamed");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto scalar = context_value.req().query("a");
        const auto& fields = context_value.req().query_fields();
        const auto decoded = fields[1].value();
        const auto z = context_value.req().queries("z");
        const auto a = context_value.req().queries("a");
        EXPECT_TRUE(context_value.req().queries("missing").empty());
        const auto empty = context_value.req().queries("empty");
        const auto unnamed = context_value.req().queries("");
        EXPECT_EQ(scalar.value_or("missing"), "three");
        EXPECT_EQ(decoded, "one two");
        EXPECT_EQ(fields.size(), std::size_t{6});
        EXPECT_EQ(fields[0].name(), "z");
        EXPECT_EQ(fields[1].name(), "a");
        EXPECT_EQ(z.size(), std::size_t{2});
        EXPECT_EQ(a.size(), std::size_t{2});
        if (z.size() == 2 && a.size() == 2) {
            EXPECT_EQ(z[0], "first");
            EXPECT_EQ(z[1], "last");
            EXPECT_EQ(a[0], "one two");
            EXPECT_EQ(a[1], "three");
        }
        EXPECT_EQ(empty.size(), std::size_t{1});
        EXPECT_EQ(unnamed.size(), std::size_t{1});
        if (!empty.empty() && !unnamed.empty()) {
            EXPECT_TRUE(empty.front().empty());
            EXPECT_EQ(unnamed.front(), "unnamed");
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_reads_unencoded_fields) {
    auto request = make_request("/?tag=x&page=2");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto tag = context_value.req().query("tag");
        EXPECT_TRUE(tag.has_value());
        EXPECT_EQ(*tag, std::string_view("x"));
        const auto page = context_value.req().query("page");
        EXPECT_TRUE(page.has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_query_preserves_short_decoded_names_and_values) {
    auto request = make_request("/?%61=%31&%62=%32&%61=%33");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(fields_value.size(), std::size_t{3});
        EXPECT_EQ(fields_value[0].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[0].value(), std::string_view("1"));
        EXPECT_EQ(fields_value[1].name(), std::string_view("b"));
        EXPECT_EQ(fields_value[1].value(), std::string_view("2"));
        EXPECT_EQ(fields_value[2].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[2].value(), std::string_view("3"));
        EXPECT_EQ(context_value.req().query("a").value_or("missing"), std::string_view("3"));
        EXPECT_EQ(context_value.req().query("b").value_or("missing"), std::string_view("2"));
        const auto values = context_value.req().queries("a");
        EXPECT_EQ(values.size(), std::size_t{2});
        if (values.size() == 2) {
            EXPECT_EQ(values[0], std::string_view("1"));
            EXPECT_EQ(values[1], std::string_view("3"));
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_list_uses_last_duplicate_like_single_lookup) {
    auto request = make_request("/?a=1&b=2&a=3");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // Single-value lookup resolves a duplicate name to its LAST value.
        EXPECT_EQ(*context_value.req().query("a"), std::string_view("3"));

        // The query field list (used by controller field binding) preserves every
        // decoded occurrence, while its scalar get() agrees with query("a") by
        // taking the last value.
        const auto& list = context_value.req().query_fields();
        EXPECT_EQ(list.size(), std::size_t{3});
        EXPECT_EQ(list[0].name(), std::string_view("a"));
        EXPECT_EQ(list[0].value(), std::string_view("1"));
        EXPECT_EQ(list[1].name(), std::string_view("b"));
        EXPECT_EQ(list[1].value(), std::string_view("2"));
        EXPECT_EQ(list[2].name(), std::string_view("a"));
        EXPECT_EQ(list[2].value(), std::string_view("3"));
        const auto via_list = list.get("a");
        EXPECT_TRUE(via_list.has_value());
        EXPECT_EQ(*via_list, std::string_view("3"));
        EXPECT_EQ(*list.get("b"), std::string_view("2"));

        // queries() still exposes every value in order (getAll semantics).
        const auto all = context_value.req().queries("a");
        EXPECT_EQ(all.size(), std::size_t{2});
        EXPECT_EQ(all[0], std::string_view("1"));
        EXPECT_EQ(all[1], std::string_view("3"));
        co_return;
    });
}

TEST(Ruvia, context_request_queries_use_empty_span_only_for_missing_name) {
    auto request = make_request("/?empty=&flag&value=x");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto empty_value = context_value.req().queries("empty");
        EXPECT_EQ(empty_value.size(), std::size_t{1});
        EXPECT_TRUE(empty_value.front().empty());

        const auto valueless = context_value.req().queries("flag");
        EXPECT_EQ(valueless.size(), std::size_t{1});
        EXPECT_TRUE(valueless.front().empty());

        EXPECT_TRUE(context_value.req().queries("missing").empty());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_merges_multiple_accept_field_lines) {
    const auto accepts = [](std::initializer_list<std::string_view> accept_lines, std::string_view media_type) {
        std::vector<http_header_view> headers{};
        headers.reserve(accept_lines.size());
        for (const auto line : accept_lines) {
            headers.emplace_back("Accept", line);
        }
        auto request = make_request("/", headers);
        bool accepted = false;
        (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            accepted = context_value.req()
                           .negotiate(ruvia::context_request::negotiable_field::media_type, {media_type})
                           .has_value();
            co_return;
        });
        return accepted;
    };

    // No Accept header -> the client accepts anything.
    EXPECT_TRUE(accepts({}, "text/html"));
    // A present but empty Accept list is distinct from an absent field: it has
    // no matching media range. Empty members remain harmless when another field
    // line supplies an actual range.
    EXPECT_TRUE(!accepts({""}, "text/html"));
    EXPECT_TRUE(accepts({"", "text/html"}, "text/html"));
    // A single line behaves as before.
    EXPECT_TRUE(accepts({"text/html"}, "text/html"));
    EXPECT_TRUE(!accepts({"text/html"}, "application/json"));

    // RFC 9110 5.3: two Accept lines are equivalent to their comma-join. A type
    // offered only on the SECOND line must be accepted -- previously the stored
    // known-header slot held one line and the other was ignored.
    EXPECT_TRUE(accepts({"text/html", "application/json"}, "application/json"));
    EXPECT_TRUE(accepts({"text/html", "application/json"}, "text/html"));
    EXPECT_TRUE(!accepts({"text/html", "application/json"}, "image/png"));

    // A q=0 exclusion whose range is more specific than an accepting range on
    // another line must win, exactly as if joined "text/*, text/html;q=0" -- which
    // a naive per-line OR would get wrong.
    EXPECT_TRUE(!accepts({"text/*", "text/html;q=0"}, "text/html"));
    EXPECT_TRUE(accepts({"text/*", "text/html;q=0"}, "text/plain"));
}

TEST(Ruvia, context_request_header_lookup_uses_last_match) {
    auto request = make_request("/", {http_header_view{"X-Trace", "first"}, http_header_view{"x-trace", "second"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_EQ(context_value.req().header("X-Trace"), std::string_view("second"));
        co_return;
    });
}

TEST(Ruvia, context_request_header_lookup_is_case_insensitive_and_presence_aware) {
    auto request = make_request("/", {http_header_view{"X-Empty", ""}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto present_empty = context_value.req().header("x-EMPTY");
        EXPECT_TRUE(present_empty.has_value());
        EXPECT_TRUE(present_empty.value_or("missing").empty());
        EXPECT_TRUE(!context_value.req().header("X-Missing").has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_preserves_exact_extension_method_token) {
    auto request = make_request("/", {}, "PROPFIND");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_EQ(context_value.req().method(), std::string_view("PROPFIND"));
        EXPECT_TRUE(context_value.req().known_method() == ruvia::http_known_method::unknown);
        co_return;
    });
}

TEST(Ruvia, context_request_header_fields_enumerate_every_field_in_order) {
    auto request = make_request("/", {http_header_view{"X-Trace", "a"}, http_header_view{"X-Trace", "b"}, http_header_view{"X-Other", "c"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& headers = context_value.req().header_fields();
        // Enumeration preserves spelling; both lookup surfaces ignore ASCII case.
        EXPECT_EQ(headers.count("x-trace"), std::size_t(2));
        EXPECT_EQ(headers.count("X-Trace"), std::size_t(2));
        EXPECT_EQ(headers[0].name(), std::string_view("X-Trace"));
        EXPECT_EQ(headers[0].value(), std::string_view("a"));
        EXPECT_EQ(headers[1].value(), std::string_view("b"));
        EXPECT_EQ(headers[2].name(), std::string_view("X-Other"));
        // Scalar lookup keeps last-occurrence semantics across the same list.
        EXPECT_EQ(*headers.get("x-trace"), std::string_view("b"));
        // ...while the named lookup still accepts the sent spelling, and both paths
        // agree on last-occurrence-wins for a repeated name.
        EXPECT_EQ(*context_value.req().header("X-Trace"), std::string_view("b"));
        co_return;
    });
}

TEST(Ruvia, context_request_query_fields_enumerate_repeated_names) {
    auto request = make_request("/?tag=x&tag=y&page=2");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& queries = context_value.req().query_fields();
        EXPECT_EQ(queries.size(), std::size_t(3));
        EXPECT_EQ(queries.count("tag"), std::size_t(2));
        EXPECT_EQ(queries[0].value(), std::string_view("x"));
        EXPECT_EQ(queries[1].value(), std::string_view("y"));
        EXPECT_EQ(*queries.get("page"), std::string_view("2"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_encoding_preserves_identity_defaults) {
    EXPECT_EQ(negotiate_encoding({}, {"gzip", "identity"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, *;q=0.2"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, identity;q=0.1"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5"}, {"accept-encoding", "identity;q=0"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "*;q=0"}, {"Accept-Encoding", "identity;q=0.7"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_TRUE(!negotiate_encoding({{"Accept-Encoding", "*;q=0"}}, {"identity"}).has_value());
    EXPECT_TRUE(!negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip"}).has_value());
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "extension;q=0.8, identity;q=0.2"}}, {"identity", "extension"}).value_or("missing"), std::string_view("extension"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip, identity"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("identity"));
}

TEST(Ruvia, context_request_negotiate_encoding_preserves_coding_aliases) {
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip"}}, {"gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"x-gzip"}).value_or("missing"), std::string_view("x-gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip;q=0, *;q=1"}}, {"gzip", "br"}).value_or("missing"), std::string_view("br"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.3"}, {"Accept-Encoding", "X-GZIP;q=0.9, br;q=0.5"}}, {"br", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress"}}, {"compress"}).value_or("missing"), std::string_view("compress"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "compress"}}, {"x-compress"}).value_or("missing"), std::string_view("x-compress"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress;q=0, *;q=1"}}, {"compress", "gzip"}).value_or("missing"), std::string_view("gzip"));
}

TEST(Ruvia, context_request_negotiate_picks_the_client_preferred_media_type) {
    auto request = make_request("/", {http_header_view{"Accept", "text/html;q=0.3, application/json;q=0.9"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // Server order lists html first, but the client prefers json.
        const std::string_view supported[] = {"text/html", "application/json"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("application/json"));

        // A first-acceptable-offer check would have taken html; negotiation
        // ranks the offers by the client's weights instead.
        EXPECT_TRUE(context_value.req()
                .negotiate(ruvia::context_request::negotiable_field::media_type, {"text/html"})
                .has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_reports_no_acceptable_representation) {
    auto request = make_request("/", {http_header_view{"Accept", "image/png"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"text/html", "application/json"};
        // nullopt is the 406 signal, which is why this cannot fall back to front().
        EXPECT_TRUE(!context_value.req()
                .negotiate(ruvia::context_request::negotiable_field::media_type, supported)
                .has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_without_the_field_takes_server_preference) {
    auto request = make_request("/");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"application/json", "text/html"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("application/json"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_language_uses_basic_prefix_filtering) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "fr;q=0.4, en;q=0.8"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // RFC 4647 basic filtering: the range "en" matches the tag "en-US".
        const std::string_view supported[] = {"fr-CA", "en-US"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("en-US"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_honours_explicit_zero_quality_exclusion) {
    auto request = make_request("/", {http_header_view{"Accept-Encoding", "*, gzip;q=0"}});
    // "*" accepts everything except the explicitly excluded, more specific tag.

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"gzip", "br"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("br"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_folds_repeated_field_lines) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "de;q=0.2"}, http_header_view{"Accept-Language", "ja;q=0.9"}});
    // RFC 9110 5.3: equivalent to one comma-joined value.

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"de", "ja"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("ja"));
        co_return;
    });
}

TEST(Ruvia, context_request_query_decodes_names_and_values_in_wire_order) {
    auto request = make_request(
        "/?%61=one+two&%62=three%20four&%61=five+six&plain=borrowed");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(fields_value.size(), std::size_t{4});
        EXPECT_EQ(fields_value[0].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[0].value(), std::string_view("one two"));
        EXPECT_EQ(fields_value[1].name(), std::string_view("b"));
        EXPECT_EQ(fields_value[1].value(), std::string_view("three four"));
        EXPECT_EQ(fields_value[2].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[2].value(), std::string_view("five six"));
        EXPECT_EQ(fields_value[3].value(), std::string_view("borrowed"));

        const auto values = context_value.req().queries("a");
        EXPECT_EQ(values.size(), std::size_t{2});
        if (values.size() == 2) {
            EXPECT_EQ(values[0], std::string_view("one two"));
            EXPECT_EQ(values[1], std::string_view("five six"));
        }
        const auto value = context_value.req().query("a");
        EXPECT_TRUE(value.has_value());
        if (value) {
            EXPECT_EQ(*value, std::string_view("five six"));
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_preserves_many_decoded_values) {
    constexpr std::size_t count = 257;
    std::string target{"/?"};
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            target += '&';
        }
        target += std::move(std::string("%74ag=v+").append(std::to_string(i)));
    }

    auto request = make_request(target);

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        const auto values = context_value.req().queries("tag");
        EXPECT_EQ(fields_value.size(), count);
        EXPECT_EQ(values.size(), count);
        if (fields_value.size() == count && values.size() == count) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto expected = std::move(std::string("v ").append(std::to_string(i)));
                EXPECT_EQ(fields_value[i].name(), std::string_view("tag"));
                EXPECT_EQ(fields_value[i].value(), std::string_view(expected));
                EXPECT_EQ(values[i], std::string_view(expected));
            }
        }
        co_return;
    });
}

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
TEST(Ruvia, context_rejects_unconfigured_worker_clients_consistently) {
    const auto response = ruvia::test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
#ifdef RUVIA_ENABLE_DATABASE
            bool database_rejected = false;
            try {
                static_cast<void>(context_value.db());
            } catch (const ruvia::db_error& error) {
                database_rejected = error.code() == ruvia::db_error::code_type::not_configured;
            }
            EXPECT_TRUE(database_rejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
            bool redis_rejected = false;
            try {
                static_cast<void>(context_value.redis());
            } catch (const ruvia::redis_error& error) {
                redis_rejected = error.code() == ruvia::redis_error::code_type::not_configured;
            }
            EXPECT_TRUE(redis_rejected);
#endif
            co_return;
        });
    (void)response;
}
#endif

TEST(Ruvia, context_session_capability_requires_explicit_middleware_binding) {
    const auto response = ruvia::test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            bool rejected = false;
            try {
                static_cast<void>(context_value.session());
            } catch (const std::logic_error&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);

            co_return;
        });
    (void)response;
}

// Setting response cookies, including prefixes and signatures.

TEST(Ruvia, context_set_cookie_serializes_all_attributes) {
    auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options host{
            .path_ = "/",
            .same_site_ = ruvia::cookie_same_site::strict,
            .priority_ = ruvia::cookie_priority::high,
            .max_age_ = std::chrono::seconds(3600),
            .prefix_ = ruvia::cookie_prefix::host,
            .http_only_ = ruvia::cookie_attribute_policy::emit,
            .secure_ = ruvia::cookie_attribute_policy::emit,
            .partitioned_ = ruvia::cookie_attribute_policy::emit,
        };
        context_value.draft().set_cookie({.name_ = "id", .value_ = "abc", .attributes_ = host});
        const auto host_response = context_value.text("ok");
        EXPECT_EQ(host_response.header("Set-Cookie"),
            std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                             "SameSite=Strict; Priority=High; Partitioned"));

        co_return;
    });

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    const ruvia::cookie_options secure{
        .path_ = "/app",
        .domain_ = "example.com",
        .same_site_ = ruvia::cookie_same_site::none,
        .expires_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890)),
        .prefix_ = ruvia::cookie_prefix::secure,
        .secure_ = ruvia::cookie_attribute_policy::emit,
    };
    auto secure_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(secure_request, [&](ruvia::context& secure_context) -> ruvia::task<void> {
        secure_context.draft().set_cookie({.name_ = "sess", .value_ = "xyz", .attributes_ = secure});
        const auto secure_response = secure_context.text("ok");
        EXPECT_EQ(secure_response.header("Set-Cookie"),
            std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                             "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
        co_return;
    });
}

TEST(Ruvia, context_signed_cookie_handles_large_values_in_request_storage) {
    const std::string value(2048, 'v');
    std::string cookie{};
    const auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context) -> ruvia::task<void> {
        bool signed_value = false;
        try {
            context.draft().set_signed_cookie({.name_ = "session", .value_ = value, .secret_ = "secret"});
            signed_value = true;
        } catch (const std::bad_alloc&) {
        }
        EXPECT_TRUE(signed_value);
        if (signed_value) {
            const auto response = context.text("ok");
            const auto header = response.header("Set-Cookie").value_or("");
            cookie.assign(header.substr(0, header.find(';')));
        }
        co_return;
    });
    EXPECT_TRUE(!cookie.empty());
    if (cookie.empty()) {
        return;
    }
    const auto read = ruvia::test_request::get("/").header("Cookie", cookie);
    (void)ruvia::test::with_context(read, [&](ruvia::context& context) -> ruvia::task<void> {
        std::optional<std::string_view> verified;
        try {
            verified = context.req().signed_cookie({.name_ = "session", .secret_ = "secret"});
        } catch (const std::bad_alloc&) {
        }
        EXPECT_TRUE(verified.has_value());
        if (verified) {
            EXPECT_EQ(*verified, std::string_view(value));
        }
        co_return;
    });
}

TEST(Ruvia, context_set_cookie_preserves_same_name_different_path) {
    auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        ruvia::cookie_options root;
        root.path_ = "/";
        ruvia::cookie_options admin;
        admin.path_ = "/admin";
        context_value.draft().set_cookie({.name_ = "session", .value_ = "root-old", .attributes_ = root});
        context_value.draft().set_cookie({.name_ = "session", .value_ = "admin", .attributes_ = admin});
        context_value.draft().set_cookie({.name_ = "session", .value_ = "root-new", .attributes_ = root});
        const auto response = context_value.text("ok");

        std::size_t count = 0;
        bool has_root_old = false;
        bool has_root_new = false;
        bool has_admin = false;
        for (const auto& header : response.headers()) {
            if (header.name() != std::string_view("Set-Cookie")) {
                continue;
            }
            ++count;
            has_root_old = has_root_old || header.value() == "session=root-old; Path=/";
            has_root_new = has_root_new || header.value() == "session=root-new; Path=/";
            has_admin = has_admin || header.value() == "session=admin; Path=/admin";
        }
        EXPECT_EQ(count, std::size_t{2});
        EXPECT_TRUE(!has_root_old);
        EXPECT_TRUE(has_root_new);
        EXPECT_TRUE(has_admin);
        co_return;
    });
}

TEST(Ruvia, context_signed_cookie_with_prefix_verifies_round_trip) {
    std::string set_cookie{};
    std::string bare{};
    auto write_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(write_request, [&](ruvia::context& write_context) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        write_context.draft().set_signed_cookie(
            {.name_ = "session", .value_ = "user-1", .secret_ = "secret", .attributes_ = options});
        const auto write_response = write_context.text("ok");
        set_cookie = write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view line(set_cookie);
    const auto pair = line.substr(0, line.find(';'));
    EXPECT_TRUE(pair.starts_with("__Host-session="));

    // Present the cookie exactly as a browser sends it back.
    const std::string cookie_field(pair);
    auto read_request = ruvia::test_request::get("/").header("Cookie", cookie_field);

    (void)ruvia::test::with_context(read_request, [&](ruvia::context& read_context) -> ruvia::task<void> {
        const auto verified =
            read_context.req().signed_cookie({.name_ = "__Host-session", .secret_ = "secret"});
        EXPECT_TRUE(verified.has_value());
        EXPECT_EQ(*verified, std::string_view("user-1"));

        co_return;
    });

    // An unprefixed signed cookie keeps verifying under its own name.
    auto bare_write_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(bare_write_request, [&](ruvia::context& bare_write_context) -> ruvia::task<void> {
        bare_write_context.draft().set_signed_cookie({.name_ = "plain", .value_ = "user-2", .secret_ = "secret"});
        const auto bare_write_response = bare_write_context.text("ok");
        bare = bare_write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view bare_line(bare);
    const std::string bare_field(bare_line.substr(0, bare_line.find(';')));
    auto bare_request = ruvia::test_request::get("/").header("Cookie", bare_field);

    (void)ruvia::test::with_context(bare_request, [&](ruvia::context& bare_context) -> ruvia::task<void> {
        const auto bare_verified = bare_context.req().signed_cookie({.name_ = "plain", .secret_ = "secret"});
        EXPECT_TRUE(bare_verified.has_value());
        EXPECT_EQ(*bare_verified, std::string_view("user-2"));
        co_return;
    });
}

TEST(Ruvia, context_delete_cookie_with_prefix_is_response_only) {
    auto request = ruvia::test_request::get("/").header("Cookie", "__Host-session=user-1");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        const auto previous = context_value.req().cookie("__Host-session");
        EXPECT_TRUE(previous.has_value());
        EXPECT_EQ(*previous, std::string_view("user-1"));
        context_value.draft().delete_cookie({.name_ = "session", .attributes_ = options});
        const auto response = context_value.text("deleted");
        const auto set_cookie = response.header("Set-Cookie");
        EXPECT_TRUE(set_cookie.has_value());
        EXPECT_TRUE(set_cookie.value_or(std::string_view{}).starts_with("__Host-session=;"));
        EXPECT_TRUE((set_cookie.value_or(std::string_view{}).find("; Expires=Thu, 01 Jan 1970 00:00:00 GMT") != std::string_view::npos));
        EXPECT_TRUE((set_cookie.value_or(std::string_view{}).find("Max-Age") == std::string_view::npos));
        co_return;
    });
}

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

using ruvia::test::throws_on;

TEST(Ruvia, context_redirect_sets_verbatim_ascii_location_and_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "https://example.com/path?q=1"});
        EXPECT_EQ(response.status(), ruvia::http_status::found);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_accepts_only_redirect_statuses) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        for (const auto status : {ruvia::http_status::moved_permanently, ruvia::http_status::found,
                 ruvia::http_status::see_other, ruvia::http_status::temporary_redirect,
                 ruvia::http_status::permanent_redirect}) {
            EXPECT_EQ(context.redirect({.location_ = "/next", .status_ = status}).status(), status);
        }

        EXPECT_TRUE(throws_on(
            [&] { (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::ok}); }));
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_modified});
        }));
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_found});
        }));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_non_ascii_location) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
        // (scheme, host, path separators) is preserved.
        const auto response =
            context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9"),
                .status_ = ruvia::http_status::temporary_redirect});
        EXPECT_EQ(response.status(), ruvia::http_status::temporary_redirect);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
        EXPECT_EQ(
            response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_existing_percent_escapes_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
        // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
        // become %C3%A9, but the existing escapes must survive intact -- not be
        // double-encoded to "%2520"/"%252F", which would corrupt the target.
        const auto response = context.redirect(
            {.location_ = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
        EXPECT_EQ(
            response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

        // A lone or malformed '%' (not followed by two hex digits) is not a valid
        // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
        const auto malformed =
            context.redirect({.location_ = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
        EXPECT_EQ(
            malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response =
            context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
        EXPECT_EQ(
            response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/items[0]?filter=[x]#section[1]"});
        EXPECT_EQ(response.header("Location"),
            std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

        const auto absolute = context.redirect(
            {.location_ = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
        EXPECT_EQ(absolute.header("Location"),
            std::string_view("https://[2001:db8::1]/items%5B0%5D?filter=%5Bx%5D"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_fragment_separator_and_escapes_fragment_hashes) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"/page#part#nested", "/page#part%23nested"},
            {"/page?x=1#part##nested", "/page?x=1#part%23%23nested"},
            {"https://[::1]/x%20y#part#nested", "https://[::1]/x%20y#part%23nested"},
            {"/目标#片#段", "/%E7%9B%AE%E6%A0%87#%E7%89%87%23%E6%AE%B5"},
            {"#part/nested?x=1", "#part/nested?x=1"},
            {"/page#part%23nested", "/page#part%23nested"},
            {"/page%23part?x=%23#part", "/page%23part?x=%23#part"},
            {"/page#", "/page#"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_escapes_colons_that_would_read_as_a_scheme) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"1foo:bar", "1foo%3Abar"},
            {"a b:c/d:e", "a%20b%3Ac/d:e"},
            {"page:1?q=a:b#x:y", "page:1?q=a:b#x:y"},
            {"mailto:user@example.com", "mailto:user@example.com"},
            {"./a:b", "./a:b"},
            {"/a:b", "/a:b"},
            {"?a:b", "?a:b"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_escapes_at_signs_inside_userinfo) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"http://user@name@host.example/p@q", "http://user%40name@host.example/p@q"},
            {"//a@b@[::1]/", "//a%40b@[::1]/"},
            {"http://user@host.example/", "http://user@host.example/"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_crlf_header_injection) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // A CRLF in the location must not split the response: header-value
        // validation rejects it (the location is ASCII, so it takes the verbatim
        // path straight into the validated header setter).
        bool threw = false;
        try {
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/\r\nX-Injected: y")});
        } catch (const std::exception&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_crlf_even_when_location_needs_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
        }));
        co_return;
    });
}

TEST(Ruvia, context_body_sets_body_and_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::created);
        const auto response = context.body("hello world");
        EXPECT_EQ(response.status(), ruvia::http_status::created);
        EXPECT_EQ(response.body_bytes(), std::string_view("hello world"));
        co_return;
    });
}

TEST(Ruvia, context_dynamic_body_owns_input_and_preserves_lvalue) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        std::string source_value("dynamic body");

        const auto response = context.body(std::string_view(source_value));
        source_value[0] = 'X';

        EXPECT_EQ(response.body_bytes(), std::string_view("dynamic body"));
        EXPECT_EQ(source_value, std::string_view("Xynamic body"));
        co_return;
    });
}

TEST(Ruvia, context_static_text_builders_borrow_static_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        static constexpr std::string_view page = "<b>html</b>";
        constexpr ruvia::static_text body_text("bo\0dy");
        const auto body_response = context.body(body_text);
        const auto text_response = context.text(ruvia::static_text("text"));
        const auto html_response = context.html(ruvia::static_text(page));

        EXPECT_TRUE(body_response.body_bytes() == std::string_view("bo\0dy", 5));
        EXPECT_TRUE(body_response.body_bytes().data() == body_text.view().data());
        EXPECT_TRUE(text_response.body_bytes() == std::string_view("text"));
        EXPECT_EQ(text_response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(html_response.body_bytes() == page);
        EXPECT_TRUE(html_response.body_bytes().data() == page.data());
        EXPECT_EQ(html_response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        co_return;
    });
}

TEST(Ruvia, context_character_array_builders_copy_until_terminator) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        char buffer[64] = {};
        const auto written = std::snprintf(buffer, sizeof(buffer), "count=%d", 7);
        EXPECT_TRUE(written > 0);
        const auto text_response = context.text(buffer);
        const auto html_response = context.html(buffer);
        std::string owned_string = "abc";
        const auto body_response = context.body(owned_string);
        owned_string[0] = 'Z';
        buffer[0] = 'X';

        EXPECT_EQ(text_response.body_bytes(), std::string_view("count=7"));
        EXPECT_TRUE(text_response.body_bytes().data() != buffer);
        EXPECT_EQ(html_response.body_bytes(), std::string_view("count=7"));
        EXPECT_EQ(body_response.body_bytes(), std::string_view("abc"));
        EXPECT_TRUE(body_response.body_bytes().data() != owned_string.data());

        const auto literal_response = context.text("literal");
        EXPECT_EQ(literal_response.body_bytes(), std::string_view("literal"));
        co_return;
    });
}

TEST(Ruvia, context_rejects_informational_and_non_http_final_statuses) {
    {
        (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
            bool threw = false;
            try {
                context.draft().status(ruvia::http_status::early_hints);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            co_return;
        });
    }
    {
        bool threw = false;
        try {
            (void)ruvia::http_status_code::from_value(600);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    }
}

TEST(Ruvia, context_error_normalizes_non_error_status_before_response_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("X-Trace", "1");

        const auto ok = context.error(
            {.status_ = ruvia::http_status::ok, .code_ = "bad", .message_ = "not an error"});
        EXPECT_EQ(ok.status(), ruvia::http_status::internal_server_error);
        EXPECT_TRUE(!ok.header("X-Trace").has_value());

        const auto redirect = context.error({.status_ = ruvia::http_status::temporary_redirect,
            .code_ = "bad",
            .message_ = "not an error"});
        EXPECT_EQ(redirect.status(), ruvia::http_status::internal_server_error);
        co_return;
    });
}

TEST(Ruvia, context_response_metadata_uses_http_response_validation) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        bool threw = false;
        try {
            context.draft().header("Connection", "close,");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        const auto response = context.body("unchanged");
        EXPECT_TRUE(!response.header("Connection").has_value());
        co_return;
    });
}

TEST(Ruvia, context_body_applies_context_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::ok);
        context.draft().header("Content-Type", "text/plain");
        context.draft().header("X-Custom", "v");
        const auto response = context.body("data");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body_bytes(), std::string_view("data"));
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        EXPECT_EQ(response.header("X-Custom"), std::string_view("v"));
        co_return;
    });
}

TEST(Ruvia, context_text_responses_send_nosniff_unless_the_application_set_it) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto borrowed = context.text("plain");
        EXPECT_EQ(borrowed.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(borrowed.header("X-Content-Type-Options"), std::string_view("nosniff"));
        const auto owned = context.text(std::string("plain"));
        EXPECT_EQ(owned.header("X-Content-Type-Options"), std::string_view("nosniff"));
        context.draft().header("X-Content-Type-Options", "application-value");
        const auto configured = context.text("plain");
        EXPECT_EQ(configured.header("X-Content-Type-Options"), std::string_view("application-value"));
        co_return;
    });
}

TEST(Ruvia, context_response_header_transactions_preserve_owned_body_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("X-Added", "context");
        context.draft().header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        context.draft().header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        ruvia::http_response response;
        response.header("Content-Type", "application/octet-stream");
        response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        response.body(std::string(4096, 'x'));
        context.respond(std::move(response));
        EXPECT_EQ(context.response()->body_bytes().size(), std::size_t{4096});
        EXPECT_TRUE(!context.response()->header("X-Added").has_value());
        EXPECT_EQ(context.response()->headers().size(), std::size_t{2});
        co_return;
    });
}

TEST(Ruvia, context_response_header_transactions_preserve_cookie_policy_and_spilled_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("Set-Cookie", "session=new; Path=/");
        context.draft().header("Cache-Control", "no-store");
        ruvia::http_response response;
        response.header("Set-Cookie", "session=old; Path=/");
        response.header("Set-Cookie", "other=kept; Path=/", {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Content-Type", "application/octet-stream");
        response.header("Cache-Control", "private");
        for (int i = 0; i < 12; ++i) {
            response.header(std::move(std::string("X-Field-").append(std::to_string(i))), "value");
        }
        response.body(std::string(4096, 'x'));
        context.respond(std::move(response));
        const auto& result_value = *context.response();
        EXPECT_EQ(result_value.header("Content-Type"), std::string_view("application/octet-stream"));
        EXPECT_EQ(result_value.header("Cache-Control"), std::string_view("private"));
        EXPECT_EQ(result_value.header("Set-Cookie"), std::string_view("session=old; Path=/"));
        std::size_t cookies = 0;
        for (const auto& header : result_value.headers()) {
            if (header.name() == "Set-Cookie") {
                ++cookies;
            }
        }
        EXPECT_EQ(cookies, std::size_t{2});
        for (int i = 0; i < 12; ++i) {
            EXPECT_EQ(result_value.header(std::move(std::string("X-Field-").append(std::to_string(i)))), std::string_view("value"));
        }
        co_return;
    });
}

TEST(Ruvia, context_draft_is_consumed_and_final_edits_do_not_cross_replacement) {
    const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"),
        [](ruvia::context& context) -> ruvia::task<void> {
            bool missing_rejected = false;
            try {
                context.edit_response().header("X-Invalid", "value");
            } catch (const std::logic_error&) {
                missing_rejected = true;
            }
            EXPECT_TRUE(missing_rejected);
            context.draft().status(ruvia::http_status::partial_content);
            context.draft().header("Content-Range", "bytes 0-2/10");
            context.draft().set_cookie({.name_ = "draft", .value_ = "first"});
            context.respond(context.body("abc"));
            context.edit_response().header("ETag", "\"first\"");
            context.draft().header("X-Replacement", "fresh");
            context.respond(context.text("second"));
            EXPECT_TRUE(!context.response()->header("ETag").has_value());
            EXPECT_TRUE(!context.response()->header("Content-Range").has_value());
            co_return;
        });
    EXPECT_EQ(delivered.status(), ruvia::http_status::ok);
    EXPECT_EQ(delivered.body(), std::string_view("second"));
    EXPECT_EQ(delivered.header("X-Replacement"), std::string_view("fresh"));
    EXPECT_TRUE(!delivered.header("Set-Cookie").has_value());
}

TEST(Ruvia, context_policy_is_explicit_and_independent_of_response_selection_time) {
    for (const bool after : {false, true}) {
        const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"),
            [after](ruvia::context& context) -> ruvia::task<void> {
                if (!after) {
                    context.policy().header("X-Policy", "retained");
                    context.policy().set_cookie({.name_ = "session", .value_ = "policy"});
                }
                context.respond(context.text("first"));
                if (after) {
                    context.policy().header("X-Policy", "retained");
                    context.policy().set_cookie({.name_ = "session", .value_ = "policy"});
                }
                context.edit_response().header("X-Discarded", "first-only");
                context.respond(context.error({.status_ = ruvia::http_status::conflict}));
                EXPECT_TRUE(!context.response()->header("X-Policy").has_value());
                co_return;
            });
        EXPECT_EQ(delivered.status(), ruvia::http_status::conflict);
        EXPECT_EQ(delivered.header("X-Policy"), std::string_view("retained"));
        EXPECT_TRUE(!delivered.header("X-Discarded").has_value());
        EXPECT_TRUE(delivered.header("Set-Cookie").value_or("").starts_with("session=policy"));
        std::size_t cookies = 0;
        for (const auto& header : delivered.headers()) {
            if (header.name() == "Set-Cookie") {
                ++cookies;
            }
        }
        EXPECT_EQ(cookies, std::size_t{1});
    }
}

TEST(Ruvia, context_metadata_preserves_repeated_set_cookie_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("Set-Cookie", "a=1", {.mode_ = ruvia::http_response_header_mode::append});
        context.draft().header("Set-Cookie", "b=2", {.mode_ = ruvia::http_response_header_mode::append});
        const auto response = context.body("data");

        std::size_t set_cookie_count = 0;
        for (const auto& header : response.headers()) {
            if (header.name() == std::string_view("Set-Cookie")) {
                ++set_cookie_count;
            }
        }
        EXPECT_EQ(set_cookie_count, std::size_t{2});
        co_return;
    });
}

TEST(Ruvia, context_body_null_gives_empty_body_with_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::no_content);
        const auto response = context.body();
        EXPECT_EQ(response.status(), ruvia::http_status::no_content);
        EXPECT_TRUE(response.body_bytes().empty());
        co_return;
    });
}

TEST(Ruvia, context_body_byte_span_copies_into_response_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const std::byte bytes_value[] = {
            std::byte{0x00},
            std::byte{0x41},
            std::byte{0xff},
        };
        const auto response = context.body(std::span<const std::byte>(bytes_value));
        const auto body = response.body_bytes();

        EXPECT_EQ(body.size(), std::size(bytes_value));
        EXPECT_EQ(body[0], '\0');
        EXPECT_EQ(body[1], 'A');
        EXPECT_EQ(static_cast<unsigned char>(body[2]), 0xff);
        co_return;
    });
}

TEST(Ruvia, context_text_sets_plain_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.text("hello");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("hello"));
        co_return;
    });
}

TEST(Ruvia, context_html_sets_html_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.html("<h1>hi</h1>");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("<h1>hi</h1>"));
        co_return;
    });
}

TEST(Ruvia, context_json_serializes_response_model_with_json_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context_json_response model;
        model.set<"number">(42);
        model.set<"boolean">(true);
        model.set<"real">(3.5);
        const auto response = context.json(model);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(),
            std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_backslash_inside_authority) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // Browsers end the host at '\'; "%5C" would send them to evil.example.
        EXPECT_TRUE(throws_on([&] { (void)context.redirect({.location_ = "http://good.example\\@evil.example/"}); }));
        EXPECT_TRUE(throws_on([&] { (void)context.redirect({.location_ = "//good.example\\@evil.example/"}); }));
        // Outside the authority the backslash is encoded, which only narrows it.
        const auto relative = context.redirect({.location_ = "/\\evil.example"});
        EXPECT_EQ(relative.header("Location"), std::string_view("/%5Cevil.example"));
        const auto in_path = context.redirect({.location_ = "https://good.example/a\\b"});
        EXPECT_EQ(in_path.header("Location"), std::string_view("https://good.example/a%5Cb"));
        co_return;
    });
}

TEST(Ruvia, health_responses_use_context_response_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().header("X-Trace", "health");
        context_value.draft().set_cookie({.name_ = "probe", .value_ = "ok"});

        const auto response = ruvia::make_health_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.header("X-Trace"), std::string_view("health"));
        EXPECT_TRUE(
            response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ok\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_defaults_to_ready) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto response = ruvia::make_readiness_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ready\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_keeps_explicit_failure_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().status(ruvia::http_status::created);

        const auto response =
            ruvia::make_readiness_response(context_value, {
                                                              .state_ = ruvia::readiness_state::unavailable,
                                                              .unavailable_reason_ = "database is unavailable",
                                                          });
        EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(),
            std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_rejects_invalid_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        bool rejected = false;
        try {
            (void)ruvia::make_readiness_response(
                context_value, {.state_ = static_cast<ruvia::readiness_state>(std::uint8_t{0xFF})});
        } catch (const std::invalid_argument& exception) {
            rejected = std::string_view(exception.what()) == "readiness state is invalid";
        }
        EXPECT_TRUE(rejected);
        co_return;
    });
}

namespace {

using ruvia::default_error_code;
using ruvia::http_error;

}  // namespace

TEST(Ruvia, default_error_code_mapping) {
    EXPECT_EQ(
        default_error_code(ruvia::http_status::bad_request), std::string_view("bad_request"));
    EXPECT_EQ(default_error_code(ruvia::http_status::not_found), std::string_view("not_found"));
    EXPECT_EQ(default_error_code(ruvia::http_status::method_not_allowed),
        std::string_view("method_not_allowed"));
    EXPECT_EQ(default_error_code(ruvia::http_status::content_too_large),
        std::string_view("content_too_large"));
}

TEST(Ruvia, http_error_info_round_trips) {
    const http_error error({.status_ = ruvia::http_status::unprocessable_content,
        .code_ = "unprocessable",
        .message_ = "bad fields"});
    const auto info = error.info();
    EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
    EXPECT_EQ(info.code(), std::string_view("unprocessable"));
    EXPECT_EQ(info.message(), std::string_view("bad fields"));
}

TEST(Ruvia, exception_text_describes_any_exception) {
    EXPECT_TRUE(ruvia::exception_text(nullptr).empty());
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(std::runtime_error("plain failure"))),
        std::string("plain failure"));
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(42)), std::string("non-standard exception"));

    const std::system_error generic(std::make_error_code(std::errc::timed_out), "upstream");
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(generic)), std::string(generic.what()));

    // A system-category message is the OS text (UTF-8 on Windows, where what()
    // uses the ANSI code page); the context before it is kept.
    const std::system_error system(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::exception_text(std::make_exception_ptr(system));
    EXPECT_TRUE(text.starts_with("bind: "));
    EXPECT_TRUE(text.size() > std::string_view("bind: ").size());
}

TEST(Ruvia, http_error_preserves_all_validation_issues_beyond_64) {
    ruvia::validator validator_value;
    constexpr std::size_t issue_count = 80;
    for (std::size_t index = 0; index < issue_count; ++index) {
        validator_value.add(std::move(std::string("field_").append(std::to_string(index))), "invalid", std::move(std::string("msg_").append(std::to_string(index))));
    }
    EXPECT_EQ(validator_value.issues().size(), issue_count);

    const http_error error({.status_ = ruvia::http_status::bad_request,
        .code_ = "validation_failed",
        .message_ = "request validation failed",
        .validation_issues_ = validator_value.issues()});
    EXPECT_EQ(error.validation_issues().size(), issue_count);
    EXPECT_EQ(error.info().validation_issues().size(), issue_count);
    if (error.validation_issues().size() == issue_count) {
        EXPECT_EQ(error.validation_issues().front().field(), std::string_view("field_0"));
        EXPECT_EQ(error.validation_issues().back().field(), std::string_view("field_79"));
    }
}

namespace final_response_commit_test {

class invalid_final_response_routes final {
    RUVIA_ROUTES_BEGIN(invalid_final_response_routes)
    RUVIA_GET("/server-upgrade-required-without-upgrade", upgrade_required);
    RUVIA_ROUTES_END

    // 426 must name the required protocol in Upgrade (RFC 9110 15.5.22).
    ruvia::task<ruvia::http_response> upgrade_required(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::upgrade_required);
        co_return context_value.text("upgrade");
    }
};

}  // namespace final_response_commit_test

TEST(Ruvia, server_answers_an_uncommittable_route_response_with_a_closing_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::string response{};
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            // Keep-alive request: only the server's close verdict ends the read.
            const std::string request{"GET /server-upgrade-required-without-upgrade HTTP/1.1\r\nHost: localhost\r\n\r\n"};
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(request), std::move(handler));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
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
    EXPECT_TRUE(response.starts_with("HTTP/1.1 500 "));
    EXPECT_TRUE(response.find("\r\nConnection: close\r\n") != std::string::npos);
}

namespace ruvia::test {

class test_scoped_capability final {
public:
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count) noexcept
        : expired_count_(&expired_count),
          registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept
        : expired_count_(other.expired_count_),
          registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept
        : expired_count_(std::exchange(other.expired_count_, nullptr)),
          registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<test_scoped_capability*>(target);
        ++*capability.expired_count_;
    }

    int* expired_count_;
    ruvia::scoped_capability_registration registration_;
};

struct cold_frame_probe final {
    bool* destroyed_;

    explicit cold_frame_probe(bool& destroyed) noexcept
        : destroyed_(&destroyed) {}

    cold_frame_probe(cold_frame_probe&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

    ~cold_frame_probe() {
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }
};

inline ruvia::task<void> cold_frame_task(cold_frame_probe probe) {
    (void)probe;
    co_return;
}

}  // namespace ruvia::test

using namespace ruvia::test;

// Scoped operations release cold frames and shared registrations exactly once.

TEST(Ruvia, scoped_operation_destruction_destroys_cold_frame) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    {
        auto operation =
            ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
        EXPECT_TRUE(!destroyed);
        (void)operation;
    }
    EXPECT_TRUE(destroyed);
}

TEST(Ruvia, scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability first(scope, expired_count);
    test_scoped_capability moved(std::move(first));
    moved.use();
    scope.close();
    EXPECT_EQ(expired_count, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expired_count = 0;
    ruvia::operation_scope scope;
    test_scoped_capability source(scope, expired_count);
    {
        test_scoped_capability destroyed_early(source);
        destroyed_early.use();
    }
    test_scoped_capability surviving_copy(source);
    scope.close();
    EXPECT_EQ(expired_count, 2);

    bool source_rejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        source_rejected = true;
    }
    EXPECT_TRUE(source_rejected);
}

TEST(Ruvia, scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    auto operation =
        ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    EXPECT_TRUE(!destroyed);
    scope.close();
    EXPECT_TRUE(destroyed);
    (void)operation;
}

namespace security_header_test {

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;

[[nodiscard]] ruvia::dispatch_response security_response(
    const security_headers_config& options = {}, std::string_view existing_frame_options = {}) {
    auto callback = [&](ruvia::context& ctx) -> ruvia::task<void> {
        if (!existing_frame_options.empty()) {
            ctx.draft().header("X-Frame-Options", existing_frame_options);
        }
        ctx.respond(ctx.text("ok"));
        co_return;
    };
    ruvia::test_app app;
    app.use<ruvia::security_headers_middleware>(options);
    app.use<ruvia::test::context_callback<decltype(callback)>>(&callback);
    return app.request(ruvia::test_request::get("/"));
}

}  // namespace security_header_test

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;
using security_header_test::security_response;

TEST(Ruvia, security_headers_default_set) {
    const auto response = security_response();

    EXPECT_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    EXPECT_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    EXPECT_TRUE(!response.header("Strict-Transport-Security").has_value());
    // Modern guidance disables the legacy XSS auditor rather than enabling it.
    EXPECT_EQ(response.header("X-XSS-Protection"), std::string_view("0"));
    // Secure-by-default policy values ship out of the box.
    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    EXPECT_EQ(
        response.header("Referrer-Policy"), std::string_view("strict-origin-when-cross-origin"));
    EXPECT_EQ(response.header("Permissions-Policy"),
        std::string_view("geolocation=(), microphone=(), camera=()"));
}

TEST(Ruvia, security_headers_xss_protection_header_policy_is_explicit) {
    const security_headers_config options{
        .xss_protection_header_ = xss_protection_header_policy::omit,
    };
    const auto response = security_response(options);
    EXPECT_TRUE(!response.header("X-XSS-Protection").has_value());
}

TEST(Ruvia, security_headers_reject_invalid_xss_protection_header_policy) {
    const security_headers_config options{
        .xss_protection_header_ = std::bit_cast<xss_protection_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, security_headers_reject_invalid_default_header_policy) {
    const security_headers_config options{
        .frame_options_header_ = std::bit_cast<default_security_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    security_headers_config options;
    options.content_security_policy_ = "";
    options.referrer_policy_ = "";
    const auto response = security_response(options);
    EXPECT_TRUE(!response.header("Content-Security-Policy").has_value());
    EXPECT_TRUE(!response.header("Referrer-Policy").has_value());
}

TEST(Ruvia, security_headers_disabled_options_omit_headers) {
    security_headers_config options;
    options.frame_options_header_ = default_security_header_policy::omit;
    options.strict_transport_security_header_ = default_security_header_policy::omit;
    const auto response = security_response(options);

    EXPECT_TRUE(!response.header("X-Frame-Options").has_value());
    EXPECT_TRUE(!response.header("Strict-Transport-Security").has_value());
    EXPECT_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

TEST(Ruvia, security_headers_emit_configured_policies) {
    security_headers_config options;
    options.content_security_policy_ = "default-src 'self'";
    options.referrer_policy_ = "no-referrer";
    const auto response = security_response(options);

    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    EXPECT_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

TEST(Ruvia, security_headers_apply_custom_headers) {
    const std::vector<ruvia::security_header> custom_value({
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    });
    security_headers_config options;
    options.custom_headers_ = custom_value;
    const auto response = security_response(options);

    EXPECT_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    EXPECT_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    EXPECT_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

TEST(Ruvia, security_headers_reject_invalid_custom_header_at_construction) {
    bool invalid_name_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"bad name", "value"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_name_rejected = true;
    }
    EXPECT_TRUE(invalid_name_rejected);

    bool invalid_value_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"X-Security", "bad\r\nvalue"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_value_rejected = true;
    }
    EXPECT_TRUE(invalid_value_rejected);
}

TEST(Ruvia, security_headers_respect_existing_header_conflict_policy) {
    // The default preserves a header a handler already set -- the middleware
    // only supplies defaults.
    const auto keep = security_response({}, "SAMEORIGIN");
    EXPECT_EQ(keep.header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    const auto replace = security_response(
        security_headers_config{
            .existing_headers_ = security_header_conflict_policy::replace_existing,
        },
        "SAMEORIGIN");
    EXPECT_EQ(replace.header("X-Frame-Options"), std::string_view("DENY"));
}

TEST(Ruvia, security_headers_reject_invalid_conflict_policy) {
    const security_headers_config options{
        .existing_headers_ = std::bit_cast<security_header_conflict_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

namespace csrf_test {

using ruvia::http_known_method;

bool is_lower_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

template <typename fn_type>
bool throws_invalid(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

struct csrf_outcome final {
    bool accepted_{false};
    bool reseeded_{false};
    std::uint16_t status_{0};
    std::string set_cookie_{};
};

[[nodiscard]] bool set_cookie_has_secure(std::string_view set_cookie) noexcept {
    return set_cookie.find("; Secure") != std::string_view::npos;
}

class csrf_test_middleware final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit csrf_test_middleware(ruvia::csrf_protection* middleware_value)
        : middleware_(middleware_value) {}

    ruvia::task<void> invoke(ruvia::context& ctx, ruvia::next& next_step) {
        co_await middleware_->invoke(ctx, next_step);
    }

private:
    ruvia::csrf_protection* middleware_;
};

class accepted_response final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& ctx, ruvia::next&) {
        ctx.respond(ctx.text("accepted"));
        co_return;
    }
};

csrf_outcome run_csrf(ruvia::csrf_protection& csrf, std::string_view cookie_name,
    std::string_view header_name, http_known_method method, bool with_cookie,
    std::string_view cookie_token, bool with_header, std::string_view header_token) {
    auto request = ruvia::test_request::method(ruvia::known_http_method_token(method), "/");
    if (with_cookie) {
        request.cookie(cookie_name, cookie_token);
    }
    if (with_header) {
        request.header(header_name, header_token);
    }
    ruvia::test_app app;
    app.use<csrf_test_middleware>(&csrf);
    app.use<accepted_response>();
    const auto response = app.request(request);
    const auto cookie = response.header("Set-Cookie");
    return csrf_outcome{
        .accepted_ = response.body() == "accepted",
        .reseeded_ = cookie.has_value(),
        .status_ = response.status().value(),
        .set_cookie_ = cookie ? std::string(*cookie) : std::string{},
    };
}

csrf_outcome run_csrf(http_known_method method, bool with_cookie, std::string_view cookie_token,
    bool with_header, std::string_view header_token) {
    ruvia::csrf_protection csrf;
    return run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", method, with_cookie, cookie_token, with_header,
        header_token);
}

[[nodiscard]] std::string issued_token() {
    const auto outcome = run_csrf(http_known_method::get, false, {}, false, {});
    const auto start = outcome.set_cookie_.find('=') + 1;
    const auto end = outcome.set_cookie_.find(';', start);
    return std::string(std::string_view(outcome.set_cookie_).substr(start, end - start));
}

}  // namespace csrf_test

using csrf_test::is_lower_hex;
using csrf_test::issued_token;
using csrf_test::run_csrf;
using csrf_test::set_cookie_has_secure;
using csrf_test::throws_invalid;
using ruvia::http_known_method;

TEST(Ruvia, csrf_token_is_48_lowercase_hex_chars) {
    const auto token = issued_token();
    EXPECT_EQ(token.size(), std::size_t{48});
    for (const char c : token) {
        EXPECT_TRUE(is_lower_hex(c));
    }
}

TEST(Ruvia, csrf_custom_names_validate_at_construction) {
    EXPECT_TRUE(throws_invalid(
        [] { (void)ruvia::csrf_protection({.cookie_name_ = "", .header_name_ = "X-XSRF-TOKEN"}); }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "bad;name", .header_name_ = "X-XSRF-TOKEN"});
    }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "XSRF-TOKEN", .header_name_ = "Bad Header"});
    }));
}

TEST(Ruvia, csrf_rejects_safelisted_and_ambient_token_fields) {
    for (const std::string_view name : {"Accept", "aCcEpT-LaNgUaGe", "Content-Language",
             "CONTENT-TYPE", "Range", "Origin", "Host", "Content-Length", "Cookie",
             "uSeR-aGeNt", "Sec-Fetch-Site", "sEc-Ch-Ua", "Proxy-Authorization",
             "Access-Control-Request-Headers", "X-Forwarded-For", "Forwarded",
             "X-Real-IP", "Save-Data", "DPR", "X-Requested-With"}) {
        EXPECT_TRUE(throws_invalid([name] {
            (void)ruvia::csrf_protection({.header_name_ = std::string(name)});
        }));
    }
}

TEST(Ruvia, csrf_custom_token_fields_preserve_issue_and_validation_round_trip) {
    for (const std::string_view name : {"X-App-Csrf", "Csrf-Token", "Security-Token"}) {
        ruvia::csrf_protection csrf({.cookie_name_ = "APP-XSRF", .header_name_ = std::string(name)});
        const auto issued = run_csrf(csrf, "APP-XSRF", name, http_known_method::get, false, {}, false, {});
        EXPECT_TRUE(issued.accepted_);
        EXPECT_TRUE(issued.reseeded_);
        const auto start = issued.set_cookie_.find('=') + 1;
        const auto token = std::string_view(issued.set_cookie_).substr(start, issued.set_cookie_.find(';', start) - start);
        const auto accepted = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, true, token);
        EXPECT_TRUE(accepted.accepted_);
        const auto missing = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, false, {});
        EXPECT_EQ(missing.status_, std::uint16_t{403});
        const auto mismatched = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, true, "different");
        EXPECT_EQ(mismatched.status_, std::uint16_t{403});
    }
}

TEST(Ruvia, csrf_middleware_owns_custom_names_after_source_destruction) {
    std::unique_ptr<ruvia::csrf_protection> csrf;
    const std::string cookie_name(80, 'c');
    const std::string header_name(80, 'h');
    {
        ruvia::csrf_protection_config config{
            .cookie_name_ = cookie_name,
            .header_name_ = header_name,
        };
        csrf = std::make_unique<ruvia::csrf_protection>(config);
    }

    const auto safe =
        run_csrf(*csrf, cookie_name, header_name, http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(safe.accepted_);
    EXPECT_TRUE(safe.reseeded_);

    const auto unsafe = run_csrf(*csrf, cookie_name, header_name, http_known_method::post, true,
        "abcdef123456", true, "abcdef123456");
    EXPECT_TRUE(unsafe.accepted_);
    EXPECT_EQ(unsafe.status_, std::uint16_t{200});
}

TEST(Ruvia, csrf_token_is_unpredictable) {
    const auto first = issued_token();
    const auto second = issued_token();
    EXPECT_EQ(first.size(), std::size_t{48});
    EXPECT_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    EXPECT_TRUE(first != second);
}

TEST(Ruvia, csrf_unsafe_method_requires_matching_double_submit) {
    const auto ok = run_csrf(http_known_method::post, true, "abcdef123456", true, "abcdef123456");
    EXPECT_TRUE(ok.accepted_);
    EXPECT_EQ(ok.status_, std::uint16_t{200});

    const auto mismatch =
        run_csrf(http_known_method::post, true, "abcdef123456", true, "DIFFERENTtoken");
    EXPECT_TRUE(!mismatch.accepted_);
    EXPECT_EQ(mismatch.status_, std::uint16_t{403});

    const auto no_header = run_csrf(http_known_method::post, true, "abcdef123456", false, {});
    EXPECT_TRUE(!no_header.accepted_);
    EXPECT_EQ(no_header.status_, std::uint16_t{403});

    const auto no_cookie = run_csrf(http_known_method::post, false, {}, true, "abcdef123456");
    EXPECT_TRUE(!no_cookie.accepted_);
    EXPECT_EQ(no_cookie.status_, std::uint16_t{403});
}

TEST(Ruvia, csrf_safe_method_skips_validation) {
    const auto get = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    EXPECT_TRUE(get.accepted_);
    EXPECT_EQ(get.status_, std::uint16_t{200});

    const auto head = run_csrf(http_known_method::head, true, "one", true, "two");
    EXPECT_EQ(head.status_, std::uint16_t{200});
}

TEST(Ruvia, csrf_safe_method_reseeds_absent_or_empty_cookie) {
    const auto absent = run_csrf(http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(absent.accepted_);
    EXPECT_TRUE(absent.reseeded_);

    const auto empty = run_csrf(http_known_method::get, true, "", false, {});
    EXPECT_TRUE(empty.accepted_);
    EXPECT_TRUE(empty.reseeded_);

    const auto present = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    EXPECT_TRUE(present.accepted_);
    EXPECT_TRUE(!present.reseeded_);
}

TEST(Ruvia, csrf_reseed_cookie_omits_secure_on_plaintext) {
    ruvia::csrf_protection csrf;
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {});
    EXPECT_TRUE(issued.reseeded_);
    EXPECT_TRUE(!set_cookie_has_secure(issued.set_cookie_));
}

TEST(Ruvia, csrf_rejection_reaches_the_error_handler) {
    ruvia::csrf_protection csrf;
    ruvia::test_app app;
    std::string seen_code{};
    app.on_error([seen = &seen_code](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        *seen = std::string(error.code());
        c.draft().status(error.status());
        co_return c.text("handled");
    });
    app.use<csrf_test::csrf_test_middleware>(&csrf);
    app.use<csrf_test::accepted_response>();
    auto request = ruvia::test_request::method("POST", "/");
    request.cookie("XSRF-TOKEN", "abcdef123456");
    request.header("X-XSRF-TOKEN", "DIFFERENTtoken");
    const auto response = app.request(request);
    EXPECT_EQ(response.status(), ruvia::http_status::forbidden);
    EXPECT_EQ(response.body(), std::string_view("handled"));
    EXPECT_EQ(seen_code, std::string("csrf_token_mismatch"));
}

// The per-worker rate-limit table: each key's sliding-window estimate, the
// refusal headers derived from it, slot reclamation, and the overflow policy
// for keys a saturated table cannot place.
//
// Every request pins its arrival at an exact offset from one origin, so the
// window arithmetic includes sub-millisecond arrivals without reading a clock.

namespace {

class rate_limit_table_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/rate-limit-table")
    RUVIA_ROUTES_BEGIN(rate_limit_table_controller)
    RUVIA_GET("/ok", ok);
    RUVIA_GET("/route-limited", ok, ruvia::rate_limit<2, 1000>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ok(ruvia::context& context_value) {
        co_return context_value.text("ok");
    }
};

constexpr std::array<std::string_view, 4> held_addresses{"192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4"};

constexpr std::chrono::steady_clock::time_point window_origin{std::chrono::hours(1)};

[[nodiscard]] ruvia::dispatch_response request_at(ruvia::test_app& app, std::string_view address,
    std::chrono::steady_clock::duration offset, std::string_view target = "/rate-limit-table/ok") {
    return app.request(ruvia::test_request::get(target).remote(address).arrival(window_origin + offset));
}

[[nodiscard]] ruvia::http_status_code request_from(
    ruvia::test_app& app, std::string_view address, std::chrono::steady_clock::duration offset = {}) {
    return request_at(app, address, offset).status();
}

[[nodiscard]] std::optional<std::uint64_t> header_number(
    const ruvia::dispatch_response& response, std::string_view name) {
    const auto text = response.header(name);
    std::uint64_t value = 0;
    if (!text.has_value() || text->empty()) {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) {
        return std::nullopt;
    }
    return value;
}

void configure_saturating_limit(ruvia::test_app& app, ruvia::rate_limit_overflow_policy overflow_policy) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = 2, .window_ = std::chrono::minutes(10), .overflow_policy_ = overflow_policy},
        .capacity_per_worker_ = held_addresses.size()});
}

void configure_window_limit(ruvia::test_app& app, std::size_t max_requests, std::chrono::milliseconds window,
    std::size_t capacity = ruvia::default_rate_limit_capacity_per_worker) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = max_requests, .window_ = window}, .capacity_per_worker_ = capacity});
}

}  // namespace

TEST(Ruvia, rate_limit_saturated_table_denies_new_keys_and_keeps_counting_held_keys) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::deny);
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    // Every slot holds a live entry, so a fifth address cannot be placed.
    EXPECT_TRUE(request_from(app, "192.0.2.5") == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, "192.0.2.6") == ruvia::http_status::too_many_requests);
    // Held keys are still found and counted against their own budget.
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::too_many_requests);
    }
}

TEST(Ruvia, rate_limit_saturated_table_admits_new_keys_under_allow_overflow) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::allow);
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    // An unplaced key is admitted without a counter, so it is never limited.
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(request_from(app, "192.0.2.5") == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, held_addresses.front()) == ruvia::http_status::ok);
    EXPECT_TRUE(request_from(app, held_addresses.front()) == ruvia::http_status::too_many_requests);
}

TEST(Ruvia, rate_limit_budget_spent_before_a_window_boundary_does_not_refill_after_it) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 10, milliseconds(1000));
    constexpr std::string_view client = "198.51.100.1";
    // The first request opens the key's window at 0; the other nine go in its
    // last millisecond.
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    for (int i = 1; i < 10; ++i) {
        EXPECT_TRUE(request_from(app, client, milliseconds(999)) == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, client, milliseconds(999)) == ruvia::http_status::too_many_requests);

    // At the boundary a fixed window would grant ten fresh requests; here the
    // previous ten still count in full, so the estimate sits at the limit.
    const auto at_boundary = request_at(app, client, milliseconds(1000));
    EXPECT_TRUE(at_boundary.status() == ruvia::http_status::too_many_requests);
    // 10 * 999 / 1000 < 10 one millisecond later; draining 10 takes the rest
    // of this window. Both round up to one second.
    EXPECT_TRUE(header_number(at_boundary, "Retry-After") == std::optional<std::uint64_t>(1));
    EXPECT_TRUE(header_number(at_boundary, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1));

    int admitted = 0;
    for (int i = 0; i < 10; ++i) {
        admitted += request_from(app, client, milliseconds(1001)) == ruvia::http_status::ok ? 1 : 0;
    }
    EXPECT_EQ(admitted, 1);

    // With one admitted, 10 * remaining / 1000 + 1 < 10 needs remaining
    // below 900 ms: exactly 100 ms after 1001, which Retry-After rounds up.
    const auto refused = request_at(app, client, milliseconds(1001));
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(1));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The current request fades out across the next window: 999 + 1000 ms.
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(2));
    EXPECT_TRUE(request_from(app, client, milliseconds(1100)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, client, milliseconds(1101)) == ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_admits_steady_traffic_paced_at_the_limit) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 4, milliseconds(400));
    // Gaps longer than window / limit keep the estimate below the limit at
    // every arrival, across three window boundaries.
    for (int i = 0; i < 13; ++i) {
        EXPECT_TRUE(request_from(app, "198.51.100.2", milliseconds(101 * i)) == ruvia::http_status::ok);
    }

    // Gaps of exactly window / limit put the estimate exactly at the limit
    // whenever an arrival coincides with a window start; one millisecond
    // later the previous window has faded enough.
    constexpr std::string_view aligned = "198.51.100.3";
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(request_from(app, aligned, milliseconds(100 * i)) == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, aligned, milliseconds(400)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, aligned, milliseconds(401)) == ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_refusal_headers_follow_the_estimate) {
    ruvia::test_app app;
    configure_window_limit(app, 2, std::chrono::minutes(10));
    constexpr std::string_view client = "198.51.100.4";
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Limit") == std::optional<std::uint64_t>(2));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The spent window carries its full count into the next one, which admits
    // once its first millisecond has passed: 600'001 ms, rounded up.
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(601));
    // The two requests fade out across the next window: zero after 1'200'000 ms.
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1200));
}

TEST(Ruvia, rate_limit_refusal_headers_do_not_saturate_for_the_longest_window) {
    ruvia::test_app app;
    configure_window_limit(app, 1, std::chrono::milliseconds::max());
    constexpr std::string_view client = "198.51.100.5";
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    // Retry: the whole window plus one millisecond, 2^63 ms. Reset: two whole
    // windows, 2^64 - 2 ms, beyond any signed millisecond count. Both are
    // rounded up to seconds without clamping.
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(9'223'372'036'854'776ULL));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(18'446'744'073'709'552ULL));
}

TEST(Ruvia, rate_limit_reset_preserves_fractional_window_origin) {
    using namespace std::chrono;
    ruvia::test_app app;
    configure_window_limit(app, 1, milliseconds(1000));
    constexpr std::string_view client = "198.51.100.7";
    EXPECT_EQ(request_from(app, client, microseconds(900)), ruvia::http_status::ok);
    const auto before_roll = request_at(app, client, milliseconds(1000));
    EXPECT_EQ(before_roll.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(header_number(before_roll, "X-RateLimit-Reset"), std::optional<std::uint64_t>(2));
    const auto at_roll = request_at(app, client, milliseconds(1000) + microseconds(900));
    EXPECT_EQ(at_roll.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(header_number(at_roll, "X-RateLimit-Reset"), std::optional<std::uint64_t>(1));
    EXPECT_EQ(request_from(app, client, milliseconds(1001) + microseconds(900)), ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_reclamation_preserves_fractional_window_origin) {
    using namespace std::chrono;
    ruvia::test_app app;
    configure_window_limit(app, 1, milliseconds(1000), 1);
    EXPECT_EQ(request_from(app, held_addresses[0], microseconds(900)), ruvia::http_status::ok);
    EXPECT_EQ(request_from(app, held_addresses[1], milliseconds(2000)), ruvia::http_status::too_many_requests);
    EXPECT_EQ(request_from(app, held_addresses[1], milliseconds(2000) + microseconds(900)), ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_table_reclaims_a_slot_only_after_two_windows) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 2, milliseconds(1000), held_addresses.size());
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    constexpr std::string_view newcomer = "192.0.2.5";
    EXPECT_TRUE(request_from(app, newcomer) == ruvia::http_status::too_many_requests);
    // After one window each held count has only become its key's previous
    // count, which the estimate still reads.
    EXPECT_TRUE(request_from(app, newcomer, milliseconds(1999)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, newcomer, milliseconds(2000)) == ruvia::http_status::ok);
}

TEST(Ruvia, route_rate_limit_counts_with_the_sliding_window_estimate) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    constexpr std::string_view client = "198.51.100.6";
    constexpr std::string_view target = "/rate-limit-table/route-limited";
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::too_many_requests);
    // rate_limit<2, 1000>: 2 * 1000 / 1000 is still the limit at the
    // boundary, 2 * 999 / 1000 is below it a millisecond later.
    EXPECT_TRUE(request_at(app, client, milliseconds(1000), target).status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_at(app, client, milliseconds(1001), target).status() == ruvia::http_status::ok);
}

namespace happy_eyeballs_tests {

using namespace std::chrono_literals;
using tcp = asio::ip::tcp;

[[nodiscard]] std::optional<bool> localhost_first_is_v6() {
    asio::io_context io;
    tcp::resolver resolver(io);
    std::error_code error;
    const auto addresses = resolver.resolve("localhost", "80", error);
    if (error || addresses.empty()) {
        return std::nullopt;
    }
    bool has_v4 = false;
    bool has_v6 = false;
    for (const auto& entry : addresses) {
        has_v4 = has_v4 || entry.endpoint().address().is_v4();
        has_v6 = has_v6 || entry.endpoint().address().is_v6();
    }
    if (!has_v4 || !has_v6) {
        return std::nullopt;
    }
    return addresses.begin()->endpoint().address().is_v6();
}

[[nodiscard]] tcp::endpoint loopback_endpoint(bool v6, std::uint16_t port) {
    return v6 ? tcp::endpoint(asio::ip::address_v6::loopback(), port)
              : tcp::endpoint(asio::ip::address_v4::loopback(), port);
}

void open_acceptor(tcp::acceptor& acceptor, bool v6) {
    acceptor.open(v6 ? tcp::v6() : tcp::v4());
    if (v6) {
        acceptor.set_option(asio::ip::v6_only(true));
    }
}

// The first localhost family is bound but does not listen. The other family
// answers HTTP, so success requires the client to launch the later family
// while the first attempt remains unresolved or refused.
class opposite_family_peer final {
public:
    opposite_family_peer(asio::io_context& io, const ruvia::worker_ref& worker, bool first_is_v6)
        : io_(io),
          listening_(io),
          unavailable_(io),
          socket_(io),
          done_(worker) {
        const bool listening_is_v6 = !first_is_v6;
        std::vector<tcp::acceptor> skipped{};
        for (std::size_t attempt = 0;; ++attempt) {
            open_acceptor(listening_, listening_is_v6);
            listening_.bind(loopback_endpoint(listening_is_v6, 0));
            listening_.listen();

            open_acceptor(unavailable_, first_is_v6);
            std::error_code ec;
            unavailable_.bind(loopback_endpoint(first_is_v6, port()), ec);
            if (!ec) {
                break;
            }
            std::error_code ignored;
            unavailable_.close(ignored);
            if (ec != asio::error::address_in_use || attempt == 64) {
                throw std::system_error(ec, "bind");
            }
            skipped.push_back(std::move(listening_));
            listening_ = tcp::acceptor(io);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return listening_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = std::move(failure);
            }
            complete_ = true;
            done_.notify();
        });
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        listening_.close(ignored);
        unavailable_.close(ignored);
        socket_.close(ignored);
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> serve() {
        co_await listening_.async_accept(socket_, asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket_, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
    }

    asio::io_context& io_;
    tcp::acceptor listening_;
    tcp::acceptor unavailable_;
    tcp::socket socket_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool complete_{false};
};

[[nodiscard]] ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    opposite_family_peer& peer, std::string_view host,
    std::chrono::milliseconds connect_timeout) {
    const std::string authority = host.find(':') == std::string_view::npos ? std::string(host) : "[" + std::string(host) + "]";
    const std::string base_url = std::string("http://") + authority + std::move(std::string(":").append(std::to_string(peer.port())));
    ruvia::http_client client(attachment.loop(),
        {.base_url_ = base_url,
            .connect_timeout_ = connect_timeout,
            .request_timeout_ = 3s,
            .versions_ = ruvia::http_client_versions{.http2_ = false}});
    std::exception_ptr failure;
    try {
        auto response = co_await client.client().get("/happy-eyeballs").send();
        const auto body = co_await response.stream().read_all();
        const std::string_view body_text(reinterpret_cast<const char*>(body.bytes().data()), body.size());
        EXPECT_TRUE(body_text == "ok");
    } catch (...) {
        failure = std::current_exception();
        peer.stop();
    }
    try {
        co_await client.shutdown();
        co_await peer.wait();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace happy_eyeballs_tests

#if defined(_WIN32)
TEST(Ruvia, http_client_tries_next_address_family_when_first_is_unavailable) {
#else
TEST(Ruvia, http_client_fast_address_failure_immediately_starts_the_next_family) {
#endif
    using namespace happy_eyeballs_tests;
    const auto first_is_v6 = localhost_first_is_v6();
    if (!first_is_v6) {
        // A single-family host has no cross-family race to exercise.
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    // Looks "localhost" up on the pool the loop borrows; declared first, it
    // outlives the attachment.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    auto loop = attachment.loop();
    auto worker = loop.worker();
    opposite_family_peer peer(io, worker, *first_is_v6);
    peer.start();
    // Windows can leave a connect to a bound, non-listening socket pending
    // instead of refusing it immediately. Allow the 250 ms stagger there;
    // the shorter deadline elsewhere still checks immediate failure handling.
#if defined(_WIN32)
    constexpr auto connect_timeout = 2s;
#else
    constexpr auto connect_timeout = 150ms;
#endif
    auto root = loop.start(exercise(attachment, peer, "localhost", connect_timeout));
    attachment.run();
    std::move(root).get();
    lookups.join();
}

TEST(Ruvia, http_client_connects_to_an_ipv6_literal) {
    using namespace happy_eyeballs_tests;
    if (!localhost_first_is_v6()) {
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto loop = attachment.loop();
    auto worker = loop.worker();
    opposite_family_peer peer(io, worker, false);
    peer.start();
    auto root = loop.start(exercise(attachment, peer, "::1", 3s));
    attachment.run();
    std::move(root).get();
}

namespace resolve_cancellation_tests {

using namespace std::chrono_literals;

enum class stop_kind { timeout,
    cancellation,
    client_close,
    worker_stop };

struct held_queries final {
    std::binary_semaphore release_{0};
    std::atomic<unsigned> finished_{0};
    std::atomic<bool> released_{false};
    void release() {
        if (!released_.exchange(true)) {
            release_.release();
        }
    }
};

ruvia::task<void> hold_executor(ruvia::worker_ref worker, std::shared_ptr<held_queries> held) {
    auto result = co_await ruvia::run_blocking(std::move(worker), {}, [held] {
        held->release_.acquire();
        ++held->finished_;
    });
    if (result.status() != ruvia::blocking_status::worker_stopping) {
        std::move(result).take_value();
    }
}

ruvia::task<void> send_query(ruvia::http_client& client, ruvia::stop_token stop,
    std::optional<std::error_code>& code) {
    try {
        auto response = co_await client.client().with_options({.stop_token_ = std::move(stop)}).get("/resolve").send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error& error) {
        code = error.code();
    }
}

struct observation final {
    std::optional<std::error_code> code_;
    bool retired_before_release_{false};
};

ruvia::task<void> exercise(ruvia::event_loop loop, const ruvia::blocking_pool& lookups, stop_kind kind,
    std::shared_ptr<held_queries> held, observation& observed, std::promise<void>& ready,
    std::promise<void>& retired) {
    auto worker = loop.worker();
    ruvia::task_scope occupied(worker);
    // 事件循环借用的单线程池被一个未释放的任务占住，名称查询确定性地排队等待，
    // 不依赖外部 DNS 服务器、网络超时或对系统解析器的进程级修改。
    occupied.spawn(hold_executor(worker, held));
    while (lookups.stats().running_ != 1) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    ruvia::http_client client(loop, {.base_url_ = "http://localhost:1", .connect_timeout_ = kind == stop_kind::timeout ? 30ms : 3s, .request_timeout_ = 3s, .versions_ = ruvia::http_client_versions{.http2_ = false}});
    ruvia::stop_source stop;
    ruvia::task_scope operations(worker);
    std::exception_ptr failure;
    try {
        operations.spawn(send_query(client, stop.token(), observed.code_));
        if (kind != stop_kind::timeout) {
            (void)co_await ruvia::sleep_for(worker, 10ms);
            if (kind == stop_kind::cancellation) {
                stop.request_stop();
            } else if (kind == stop_kind::client_close) {
                client.close();
            } else {
                ready.set_value();
            }
        }
        co_await operations.join();
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    observed.retired_before_release_ = held->finished_ == 0;
    retired.set_value();
    if (kind != stop_kind::worker_stop) {
        held->release();
    }
    co_await occupied.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

[[nodiscard]] ruvia::http_client_config dns_http_config() {
    return {.base_url_ = "http://localhost:1"};
}

}  // namespace resolve_cancellation_tests

TEST(Ruvia, clients_need_a_loop_with_a_blocking_pool_to_look_up_a_dns_host) {
    using namespace resolve_cancellation_tests;
    bool http_rejected = false;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        try {
            ruvia::http_client client(loops.loop(0), dns_http_config());
        } catch (const std::invalid_argument&) {
            http_rejected = true;
        }
        loops.join();
    }
    EXPECT_TRUE(http_rejected);

    // The unified HTTP/WebSocket origin is accepted once the loop borrows a pool.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &lookups});
    {
        ruvia::http_client http(loops.loop(0), dns_http_config());
    }
    loops.join();
    lookups.join();
}

TEST(Ruvia, host_lookup_timeout_cancel_and_shutdown_retire_before_system_work_finishes) {
    using namespace resolve_cancellation_tests;
    for (const auto kind : {stop_kind::timeout, stop_kind::cancellation, stop_kind::client_close, stop_kind::worker_stop}) {
        auto held = std::make_shared<held_queries>();
        observation observed;
        std::promise<void> ready;
        auto ready_result = ready.get_future();
        std::promise<void> retired;
        auto retired_result = retired.get_future();
        // 循环借用查询池，池须声明在循环之前并比它活得久。
        ruvia::blocking_pool lookups({.thread_count_ = 1});
        ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 64, .blocking_pool_ = &lookups});
        auto root = pool.loop(0).start(exercise(pool.loop(0), lookups, kind, held, observed, ready, retired));
        pool.start();
        if (kind == stop_kind::worker_stop) {
            const bool started = ready_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(started);
            pool.stop();
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!started || !ended) {
                held->release();
            }
            pool.join();
            held->release();
        } else {
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!ended) {
                held->release();
            }
            root.wait();
            pool.join();
        }
        lookups.join();
        std::move(root).get();
        EXPECT_TRUE(observed.retired_before_release_);
        const auto expected = kind == stop_kind::timeout        ? std::make_error_code(std::errc::timed_out)
                              : kind == stop_kind::cancellation ? std::make_error_code(std::errc::operation_canceled)
                                                                : ruvia::make_error_code(ruvia::http_client_errc::closing);
        EXPECT_TRUE(observed.code_ == expected);
    }
}

namespace {

ruvia::task<std::string> read_to_end(asio::ip::tcp::socket& stream) {
    std::array<char, 4096> bytes_value{};
    std::string received{};
    for (;;) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            stream.async_read_some(asio::buffer(bytes_value), std::move(handler));
        });
        if (read.error_code() == asio::error::eof) {
            co_return received;
        }
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        received.append(bytes_value.data(), read.result());
    }
}

ruvia::task<void> serve_client_connect(asio::ip::tcp::socket& stream, unsigned mode, std::string& received_value) {
    if (mode == 3) {
        received_value = co_await read_to_end(stream);
        co_return;
    }
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    ruvia::http1_server_request_parse_state parsed;
    parsed.parse_head(head);
    if (!parsed.head_ready() || parsed.request().method() != "CONNECT" || parsed.request().target() != "target.test:443") {
        throw std::runtime_error("invalid CONNECT authority-form");
    }
    if (parsed.request().header("content-length") || parsed.request().header("transfer-encoding")) {
        throw std::runtime_error("CONNECT head has HTTP content framing");
    }
    const auto send = [&](std::string_view bytes_value, bool end = false) -> ruvia::task<void> {
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(stream, asio::buffer(bytes_value), std::move(handler));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
        if (end) {
            std::error_code error;
            stream.shutdown(asio::ip::tcp::socket::shutdown_send, error);
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    if (mode == 2) {
        co_await send("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nConnection: close\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied", true);
        co_return;
    }
    co_await send("HTTP/1.1 200 Connection Established\r\nX-Tunnel: owned-metadata\r\n\r\n");
    if (mode == 1) {
        co_await send(std::string(100003, 's'), true);
    }
    received_value = co_await read_to_end(stream);
    if (mode == 0) {
        co_await send(std::string(100003, 's'), true);
    }
}

void exercise_client_connect(unsigned mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::string received{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
            if (accepted.error_code()) {
                throw std::system_error(accepted.error_code());
            }
            auto socket = std::move(accepted.result());
            co_await serve_client_connect(socket, mode, received);
        };
        tasks.spawn(serve());
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            if (mode == 3) {
                bool unavailable = false;
                try {
                    auto result_value = co_await client.client().connect("target.test:443").protocol("test-datagram").target("/datagrams").open();
                } catch (const ruvia::http_client_error& error) {
                    unavailable = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_unavailable);
                }
                EXPECT_TRUE(unavailable);
            } else {
                auto result_value = co_await client.client().connect("target.test:443").open();
                if (mode == 2) {
                    EXPECT_TRUE(!result_value.tunnel() && result_value.response());
                    if (!result_value.response()) {
                        throw std::runtime_error("missing CONNECT rejection");
                    }
                    EXPECT_TRUE(result_value.response()->status().value() == 407);
                    std::string text{};
                    while (auto bytes = co_await result_value.response()->stream().text()) {
                        text.append(*bytes);
                    }
                    EXPECT_TRUE(text == "denied");
                } else {
                    EXPECT_TRUE(result_value.tunnel() && !result_value.response());
                    if (!result_value.tunnel()) {
                        throw std::runtime_error("CONNECT rejected");
                    }
                    auto tunnel = std::move(*result_value.tunnel());
                    EXPECT_TRUE(tunnel.header("x-tunnel") == "owned-metadata");
                    const auto read_greeting = [&]() -> ruvia::task<void> {
                        std::string greeting{};
                        while (auto bytes = co_await tunnel.read()) {
                            greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                        }
                        EXPECT_TRUE(greeting == std::string(100003, 's'));
                    };
                    if (mode == 1) {
                        co_await read_greeting();
                    }
                    for (unsigned part = 0; part != 8; ++part) {
                        std::string payload_value(16384, 't');
                        auto write = tunnel.write(std::string_view(payload_value));
                        payload_value.assign("mutated");
                        co_await std::move(write);
                    }
                    co_await tunnel.finish();
                    co_await tunnel.finish();
                    if (mode == 0) {
                        co_await read_greeting();
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
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
    if (mode == 3) {
        EXPECT_TRUE(received.empty());
    } else if (mode != 2) {
        EXPECT_TRUE(received == std::string(8 * 16384, 't'));
    }
}

}  // namespace

TEST(Ruvia, http1_client_tunnel_and_rejections_preserve_ownership_and_tcp_half_close) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        exercise_client_connect(mode);
    }
}

namespace {

ruvia::task<std::string> read_head(asio::ip::tcp::socket& stream) {
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    co_return head;
}

ruvia::task<void> write_all(asio::ip::tcp::socket& stream, std::string_view bytes_value) {
    const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_write(stream, asio::buffer(bytes_value), std::move(handler));
    });
    if (written.error_code()) {
        throw std::system_error(written.error_code());
    }
}

}  // namespace

// A proxy that rejects CONNECT without Connection: close may follow the
// rejection with bytes of unknown ownership. The client delivers the rejection
// but never reuses that connection: the next request opens a new one.
TEST(Ruvia, http1_client_rejected_keep_alive_connect_retires_the_proxy_connection) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    unsigned accepted = 0;
    std::string after_rejection{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            for (;;) {
                auto accepted_socket = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
                if (accepted_socket.error_code() == asio::error::operation_aborted) {
                    co_return;
                }
                if (accepted_socket.error_code()) {
                    throw std::system_error(accepted_socket.error_code());
                }
                auto socket = std::move(accepted_socket.result());
                ++accepted;
                const auto head = co_await read_head(socket);
                ruvia::http1_server_request_parse_state parsed;
                parsed.parse_head(head);
                if (accepted == 1) {
                    if (!parsed.head_ready() || parsed.request().method() != "CONNECT" ||
                        parsed.request().target() != "target.test:443") {
                        throw std::runtime_error("expected CONNECT on the first connection");
                    }
                    co_await write_all(socket, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied");
                    // Keep-alive: the client, not the proxy, must end this connection.
                    after_rejection = co_await read_to_end(socket);
                    continue;
                }
                if (!parsed.head_ready() || parsed.request().method() != "GET" || parsed.request().target() != "/after") {
                    throw std::runtime_error("expected the next request on a new connection");
                }
                co_await write_all(socket, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
            }
        };
        tasks.spawn(serve());
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5)});
        try {
            {
                auto result_value = co_await client.client().connect("target.test:443").open();
                EXPECT_TRUE(!result_value.tunnel() && result_value.response());
                if (!result_value.response()) {
                    throw std::runtime_error("missing CONNECT rejection");
                }
                EXPECT_TRUE(result_value.response()->status().value() == 407);
                std::string text{};
                while (auto bytes = co_await result_value.response()->stream().text()) {
                    text.append(*bytes);
                }
                EXPECT_TRUE(text == "denied");
            }
            auto response = co_await client.client().get("/after").send();
            EXPECT_TRUE(response.status().value() == 200);
            std::string text{};
            while (auto bytes = co_await response.stream().text()) {
                text.append(*bytes);
            }
            EXPECT_TRUE(text == "ok");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
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
    EXPECT_EQ(accepted, 2U);
    EXPECT_TRUE(after_rejection.empty());
}

namespace static_files_test {

namespace fs = std::filesystem;

// A throwaway document root mixing a normal asset with hidden files and a
// hidden directory (the .git checkout an operator might accidentally deploy).
void populate_dotfile_root(const fs::path& dir) {
    fs::create_directories(dir / ".git");
    const auto write = [](const fs::path& path, std::string_view body) {
        std::ofstream file(path);
        file << body;
    };
    write(dir / "app.js", "console.log(1)");
    write(dir / ".env", "SECRET=1");
    write(dir / ".htpasswd", "user:hash");
    write(dir / ".backup.json", "{}");
    write(dir / ".git" / "config", "[core]");
}

[[nodiscard]] bool served(const ruvia::static_root& root, std::string_view path) {
    bool found = false;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::get("/"),
        [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                found =
                    (co_await ctx.static_file(root, {.relative_path_ = path})).status() == ruvia::http_status::ok;
            } catch (const ruvia::http_error&) {
                found = false;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return found;
}

struct served_body final {
    ruvia::http_status_code status_{ruvia::http_status::internal_server_error};
    bool file_backed_{false};
    std::string bytes_{};
};

// Serves `path` and reports whether the response names a file for the
// transport or carries its bytes in memory.
[[nodiscard]] served_body serve_body(const ruvia::static_root& root, std::string_view path) {
    served_body result;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::get("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                const auto response = co_await ctx.static_file(root, {.relative_path_ = path});
                result.status_ = response.status();
                result.file_backed_ = response.file_body().has_value();
                result.bytes_.assign(response.body_bytes());
            } catch (const ruvia::http_error& error) {
                result.status_ = error.info().status();
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return result;
}

void populate_resident_root(const fs::path& dir) {
    fs::create_directories(dir / "nested");
    std::ofstream(dir / "small.txt", std::ios::binary) << "small-v1";
    std::ofstream(dir / "nested" / "page.txt", std::ios::binary) << "nested-v1";
    std::ofstream(dir / "large.txt", std::ios::binary) << std::string(64, 'L');
}

}  // namespace static_files_test

using static_files_test::populate_dotfile_root;
using static_files_test::populate_resident_root;
using static_files_test::serve_body;
using static_files_test::served;
namespace fs = std::filesystem;

TEST(Ruvia, static_root_hides_dotfiles_even_under_all_policy) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_dotfiles");
    const auto& dir = directory.path();
    populate_dotfile_root(dir);
    ruvia::static_root_options options;
    // The hidden-path default-deny must still keep secrets out of responses.
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::static_root root(dir, std::move(options));

    EXPECT_TRUE(served(root, "app.js"));
    EXPECT_TRUE(!served(root, ".env"));
    EXPECT_TRUE(!served(root, ".htpasswd"));
    EXPECT_TRUE(!served(root, ".backup.json"));
    EXPECT_TRUE(!served(root, ".git/config"));
}

TEST(Ruvia, static_root_serves_dotfiles_when_opted_in) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_dotfiles");
    const auto& dir = directory.path();
    populate_dotfile_root(dir);
    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    options.dotfiles_ = ruvia::static_dotfile_policy::serve;
    ruvia::static_root root(dir, std::move(options));

    EXPECT_TRUE(served(root, ".env"));
    EXPECT_TRUE(served(root, ".git/config"));
    EXPECT_TRUE(served(root, "app.js"));
}

TEST(Ruvia, static_root_without_resident_opt_in_stays_file_backed_and_fails_closed) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});

    const auto before = serve_body(root, "nested/page.txt");
    EXPECT_EQ(before.status_, ruvia::http_status::ok);
    EXPECT_TRUE(before.file_backed_);

    std::ofstream(dir / "nested" / "page.txt", std::ios::binary | std::ios::trunc) << "nested-v2-longer";
    // A HEAD has no body for the transport to check, so the identity check
    // runs while framing.
    bool head_failed = false;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::head("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                static_cast<void>(co_await ctx.static_file(root, {.relative_path_ = "nested/page.txt"}));
            } catch (const ruvia::http_error& error) {
                head_failed = error.info().status() == ruvia::http_status::internal_server_error;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    EXPECT_TRUE(head_failed);
}

TEST(Ruvia, static_root_resident_opt_in_serves_indexed_bytes_from_memory) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 1024});

    // Rewrite and delete after indexing: resident files keep serving the
    // indexed bytes; the file over resident_max_bytes_ stays on disk.
    std::ofstream(dir / "small.txt", std::ios::binary | std::ios::trunc) << "small-v2-longer";
    fs::remove(dir / "nested" / "page.txt");

    const auto small = serve_body(root, "small.txt");
    EXPECT_EQ(small.status_, ruvia::http_status::ok);
    EXPECT_TRUE(!small.file_backed_);
    EXPECT_EQ(small.bytes_, std::string("small-v1"));

    const auto nested = serve_body(root, "nested/page.txt");
    EXPECT_EQ(nested.status_, ruvia::http_status::ok);
    EXPECT_TRUE(!nested.file_backed_);
    EXPECT_EQ(nested.bytes_, std::string("nested-v1"));

    const auto large = serve_body(root, "large.txt");
    EXPECT_EQ(large.status_, ruvia::http_status::ok);
    EXPECT_TRUE(large.file_backed_);
}

TEST(Ruvia, static_root_resident_budget_bounds_the_kept_bytes) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    // Index order is large.txt, nested/page.txt, small.txt; only the first
    // eligible files that fit the budget stay resident.
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 9});
    EXPECT_TRUE(!serve_body(root, "nested/page.txt").file_backed_);
    EXPECT_TRUE(serve_body(root, "small.txt").file_backed_);
}
