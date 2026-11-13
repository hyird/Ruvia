#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/http_client.h"

#include "test_io_context.h"

namespace client_upgrade_tests {

struct peer_observation final {
    std::string request_;
    std::string payload_;
};

// A real TCP peer writes one coalesced response, half-closes its sending side,
// then observes the client's bytes through EOF. No production protocol parser
// participates on the peer side.
template <typename client_fn_type>
peer_observation exchange(std::string_view wire, client_fn_type&& exercise) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_finished(worker);
    peer_observation observed;
    std::exception_ptr peer_failure;
    bool peer_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        const auto head_bytes = co_await asio::async_read_until(
            socket, asio::dynamic_buffer(observed.request_), "\r\n\r\n", asio::use_awaitable);
        observed.payload_ = observed.request_.substr(head_bytes);
        observed.request_.resize(head_bytes);
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 4096> bytes;
        for (;;) {
            std::error_code error;
            const auto count = co_await socket.async_read_some(
                asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
            observed.payload_.append(bytes.data(), count);
            if (error == asio::error::eof || error == asio::error::connection_reset) {
                co_return;
            }
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url,
                .request_timeout_ = std::chrono::seconds(3),
                .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            co_await exercise(client, worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    return observed;
}

}  // namespace client_upgrade_tests

TEST(Ruvia, http1_client_generic_upgrade_preserves_early_binary_bytes_and_caller_fields) {
    for (const std::string_view protocol : {"chat/7", "websocket"}) {
        SCOPED_TRACE(protocol);
        std::string wire =
            "HTTP/1.1 103 Early Hints\r\n\r\n"
            "HTTP/1.1 101 Switching Protocols\r\nConnection: keep-alive, Upgrade\r\nUpgrade: ";
        wire.append(protocol);
        wire.append("\r\n\r\n");
        constexpr std::string_view early("early\0bytes", 11);
        constexpr std::string_view outgoing("client\0bytes", 12);
        wire.append(early);
        const auto observed = client_upgrade_tests::exchange(wire,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                unsigned informational_count = 0;
                auto request = client.client().get("/generic");
                request.header("Connection", "Upgrade")
                    .header("Upgrade", protocol)
                    .header("Sec-WebSocket-Protocol", "caller-owned")
                    .header("Sec-WebSocket-Key", "caller-owned-key");
                request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token) -> ruvia::task<void> {
                    EXPECT_EQ(status.value(), 103);
                    ++informational_count;
                    co_return;
                }});
                auto opened = co_await request.upgrade();
                EXPECT_EQ(opened.response(), nullptr);
                if (opened.tunnel() == nullptr) {
                    throw std::runtime_error("generic Upgrade did not open a tunnel");
                }
                auto& tunnel = *opened.tunnel();
                EXPECT_EQ(tunnel.status(), ruvia::http_status::switching_protocols);
                EXPECT_EQ(tunnel.header("upgrade"), std::optional(protocol));
                EXPECT_EQ(informational_count, 1u);
                EXPECT_EQ(tunnel.informational_responses().size(), std::size_t{1});
                co_await tunnel.write(outgoing);
                co_await tunnel.finish();
                std::string received;
                while (auto bytes = co_await tunnel.text()) {
                    received.append(*bytes);
                }
                EXPECT_EQ(received, early);
            });
        EXPECT_TRUE(observed.request_.starts_with("GET /generic HTTP/1.1\r\n"));
        EXPECT_NE(observed.request_.find(": caller-owned\r\n"), std::string::npos);
        EXPECT_NE(observed.request_.find(": caller-owned-key\r\n"), std::string::npos);
        EXPECT_EQ(observed.payload_, outgoing);
    }
}

