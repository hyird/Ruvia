#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/session.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "http_server_fixture.h"
#include "tcp_port_reservation.h"
#include "test_harness.h"
#include "test_io_context.h"

// Application-wide response policy (CORS, access log, session commit) on the
// heads that do not pass through the buffered response tail: response streams
// and CONNECT/WebSocket handshakes. This executable owns its application, so
// the app-wide CORS policy and access-log hook cannot leak into other suites.

namespace {

struct access_entry final {
    std::string method_;
    std::string path_;
    ruvia::http_status_code status_;
};

std::mutex access_mutex;
std::vector<access_entry> access_entries;

[[maybe_unused]] const bool policy_configured = [] {
    ruvia::app().cors(ruvia::cors_config{}).on_access([](const ruvia::access_log_record& record) noexcept {
        try {
            std::lock_guard lock(access_mutex);
            access_entries.push_back(
                {std::string(record.method()), std::string(record.path()), record.status()});
        } catch (...) {
        }
    });
    return true;
}();

[[nodiscard]] std::vector<access_entry> take_access_entries() {
    std::lock_guard lock(access_mutex);
    return std::exchange(access_entries, {});
}

// Rejects the CONNECT before the tunnel handshake is committed.
class connect_denying_middleware final {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden, .message_ = "denied"});
        co_return;
    }
};

#ifdef RUVIA_ENABLE_REDIS
// Clears the session bound by the enclosing session_middleware, then lets the
// route continue. No cookie was presented, so the commit touches no Redis key.
class session_clearing_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        c.session().clear();
        co_await next_value();
    }
};
#endif

std::atomic<int> head_loop_writes{0};
std::atomic<bool> head_loop_aborted{false};
std::atomic<bool> head_loop_sleep_rejected{false};
std::atomic<bool> head_loop_token_stopped{false};
std::atomic<bool> head_loop_completed{false};
std::atomic<bool> naive_loop_failed{false};
std::atomic<bool> naive_loop_finished{false};
std::atomic<bool> read_after_write_failed{false};
std::atomic<bool> read_after_write_finished{false};
std::mutex read_after_write_mutex;
std::string read_after_write_body;

// Turns every exception from the rest of the chain into a 500, as
// error-wrapping middleware does.
class exception_converting_middleware final {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        try {
            co_await next_value();
        } catch (...) {
            throw ruvia::http_error({.status_ = ruvia::http_status::internal_server_error, .message_ = "converted"});
        }
    }
};

class response_policy_routes final {
    RUVIA_ROUTES_BEGIN(response_policy_routes)
    RUVIA_GET("/policy-stream", stream);
    RUVIA_GET("/policy-secure-stream", stream, ruvia::security_headers_middleware);
    RUVIA_GET("/policy-trailer-stream", trailer_stream);
    RUVIA_GET("/policy-plain", plain);
    RUVIA_GET("/policy-head-loop", head_loop);
    RUVIA_GET("/policy-head-naive", head_naive);
    RUVIA_GET("/policy-head-read-after-write", head_read_after_write);
    RUVIA_GET("/policy-head-convert", stream, exception_converting_middleware);
    RUVIA_GET("/policy-stream-fail-after-commit", stream_fail_after_commit, exception_converting_middleware);
    RUVIA_GET_WS("/policy-websocket", websocket);
    RUVIA_CONNECT("policy-target.test:443", tunnel);
    RUVIA_CONNECT("denied-target.test:443", tunnel, connect_denying_middleware);
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_CONNECT("session-target.test:443", tunnel, (ruvia::http_tunnel_route_config{}),
        ruvia::session_middleware, session_clearing_middleware);
#endif
    RUVIA_ROUTES_END

    ruvia::task<void> stream(ruvia::context& context_value) {
        if (context_value.req().query("fail").has_value()) {
            context_value.draft().header("Content-Range", "bytes 0-2/10");
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
        }
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("chunk");
    }

    ruvia::task<void> stream_fail_after_commit(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        if (!context_value.committed() || !output.committed()) {
            co_return;
        }
        co_await output.write("partial");
        throw std::runtime_error("failed after stream commit");
    }

