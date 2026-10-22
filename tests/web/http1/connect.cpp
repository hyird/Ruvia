#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct observation {
    std::mutex mutex_;
    std::string bytes_;
    std::atomic<bool> finish_first_{};
    std::atomic<bool> ended_{};
    std::atomic<bool> retained_stable_{true};
};
const auto observed = std::make_shared<observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<observation>>([] { return observed; });
    return true;
}();

class http1_tunnel_routes final {
    RUVIA_ROUTES_BEGIN(http1_tunnel_routes)
    RUVIA_CONNECT("http1-target.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& observed_value = *context_value.worker_state<std::shared_ptr<observation>>();
        auto& tunnel_value = context_value.tunnel();
        if (observed_value.finish_first_) {
            co_await tunnel_value.finish();
        }
        std::optional<std::pmr::string> retained;
        while (auto bytes = co_await tunnel_value.read()) {
            {
                std::lock_guard lock(observed_value.mutex_);
                observed_value.bytes_.append(*bytes);
            }
            if (!retained) {
                retained.emplace(*bytes, context_value.pool());
            }
            if (!observed_value.finish_first_) {
                auto output = tunnel_value.write(std::string_view(*bytes));
                bytes->assign("input changed before awaiting output");
                co_await std::move(output);
            }
            observed_value.retained_stable_ = observed_value.retained_stable_ && retained->find_first_not_of('t') == std::string_view::npos;
        }
        observed_value.ended_ = true;
        co_await tunnel_value.finish();
    }
};

void exercise_connect(ruvia::testing::test_context& ruvia_ctx, bool finish_first) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    {
        std::lock_guard lock(observed->mutex_);
        observed->bytes_.clear();
    }
    observed->finish_first_ = finish_first;
    observed->ended_ = false;
    observed->retained_stable_ = true;
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    const std::string initial_value(5007, 't');
    const std::string additional(100003, 't');
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto send = [&](std::string_view bytes_value) -> ruvia::task<void> {
                const auto result_value = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(bytes_value), std::move(handler)); });
                if (result_value.error_code()) {
                    throw std::system_error(result_value.error_code());
                }
            };
            const std::string request = "CONNECT HTTP1-TARGET.TEST:0443 HTTP/1.1\r\nHost: HTTP1-TARGET.TEST:0443\r\n\r\n" + initial_value;
            co_await send(request);
            std::string head;
            char byte{};
            while (!head.ends_with("\r\n\r\n")) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(&byte, 1), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                head.push_back(byte);
            }
            RUVIA_CHECK(head.starts_with("HTTP/1.1 200 "));
            RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
            RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string::npos);
            std::array<char, 4096> input{};
            std::string echoed;
            if (!finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                echoed.append(input.data(), read.result());
            }
            RUVIA_CHECK(echoed == (finish_first ? std::string{} : initial_value + additional));
            if (finish_first) {
                co_await send(additional);
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client);
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    server.finish();
    ruvia::close_socket(client);
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(observed->ended_ && observed->retained_stable_);
    {
        std::lock_guard lock(observed->mutex_);
        RUVIA_CHECK(observed->bytes_ == initial_value + additional);
    }
}

class http1_interim_routes final {
    RUVIA_ROUTES_BEGIN(http1_interim_routes)
    RUVIA_GET("/http1-early-hints", hints);
    RUVIA_GET("/http1-cold-hints", cold_hints);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hints(ruvia::context& context_value) {
        std::string link_value = "</asset>; rel=preload";
        const std::array<ruvia::http_header_view, 1> links{{{"link", link_value}}};
        auto first = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, links));
        auto second = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        link_value.assign("changed before starting the operation");
        if (context_value.req().protocol_version() == ruvia::http_protocol_version::http11) {
            ruvia::task_scope output(context_value.worker());
            const auto send_first = [&]() -> ruvia::task<void> { co_await std::move(first); };
            output.spawn(send_first());
            unsigned rejected = 0;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                try {
                    co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
                } catch (const std::logic_error&) {
                    ++rejected;
                }
            }
            co_await output.join();
            if (rejected != 2) {
                throw std::logic_error("interim response output accepted concurrent operations");
            }
        } else {
            co_await std::move(first);
        }
        co_await std::move(second);
        co_return context_value.text("final");
    }

    ruvia::task<void> cold_hints(ruvia::context& context_value) {
        auto cold = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        auto another = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::processing));
        co_await context_value.stream().write("cold-final");
        bool rejected_after_final = false;
        try {
            co_await std::move(cold);
        } catch (const std::logic_error&) {
            rejected_after_final = true;
        }
        if (!rejected_after_final) {
            throw std::logic_error("interim response started after final response commit");
        }
    }
};

// Sends one closing request and returns every byte the server wrote until EOF.
std::string exchange_closing_request(std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::string received;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code timeout) { if (!timeout) { ruvia::close_socket(client); } });
        try {
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(client, asio::buffer(request), std::move(handler)); });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            std::array<char, 4096> input{};
            for (;;) {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { client.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code() == asio::error::eof) {
                    break;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received.append(input.data(), read.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    ruvia::close_socket(client);
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return received;
}
}  // namespace

RUVIA_TEST(http1_connect_routes_transfer_buffered_bytes_and_keep_receive_direction_after_send_fin) {
    exercise_connect(ruvia_ctx, false);
    exercise_connect(ruvia_ctx, true);
}

RUVIA_TEST(http1_interim_responses_reach_http11_clients_but_never_http10_clients) {
    const auto http11 = exchange_closing_request("GET /http1-early-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(http11.starts_with("HTTP/1.1 103 "));
    RUVIA_CHECK(http11.find("link: </asset>; rel=preload\r\n") != std::string::npos);
    RUVIA_CHECK(http11.find("\r\n\r\nHTTP/1.1 102 ") != std::string::npos);
    RUVIA_CHECK(http11.find("\r\n\r\nHTTP/1.1 200 ") != std::string::npos);
    RUVIA_CHECK(http11.ends_with("final"));

    // RFC 9110 15.2: an HTTP/1.0 client would take the 1xx head as the final
    // response, so the server omits it and sends only the final response.
    const auto http10 = exchange_closing_request("GET /http1-early-hints HTTP/1.0\r\nHost: localhost\r\n\r\n");
    RUVIA_CHECK(http10.find(" 103 ") == std::string::npos);
    RUVIA_CHECK(http10.find(" 102 ") == std::string::npos);
    RUVIA_CHECK(http10.substr(0, http10.find("\r\n")).find(" 200 ") != std::string::npos);
    RUVIA_CHECK(http10.ends_with("final"));
}

RUVIA_TEST(http1_unstarted_interim_responses_do_not_block_the_final_response) {
    const auto response = exchange_closing_request("GET /http1-cold-hints HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(response.find(" 103 ") == std::string::npos);
    RUVIA_CHECK(response.find(" 102 ") == std::string::npos);
    RUVIA_CHECK(response.find("cold-final") != std::string::npos);
}