TEST(Ruvia, http1_client_generic_upgrade_rejection_is_an_ordinary_response) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 6\r\nX-Rejected: yes\r\n\r\ndenied",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/rejected");
            request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
            auto result = co_await request.upgrade();
            EXPECT_EQ(result.tunnel(), nullptr);
            if (result.response() == nullptr) {
                throw std::runtime_error("rejected Upgrade lost its HTTP response");
            }
            auto& response = *result.response();
            EXPECT_EQ(response.status().value(), 426);
            EXPECT_EQ(response.header("x-rejected"), std::optional<std::string_view>("yes"));
            EXPECT_EQ(response.framing(), ruvia::http_client_response_framing::fixed_length);
            EXPECT_EQ(response.content_length(), std::optional<std::uint64_t>(6));
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), "denied");
        });
}

TEST(Ruvia, http1_client_generic_upgrade_rejects_invalid_switch_agreement) {
    for (const std::string_view fields : {
             "Upgrade: chat/7\r\n",
             "Connection: Upgrade\r\n",
             "Connection: Upgrade\r\nUpgrade: other/1\r\n",
             "Connection: close, Upgrade\r\nUpgrade: chat/7\r\n"}) {
        SCOPED_TRACE(fields);
        std::string wire = "HTTP/1.1 101 Switching Protocols\r\n";
        wire.append(fields);
        wire.append("\r\nnot-a-tunnel");
        client_upgrade_tests::exchange(wire,
            [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto request = client.client().get("/invalid-switch");
                request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
                bool rejected = false;
                try {
                    auto result = co_await request.upgrade();
                    if (result.tunnel() != nullptr) {
                        result.tunnel()->abort();
                    }
                } catch (const ruvia::http_client_error& error) {
                    rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_error);
                }
                EXPECT_TRUE(rejected);
            });
    }
}

TEST(Ruvia, http1_client_informational_callbacks_backpressure_coalesced_heads) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\nX-Order: first\r\n\r\n"
        "HTTP/1.1 102 Processing\r\nX-Order: second\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
        [](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            ruvia::worker_signal entered(worker);
            ruvia::worker_signal release(worker);
            std::vector<unsigned> statuses;
            bool returned = false;
            std::exception_ptr failure;
            auto request = client.client().get("/hints");
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> headers,
                                                     ruvia::stop_token stop) -> ruvia::task<void> {
                EXPECT_FALSE(stop.stop_requested());
                statuses.push_back(status.value());
                EXPECT_EQ(headers.size(), std::size_t{1});
                const auto expected = statuses.size() == 1 ? "first" : "second";
                entered.notify();
                co_await release.wait();
                if (!headers.empty()) {
                    EXPECT_EQ(headers.front().value(), expected);
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    returned = true;
                    EXPECT_EQ(response.informational_responses().size(), std::size_t{2});
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), "ok");
                } catch (...) {
                    failure = std::current_exception();
                }
                entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            for (std::size_t count = 1; count <= 2; ++count) {
                co_await entered.wait();
                if (failure || returned) {
                    break;
                }
                EXPECT_EQ(statuses.size(), count);
                EXPECT_FALSE(returned);
                release.notify();
            }
            co_await requests.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            EXPECT_EQ(statuses, (std::vector<unsigned>{103, 102}));
            EXPECT_TRUE(returned);
        });
}

TEST(Ruvia, http1_client_informational_callback_failure_terminates_exchange) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/callback-failure");
            request.configure({.informational_ = [](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token) -> ruvia::task<void> {
                throw std::runtime_error("informational callback failed");
                co_return;
            }});
            bool failed = false;
            try {
                auto response = co_await request.send();
                static_cast<void>(response);
            } catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()) == "informational callback failed";
            }
            EXPECT_TRUE(failed);
        });
}