    // 完成态不是取消：不启动正文生产，误用 write/sleep 必须明确拒绝。
    ruvia::task<void> head_loop(ruvia::context& context_value) {
        if (context_value.req().query("not-modified").has_value()) {
            context_value.draft().status(ruvia::http_status::not_modified);
        }
        auto& output = context_value.stream_text();
        const auto token = context_value.stop_token();
        const auto phase = co_await output.commit();
        if (phase == ruvia::stream_commit_result::body) {
            co_await output.write("tick");
            head_loop_writes.store(1);
        } else {
            head_loop_writes.store(-1);
            try {
                co_await output.write("late");
            } catch (const std::logic_error&) {
                head_loop_writes.store(0);
            }
            try {
                static_cast<void>(co_await output.sleep(std::chrono::seconds(10)));
            } catch (const std::logic_error&) {
                head_loop_sleep_rejected.store(true);
            }
        }
        head_loop_aborted.store(output.aborted());
        head_loop_token_stopped.store(token.stop_requested());
        co_await output.end();
        head_loop_completed.store(true);
    }

    // 即使没有轮询 aborted，显式提交结果也在循环开始前结束 HEAD。
    ruvia::task<void> head_naive(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            naive_loop_finished.store(true);
            co_return;
        }
        try {
            for (int i = 0; i < 1000; ++i) {
                co_await output.write("tick");
                static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)));
            }
        } catch (...) {
            naive_loop_failed.store(true);
            naive_loop_finished.store(true);
            throw;
        }
        naive_loop_finished.store(true);
    }

    ruvia::task<void> head_read_after_write(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        try {
            const auto phase = co_await output.commit();
            if (phase == ruvia::stream_commit_result::body) {
                co_await output.write("first");
            }
            const auto body = co_await context_value.req().text();
            {
                std::lock_guard lock(read_after_write_mutex);
                read_after_write_body.assign(body);
            }
            if (phase == ruvia::stream_commit_result::body) {
                co_await output.write("tick");
            }
        } catch (...) {
            read_after_write_failed.store(true);
            read_after_write_finished.store(true);
            throw;
        }
        read_after_write_finished.store(true);
    }

    ruvia::task<void> trailer_stream(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        context_value.draft().header("Trailer", "X-Complete");
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await output.write("body");
        const std::array<ruvia::http_header_view, 1> trailers{{{"X-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain");
    }

    ruvia::task<void> websocket(ruvia::context&) {
        co_return;
    }

    ruvia::task<void> tunnel(ruvia::context& context_value) {
        co_await context_value.tunnel().finish();
    }
};

// Sends `request` and returns every byte the server wrote before closing, or
// nullopt when the connection is still open after 5 s. Responses end with the
// connection (Connection: close, HTTP/1.0, or a failed CONNECT). Only an
// upgraded exchange -- a 101 or a CONNECT 2xx -- half-closes once its head has
// arrived, so that the tunnel or WebSocket ends; any other early FIN would
// read as the client leaving a streamed response still in flight.
[[nodiscard]] std::optional<std::string> exchange(
    const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output;
    bool closed = false;
    asio::async_read_until(
        socket, asio::dynamic_buffer(output), "\r\n\r\n", [&](const std::error_code& error, std::size_t) {
            if (error) {
                closed = true;
                return;
            }
            const bool upgraded = output.starts_with("HTTP/1.1 101 ") ||
                                  (request.starts_with("CONNECT ") && output.starts_with("HTTP/1.1 2"));
            if (upgraded) {
                std::error_code ignored;
                socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
            }
            asio::async_read(socket, asio::dynamic_buffer(output),
                [&closed](const std::error_code&, std::size_t) { closed = true; });
        });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

// Field names are case-insensitive; compare the head in lower case.
[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

}  // namespace

RUVIA_TEST(http1_response_stream_head_carries_the_application_cors_policy) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto response = exchange(server.endpoint(),
        "GET /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: https://client.test\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(response.has_value());
    const auto output = response.value_or("");
    RUVIA_CHECK(output.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(lower_head(output).find("\r\naccess-control-allow-origin: *") != std::string::npos);
    RUVIA_CHECK(output.find("chunk") != std::string::npos);
}
RUVIA_TEST(http1_cors_reflection_preserves_many_requested_names_in_one_field) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    std::string names;
    for (unsigned i = 0; i != 128; ++i) {
        if (!names.empty()) {
            names.append(", ");
        }
        names.append("x-name-").append(std::to_string(i));
    }
    const auto response = ::exchange(server.endpoint(),
        "OPTIONS /policy-stream HTTP/1.1\r\nHost: localhost\r\n"
        "Origin: https://client.test\r\nAccess-Control-Request-Method: GET\r\n"
        "Access-Control-Request-Headers: " +
            names +
            "\r\nAccess-Control-Request-Headers: x-final\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(response.has_value());
    const auto output = response.value_or("");
    RUVIA_CHECK(output.starts_with("HTTP/1.1 204 "));
    const auto head = lower_head(output);
    const auto expected = "\r\naccess-control-allow-headers: " + names + ", x-final\r\n";
    RUVIA_CHECK(head.find(expected) != std::string::npos);
    const auto first = head.find("\r\naccess-control-allow-headers:");
    RUVIA_CHECK(first != std::string::npos &&
                head.find("\r\naccess-control-allow-headers:", first + 1) == std::string::npos);
}

RUVIA_TEST(http1_cors_invalid_metadata_never_reflects_a_valid_prefix) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const auto& [origin, method, names] : {
             std::array<std::string_view, 3>{"https://client.test/path", "GET", "x-good"},
             {"https://client.test", "BAD METHOD", "x-good"},
             {"https://client.test", "GET", "x-good, bad name"}}) {
        std::string request = "OPTIONS /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: ";
        request.append(origin).append("\r\nAccess-Control-Request-Method: ").append(method).append("\r\nAccess-Control-Request-Headers: ").append(names).append("\r\nConnection: close\r\n\r\n");
        const auto response = ::exchange(server.endpoint(), request);
        RUVIA_CHECK(response.has_value());
        const auto output = response.value_or("");
        RUVIA_CHECK(output.starts_with("HTTP/1.1 204 "));
        const auto head = lower_head(output);
        RUVIA_CHECK(head.find("\r\naccess-control-allow-origin:") == std::string::npos);
        RUVIA_CHECK(head.find("\r\naccess-control-allow-methods:") == std::string::npos);
        RUVIA_CHECK(head.find("\r\naccess-control-allow-headers:") == std::string::npos);
    }
    server.finish();
}

// Trailers are optional metadata: an HTTP/1.0 close-delimited stream drops the
// Trailer field and the trailer section instead of failing with 500.
RUVIA_TEST(http1_response_stream_discards_trailers_it_cannot_frame) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto http10 = exchange(server.endpoint(), "GET /policy-trailer-stream HTTP/1.0\r\nHost: localhost\r\n\r\n");
    const auto http11 = exchange(server.endpoint(),
        "GET /policy-trailer-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(http10.has_value());
    const auto http10_output = http10.value_or("");
    RUVIA_CHECK(http10_output.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(lower_head(http10_output).find("\r\ntrailer:") == std::string::npos);
    RUVIA_CHECK(http10_output.ends_with("\r\n\r\nbody"));
    RUVIA_CHECK(http11.has_value());
    const auto http11_output = http11.value_or("");
    RUVIA_CHECK(lower_head(http11_output).find("\r\ntrailer: x-complete") != std::string::npos);
    RUVIA_CHECK(http11_output.ends_with("0\r\nX-Complete: yes\r\n\r\n"));
}

// RFC 9110 9.3.2: HEAD on a response-stream GET route answers like GET
// without content, and the connection stays framed for the next request. A
// websocket GET route is an upgrade, not a representation, so HEAD stays 405.
RUVIA_TEST(http1_head_falls_back_to_response_stream_get_routes) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto pipelined = exchange(server.endpoint(),
        "HEAD /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: https://client.test\r\n\r\n"
        "HEAD /policy-trailer-stream HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto websocket = exchange(server.endpoint(),
        "HEAD /policy-websocket HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    RUVIA_CHECK(pipelined.has_value());
    const auto output = pipelined.value_or("");
    RUVIA_CHECK(output.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(lower_head(output).find("\r\naccess-control-allow-origin: *") != std::string::npos);
    RUVIA_CHECK(output.find("chunk") == std::string::npos);
    RUVIA_CHECK(output.find("body") == std::string::npos);
    const auto second = output.find("HTTP/1.1 200 ", 1);
    RUVIA_CHECK(second != std::string::npos);
    const auto third = output.find("HTTP/1.1 200 ", second == std::string::npos ? output.size() : second + 1);
    RUVIA_CHECK(third != std::string::npos);
    RUVIA_CHECK(output.ends_with("\r\n\r\nplain"));

    RUVIA_CHECK(websocket.has_value());
    const auto websocket_output = websocket.value_or("");
    RUVIA_CHECK(websocket_output.starts_with("HTTP/1.1 405 "));
    RUVIA_CHECK(lower_head(websocket_output).find("\r\nallow: get, options") != std::string::npos);
}

// 完成头后不生产正文；非法写/等待明确失败，正常完成仍不是 abort。
RUVIA_TEST(http1_head_only_stream_completes_without_cancellation) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto output = exchange(server.endpoint(),
        "HEAD /policy-head-loop HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    RUVIA_CHECK(output.has_value());
    const auto text = output.value_or("");
    RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(text.find("tick") == std::string::npos);
    RUVIA_CHECK(text.ends_with("\r\n\r\nplain"));
    RUVIA_CHECK(text.find("late") == std::string::npos);
    RUVIA_CHECK_EQ(head_loop_writes.load(), 0);
    RUVIA_CHECK(!head_loop_aborted.load());
    RUVIA_CHECK(!head_loop_token_stopped.load());
    RUVIA_CHECK(head_loop_sleep_rejected.load());
    RUVIA_CHECK(head_loop_completed.load());
}

RUVIA_TEST(http1_not_modified_stream_commit_completes_without_abort) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto output = exchange(server.endpoint(),
        "GET /policy-head-loop?not-modified HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(output.has_value());
    RUVIA_CHECK(output.value_or("").starts_with("HTTP/1.1 304 "));
    RUVIA_CHECK(output.value_or("").ends_with("\r\n\r\n"));
    RUVIA_CHECK_EQ(head_loop_writes.load(), 0);
    RUVIA_CHECK(!head_loop_aborted.load());
    RUVIA_CHECK(!head_loop_token_stopped.load());
}

RUVIA_TEST(http1_security_policy_covers_stream_head_and_precommit_error) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const auto request : {
             "GET /policy-secure-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             "HEAD /policy-secure-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             "GET /policy-secure-stream?fail HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"}) {
        const auto output = exchange(server.endpoint(), request);
        RUVIA_CHECK(output.has_value());
        RUVIA_CHECK(output.value_or("").find("X-Frame-Options: DENY\r\n") != std::string::npos);
        RUVIA_CHECK(output.value_or("").find("Content-Range:") == std::string::npos);
    }
    server.finish();
}

// HEAD 在提交时完成，不等待客户端断开，也不启动无限正文循环。
RUVIA_TEST(http1_head_only_producer_finishes_at_commit) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view request : {
             std::string_view("HEAD /policy-head-naive HTTP/1.1\r\nHost: localhost\r\n\r\n"),
             std::string_view("HEAD /policy-head-naive HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"),
         }) {
        naive_loop_failed.store(false);
        naive_loop_finished.store(false);
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(server.endpoint());
            asio::write(socket, asio::buffer(request));
            std::string head;
            asio::read_until(socket, asio::dynamic_buffer(head), "\r\n\r\n");
            RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
            socket.close();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!naive_loop_finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        RUVIA_CHECK(naive_loop_finished.load());
        RUVIA_CHECK(!naive_loop_failed.load());
    }

    read_after_write_failed.store(false);
    read_after_write_finished.store(false);
    {
        std::lock_guard lock(read_after_write_mutex);
        read_after_write_body.clear();
    }
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(server.endpoint());
        asio::write(socket,
            asio::buffer(std::string_view(
                "HEAD /policy-head-read-after-write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n")));
        std::string head;
        asio::read_until(socket, asio::dynamic_buffer(head), "\r\n\r\n");
        RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
        asio::write(socket, asio::buffer(std::string_view("hello")));
        socket.close();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!read_after_write_finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    RUVIA_CHECK(read_after_write_finished.load());
    RUVIA_CHECK(!read_after_write_failed.load());
    {
        std::lock_guard lock(read_after_write_mutex);
        RUVIA_CHECK_EQ(read_after_write_body, "hello");
    }
    server.finish();
}

