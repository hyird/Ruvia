#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// Pipelined HTTP/1.1 requests: the server may coalesce the responses of
// requests already buffered behind each other, but every response still
// leaves in request order, whatever kind it is, and nothing waits on input the
// client has not sent yet.

namespace http1_pipelining_test {

namespace fs = std::filesystem;

[[nodiscard]] const fs::path& pipelined_file_path() {
    static const fs::path path = fs::temp_directory_path() / "ruvia_http1_pipelining_file.txt";
    return path;
}

inline constexpr std::size_t long_hint_value_bytes = 2000;

class pipelining_routes final {
    RUVIA_ROUTES_BEGIN(pipelining_routes)
    RUVIA_GET("/pipelining/item/:id", item);
    RUVIA_GET("/pipelining/large", large);
    RUVIA_GET("/pipelining/hint", hint);
    RUVIA_GET("/pipelining/long-hint", long_hint);
    RUVIA_GET("/pipelining/file", file);
    RUVIA_POST("/pipelining/echo", echo);
    RUVIA_GET_STREAM("/pipelining/stream", stream);
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

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        co_return c.file({.path_ = pipelined_file_path()});
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto body = "echo-" + std::string(co_await c.req().text());
        co_return c.text(std::string_view(body));
    }

    ruvia::task<void> stream(ruvia::context& c) {
        co_await c.stream().write("streamed-chunk");
    }
};

[[nodiscard]] std::string get(std::string_view target, std::string_view extra = {}) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: localhost\r\n");
    request.append(extra);
    request.append("\r\n");
    return request;
}

// Writes `requests`, then reads until the server closes; nullopt when it is
// still open after 10 s.
[[nodiscard]] std::optional<std::string> exchange_pipelined(
    const asio::ip::tcp::endpoint& endpoint, std::string_view requests) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(requests));
    std::string output;
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

RUVIA_TEST(pipelined_responses_of_every_kind_leave_in_request_order) {
    {
        std::ofstream output(pipelined_file_path(), std::ios::binary | std::ios::trunc);
        output << "file-body";
    }
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string requests;
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
    std::error_code ignored;
    fs::remove(pipelined_file_path(), ignored);

    RUVIA_CHECK(response.has_value());
    const auto output = response.value_or("");
    RUVIA_CHECK(output.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(appear_in_order(output,
        {"item-1", "item-2", "HTTP/1.1 200 ", "item-4", "large-end", "item-5", "streamed-chunk", "item-6",
            "echo-body", "item-7", "HTTP/1.1 103 ", "hinted", "item-8", "file-body", "item-9", "HTTP/1.1 404 ",
            "item-10"}));
    // The HEAD response carries no body.
    RUVIA_CHECK(output.find("item-3") == std::string::npos);
    // Exactly one final response per request, plus the one interim head.
    RUVIA_CHECK_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{17});
    RUVIA_CHECK(output.ends_with("item-10"));
}

RUVIA_TEST(a_long_pipelined_burst_is_answered_completely_and_in_order) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    constexpr std::size_t request_count = 400;
    std::string requests;
    for (std::size_t index = 0; index < request_count; ++index) {
        requests += get("/pipelining/item/" + std::to_string(index),
            index + 1 == request_count ? std::string_view("Connection: close\r\n") : std::string_view{});
    }
    const auto response = exchange_pipelined(server.endpoint(), requests);
    server.finish();

    RUVIA_CHECK(response.has_value());
    const auto text = response.value_or("");
    const std::string_view output = text;
    RUVIA_CHECK_EQ(count_occurrences(output, "HTTP/1.1 200 "), request_count);
    std::size_t position = 0;
    bool ordered = true;
    for (std::size_t index = 0; index < request_count && ordered; ++index) {
        const auto body = "item-" + std::to_string(index);
        const auto head_end = output.find("\r\n\r\n", position);
        ordered = head_end != std::string_view::npos && output.substr(head_end + 4, body.size()) == body;
        position = head_end + 4 + body.size();
    }
    RUVIA_CHECK(ordered);
    RUVIA_CHECK_EQ(position, output.size());
}

RUVIA_TEST(a_long_interim_head_is_sent_whole_between_pipelined_responses) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto response = exchange_pipelined(server.endpoint(),
        get("/pipelining/item/before") + get("/pipelining/long-hint") +
            get("/pipelining/item/after", "Connection: close\r\n"));
    server.finish();

    RUVIA_CHECK(response.has_value());
    const auto output = response.value_or("");
    const auto hint_field = "X-Hint: " + std::string(long_hint_value_bytes, 'h') + "\r\n";
    RUVIA_CHECK(appear_in_order(
        output, {"item-before", "HTTP/1.1 103 ", hint_field, "\r\n\r\n", "long-hinted", "item-after"}));
    RUVIA_CHECK_EQ(count_occurrences(output, "HTTP/1.1 "), std::size_t{4});
}

RUVIA_TEST(held_pipelined_responses_are_sent_before_waiting_for_more_input) {
    // The burst ends inside the next request head: the server must deliver
    // what it has answered before it waits for the rest of that head, or a
    // client that sends the rest only after reading would wait forever.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto first = get("/pipelining/item/first") + get("/pipelining/item/second") + "GET /pipelining/item/third";
    asio::write(socket, asio::buffer(first));
    std::string output;
    bool answered = false;
    asio::async_read_until(socket, asio::dynamic_buffer(output), "item-second",
        [&answered](const std::error_code& error, std::size_t) { answered = !error; });
    io.run_for(std::chrono::seconds(10));
    RUVIA_CHECK(answered);
    RUVIA_CHECK(appear_in_order(output, {"item-first", "item-second"}));
    RUVIA_CHECK(output.find("item-third") == std::string::npos);

    io.restart();
    asio::write(socket, asio::buffer(std::string_view(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&closed](const std::error_code&, std::size_t) {
        closed = true;
    });
    io.run_for(std::chrono::seconds(10));
    RUVIA_CHECK(closed);
    RUVIA_CHECK(output.ends_with("item-third"));
    server.finish();
}

RUVIA_TEST(a_stalled_reader_does_not_block_other_connections_on_its_worker) {
    // The test server has one worker. A client that never reads makes the
    // server's non-blocking write fall back to an asynchronous one; a
    // blocking write would stall the worker and starve the second client.
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket stalled(io);
    stalled.connect(server.endpoint());
    stalled.set_option(asio::socket_base::receive_buffer_size(4096));
    std::string stalled_requests;
    for (int index = 0; index < 64; ++index) {
        stalled_requests += get("/pipelining/large");
    }
    asio::write(stalled, asio::buffer(stalled_requests));

    const auto response =
        exchange_pipelined(server.endpoint(), get("/pipelining/item/live", "Connection: close\r\n"));
    RUVIA_CHECK(response.has_value());
    RUVIA_CHECK(response.value_or("").ends_with("item-live"));

    std::error_code ignored;
    stalled.close(ignored);
    server.finish();
}