TEST(Ruvia, http1_client_response_framing_comes_from_parser_precedence) {
    struct framing_case final {
        std::string_view method_;
        std::string_view wire_;
        ruvia::http_client_response_framing framing_;
        std::optional<std::uint64_t> length_;
        std::string_view body_;
    };
    using framing = ruvia::http_client_response_framing;
    const framing_case cases[] = {
        {"GET", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\n\r\nok", framing::close_delimited, std::nullopt, "ok"},
        {"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n", framing::none, 42, ""},
        {"GET", "HTTP/1.1 204 No Content\r\n\r\n", framing::none, std::nullopt, ""},
        {"GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, ""},
        {"GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
    };
    for (const auto& scenario : cases) {
        SCOPED_TRACE(scenario.wire_);
        client_upgrade_tests::exchange(scenario.wire_,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto response = co_await client.client().request(scenario.method_, "/framing").send();
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
                const auto body = co_await response.stream().read_all();
                EXPECT_EQ(body.text(), scenario.body_);
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
            });
    }
}

TEST(Ruvia, http1_client_informational_callback_observes_deadline_and_explicit_stop) {
    for (const bool deadline : {false, true}) {
        SCOPED_TRACE(deadline);
        client_upgrade_tests::exchange(
            "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
            [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
                ruvia::stop_source stop;
                ruvia::worker_signal entered(worker);
                const auto handle = client.client().with_options(
                    {.timeout_ = deadline ? std::optional<ruvia::timeout_duration>(std::chrono::milliseconds(500)) : std::nullopt,
                        .stop_token_ = stop.token()});
                auto request = handle.get("/cancel-hints");
                bool callback_stopped = false;
                bool callback_entered = false;
                bool completed = false;
                std::exception_ptr failure;
                std::optional<std::error_code> code;
                request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token token) -> ruvia::task<void> {
                    callback_entered = true;
                    entered.notify();
                    const auto result = co_await ruvia::sleep_for(worker, std::chrono::seconds(2), token);
                    callback_stopped = result == ruvia::timer_sleep_result::stop_requested && token.stop_requested();
                }});
                const auto send = [&]() -> ruvia::task<void> {
                    try {
                        auto response = co_await request.send();
                        static_cast<void>(response);
                    } catch (const ruvia::http_client_error& error) {
                        code = error.code();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    completed = true;
                    entered.notify();
                };
                ruvia::task_scope requests(worker);
                requests.spawn(send());
                while (!callback_entered && !completed) {
                    co_await entered.wait();
                }
                if (!deadline) {
                    stop.request_stop();
                }
                co_await requests.join();
                if (failure) {
                    std::rethrow_exception(failure);
                }
                EXPECT_TRUE(callback_entered);
                EXPECT_TRUE(callback_stopped);
                EXPECT_EQ(code, deadline ? std::make_error_code(std::errc::timed_out)
                                         : std::make_error_code(std::errc::operation_canceled));
            });
    }
}