// Head-only completion raises nothing: middleware converting every exception
// into a 500 sees a normal success, so HEAD on a stream route completes and
// the connection stays usable for the pipelined GET.
RUVIA_TEST(http1_head_only_stream_is_a_success_for_error_converting_middleware) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto output = exchange(server.endpoint(),
        "HEAD /policy-head-convert HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    RUVIA_CHECK(output.has_value());
    const auto text = output.value_or("");
    RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(text.find("chunk") == std::string::npos);
    RUVIA_CHECK(text.find("HTTP/1.1 500 ") == std::string::npos);
    RUVIA_CHECK(text.ends_with("\r\n\r\nplain"));
}

RUVIA_TEST(http1_stream_failure_after_commit_is_not_converted_to_buffered_response_by_middleware) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto output = exchange(server.endpoint(),
        "GET /policy-stream-fail-after-commit HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(output.has_value());
    const auto text = output.value_or("");
    RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(text.find("partial") != std::string::npos);
    RUVIA_CHECK(text.find("0\r\n\r\n") == std::string::npos);
}

// A failed CONNECT closes the connection: bytes the client already sent for
// the tunnel are never parsed as another request (RFC 9110 9.3.6).
RUVIA_TEST(http1_failed_connect_closes_instead_of_parsing_tunnel_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view authority : {"unrouted-target.test:443", "denied-target.test:443"}) {
        const std::string request = std::string("CONNECT ") + std::string(authority) + " HTTP/1.1\r\nHost: " +
                                    std::string(authority) + "\r\n\r\nGET /policy-plain HTTP/1.1\r\nHost: smuggled\r\n\r\n";
        const auto output = ::exchange(server.endpoint(), request);
        RUVIA_CHECK(output.has_value());
        const auto text = output.value_or("");
        RUVIA_CHECK(!text.starts_with("HTTP/1.1 2"));
        RUVIA_CHECK(lower_head(text).find("\r\nconnection: close") != std::string::npos);
        RUVIA_CHECK(text.find("HTTP/1.1 ", 1) == std::string::npos);
    }
    server.finish();
}

// TE without the TE connection option only fails to advertise trailers; the
// request itself is served.
RUVIA_TEST(http1_te_without_connection_option_is_ignored) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto output = exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nTE: trailers\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(output.has_value());
    RUVIA_CHECK(output.value_or("").starts_with("HTTP/1.1 200 "));
}

RUVIA_TEST(http1_successful_upgrades_are_access_logged_once_with_their_handshake_status) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    static_cast<void>(take_access_entries());

    const auto tunnel = exchange(server.endpoint(),
        "CONNECT policy-target.test:443 HTTP/1.1\r\nHost: policy-target.test:443\r\n\r\n");
    RUVIA_CHECK(tunnel.has_value());
    RUVIA_CHECK(tunnel.value_or("").starts_with("HTTP/1.1 200 "));

    const auto websocket = exchange(server.endpoint(),
        "GET /policy-websocket HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    RUVIA_CHECK(websocket.has_value());
    RUVIA_CHECK(websocket.value_or("").starts_with("HTTP/1.1 101 "));
    server.finish();

    const auto entries = take_access_entries();
    RUVIA_CHECK_EQ(entries.size(), std::size_t{2});
    if (entries.size() == 2) {
        RUVIA_CHECK_EQ(entries[0].method_, std::string("CONNECT"));
        RUVIA_CHECK(entries[0].status_ == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(entries[1].method_, std::string("GET"));
        RUVIA_CHECK_EQ(entries[1].path_, std::string("/policy-websocket"));
        RUVIA_CHECK(entries[1].status_ == ruvia::http_status::switching_protocols);
    }
}

// Pipelined responses may leave in one coalesced write, after the requests
// that produced them were already replaced by later ones; each still gets
// exactly one record, in request order, with its own method, path and status.
RUVIA_TEST(http1_pipelined_responses_are_access_logged_in_order) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    static_cast<void>(take_access_entries());

    const auto output = exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD /policy-plain HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-missing HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(output.has_value());
    const auto text = output.value_or("");
    RUVIA_CHECK(text.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(text.ends_with("plain"));

    const auto entries = take_access_entries();
    RUVIA_CHECK_EQ(entries.size(), std::size_t{4});
    if (entries.size() == 4) {
        RUVIA_CHECK_EQ(entries[0].method_, std::string("GET"));
        RUVIA_CHECK_EQ(entries[0].path_, std::string("/policy-plain"));
        RUVIA_CHECK(entries[0].status_ == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(entries[1].method_, std::string("HEAD"));
        RUVIA_CHECK_EQ(entries[1].path_, std::string("/policy-plain"));
        RUVIA_CHECK(entries[1].status_ == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(entries[2].method_, std::string("GET"));
        RUVIA_CHECK_EQ(entries[2].path_, std::string("/policy-missing"));
        RUVIA_CHECK(entries[2].status_ == ruvia::http_status::not_found);
        RUVIA_CHECK_EQ(entries[3].path_, std::string("/policy-plain"));
        RUVIA_CHECK(entries[3].status_ == ruvia::http_status::ok);
    }
}

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(http1_connect_handshake_carries_the_committed_session_cookie) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto tunnel = exchange(server.endpoint(),
        "CONNECT session-target.test:443 HTTP/1.1\r\nHost: session-target.test:443\r\n\r\n");
    server.finish();
    RUVIA_CHECK(tunnel.has_value());
    const auto output = tunnel.value_or("");
    RUVIA_CHECK(output.starts_with("HTTP/1.1 200 "));
    // The cleared session's expiring cookie is part of the tunnel's only head.
    RUVIA_CHECK(lower_head(output).find("\r\nset-cookie: sid=;") != std::string::npos);
}
#endif

RUVIA_TEST(http_server_fixture_reservations_stay_bound_without_accepting_clients) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    ruvia::test::tcp_port_reservation first(io);
    ruvia::test::tcp_port_reservation second(io);
    RUVIA_CHECK(first.endpoint() != second.endpoint());
    RUVIA_CHECK(first.endpoint() != server.endpoint());
    RUVIA_CHECK(second.endpoint() != server.endpoint());

    std::error_code error;
#if !defined(_WIN32)
    // POSIX: a reservation stays bound, including before the real server starts.
    // Windows keeps only a named lease so the server's exclusive bind can succeed.
    asio::ip::tcp::acceptor contender(io);
    contender.open(asio::ip::tcp::v4());
    contender.bind(first.endpoint(), error);
    RUVIA_CHECK(error == asio::error::address_in_use);
#endif

    // It must not be a listener that can steal a client's connection.
    asio::ip::tcp::socket client(io);
    client.connect(first.endpoint(), error);
    RUVIA_CHECK(error == asio::error::connection_refused);

    const auto response = exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    RUVIA_CHECK(response.has_value());
    RUVIA_CHECK(response.value_or("").ends_with("\r\n\r\nplain"));
}