TEST(Ruvia, http1_client_buffered_upload_receives_hints_before_blocked_write_completes) {
    enum class outcome { complete,
        cancel,
        early_final };
    constexpr std::size_t content_bytes = 16 * 1024 * 1024;
    for (const auto expected : {outcome::complete, outcome::cancel, outcome::early_final}) {
        SCOPED_TRACE(static_cast<int>(expected));
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto& worker = attachment.loop().worker();
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket socket(io);
        asio::steady_timer drain_gate(io);
        drain_gate.expires_after(std::chrono::seconds(10));
        ruvia::worker_signal peer_finished(worker);
        ruvia::worker_signal callback_entered(worker);
        ruvia::worker_signal callback_release(worker);
        std::size_t received_body = 0;
        bool peer_done = false;
        bool stopping = false;
        std::exception_ptr peer_failure;
        const auto serve = [&]() -> asio::awaitable<void> {
            co_await peer.async_accept(socket, asio::use_awaitable);
            socket.set_option(asio::socket_base::receive_buffer_size(4096));
            std::string request;
            const auto head_bytes = co_await asio::async_read_until(
                socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            EXPECT_TRUE(request.starts_with("POST /buffered HTTP/1.1\r\n"));
            received_body = request.size() - head_bytes;
            constexpr std::string_view hints = "HTTP/1.1 103 Early Hints\r\nX-Phase: blocked-write\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(hints), asio::use_awaitable);
            std::error_code gate_error;
            co_await drain_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
            if (stopping) {
                co_return;
            }
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            if (expected == outcome::early_final) {
                constexpr std::string_view rejected = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n";
                co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
                co_return;
            }
            socket.set_option(asio::socket_base::receive_buffer_size(1024 * 1024));
            std::array<char, 65536> bytes;
            while (received_body < content_bytes) {
                const auto count = co_await socket.async_read_some(
                    asio::buffer(bytes.data(), std::min(bytes.size(), content_bytes - received_body)), asio::use_awaitable);
                EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + count, [](char value) { return value == 'b'; }));
                received_body += count;
            }
            constexpr std::string_view accepted = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(accepted), asio::use_awaitable);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
            if (!stopping) {
                peer_failure = error;
            }
            peer_done = true;
            peer_finished.notify();
        });
        std::exception_ptr failure;
        bool saw_callback = false;
        bool completed = false;
        std::optional<std::error_code> error_code;
        const auto run = [&]() -> ruvia::task<void> {
            const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
            ruvia::http_client client(attachment.loop(),
                {.base_url_ = url, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http2_ = false}});
            ruvia::stop_source stop;
            const auto handle = client.client().with_options({.stop_token_ = stop.token()});
            auto request = handle.post("/buffered");
            std::string content(content_bytes, 'b');
            request.body(content);
            std::string().swap(content);
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token token) -> ruvia::task<void> {
                EXPECT_EQ(status.value(), 103);
                EXPECT_LT(received_body, content_bytes);
                EXPECT_LT(handle.stats().plaintext_bytes_sent_, content_bytes);
                saw_callback = true;
                callback_entered.notify();
                co_await callback_release.wait();
                if (expected == outcome::cancel) {
                    EXPECT_TRUE(token.stop_requested());
                } else {
                    drain_gate.cancel();
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    EXPECT_NE(expected, outcome::cancel);
                    EXPECT_EQ(response.status().value(), expected == outcome::early_final ? 413 : 200);
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), expected == outcome::early_final ? "" : "ok");
                } catch (const ruvia::http_client_error& error) {
                    error_code = error.code();
                } catch (...) {
                    failure = std::current_exception();
                }
                completed = true;
                callback_entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            while (!saw_callback && !completed) {
                co_await callback_entered.wait();
            }
            if (expected == outcome::cancel) {
                stop.request_stop();
            }
            callback_release.notify();
            co_await requests.join();
            co_await client.shutdown();
            stopping = true;
            drain_gate.cancel();
            peer.close();
            std::error_code ignored;
            socket.close(ignored);
            while (!peer_done) {
                co_await peer_finished.wait();
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(saw_callback);
        if (expected == outcome::cancel) {
            EXPECT_EQ(error_code, std::make_error_code(std::errc::operation_canceled));
        } else {
            EXPECT_FALSE(error_code.has_value());
        }
        if (expected == outcome::complete) {
            EXPECT_EQ(received_body, content_bytes);
        } else {
            EXPECT_LT(received_body, content_bytes);
        }
    }
}

TEST(Ruvia, http1_client_borrowed_small_chunk_pauses_read_inactivity_until_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    asio::steady_timer tail_gate(io);
    tail_gate.expires_after(std::chrono::seconds(5));
    ruvia::worker_signal peer_finished(worker);
    bool peer_done = false;
    bool stopping = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        co_await peer.async_accept(socket, asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view first = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na";
        co_await asio::async_write(socket, asio::buffer(first), asio::use_awaitable);
        std::error_code gate_error;
        co_await tail_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
        if (!stopping) {
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            co_await asio::async_write(socket, asio::buffer("b", 1), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
        if (!stopping) {
            peer_failure = error;
        }
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url, .read_timeout_ = std::chrono::milliseconds(50), .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            auto response = co_await client.client().get("/slow-consumer").send();
            const auto first = co_await response.stream().text();
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(200));
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            tail_gate.cancel();
            const auto tail = co_await response.stream().text();
            EXPECT_EQ(tail, std::optional<std::string_view>("b"));
            EXPECT_FALSE((co_await response.stream().text()).has_value());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        stopping = true;
        tail_gate.cancel();
        peer.close();
        std::error_code ignored;
        socket.close(ignored);
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
}
