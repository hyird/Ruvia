#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "blocking_pool.h"
#include "event_loop.h"
#include "http_client.h"
#include "task.h"
#include "task_scope.h"

// Shared by the HTTP client suites; each translation unit keeps its own copy.
namespace {

class test_worker final {
public:
    explicit test_worker(asio::io_context& io) :
        attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8, .blocking_pool_ = &host_lookups_})),
        handle_(attachment_.loop().worker()) {}

    test_worker(const test_worker&) = delete;
    test_worker& operator=(const test_worker&) = delete;

    ~test_worker() {
        host_lookups_.join();
    }

    // Declared before the attachment, which borrows it for "localhost" lookups.
    ruvia::blocking_pool host_lookups_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_ref handle_;
};

[[nodiscard]] inline ruvia::http_client_config local_http_client_config(std::string_view base_url) {
    return ruvia::http_client_config{.base_url_ = base_url};
}

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await operation();
        } catch (...) {
            failure = std::current_exception();
        }
        worker_value.attachment_.stop();
    };
    auto root = worker_value.attachment_.loop().start(run());
    worker_value.attachment_.run();
    std::move(root).get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

// A failed client operation must not leave the peer's accept pending, which
// would keep the I/O context running.
struct acceptor_closer final {
    asio::ip::tcp::acceptor& acceptor_;
    ~acceptor_closer() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }
};

[[nodiscard]] inline std::string_view response_text(const ruvia::http_client_response_bytes& bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()};
}

namespace http_client_pool_test {

// Completion, progress and gating of one scripted origin coroutine, observed
// from the client's worker. Its guard closes the acceptor and every connection
// the script registered, so a client that stops early cannot leave it pending.
class scripted_origin final {
public:
    scripted_origin(asio::io_context& io, const ruvia::worker_ref& worker_value) :
        io_(io),
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        guard_(io),
        gate_(io),
        progress_(worker_value) {
        gate_.expires_at(asio::steady_timer::time_point::max());
    }
    scripted_origin(asio::io_context&, ruvia::worker_ref&&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    [[nodiscard]] asio::ip::tcp::acceptor& acceptor() noexcept {
        return acceptor_;
    }

    void start(asio::awaitable<void> script) {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error || finished_) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto* socket : sockets_) {
                socket->close(ignored);
            }
        });
        asio::co_spawn(io_, std::move(script), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            progress_.notify();
        });
    }

    // Script side.
    void watch(asio::ip::tcp::socket::lowest_layer_type& socket) {
        sockets_.push_back(&socket);
    }
    void advance() noexcept {
        ++step_;
        progress_.notify();
    }
    [[nodiscard]] asio::awaitable<void> wait_for_release() {
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        gate_.expires_at(asio::steady_timer::time_point::max());
    }

    // Client side.
    void release() {
        gate_.expires_at(asio::steady_timer::time_point::min());
    }
    [[nodiscard]] ruvia::task<void> reach(std::size_t step) {
        while (step_ < step && !finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (step_ < step) {
            throw std::runtime_error("scripted origin ended early");
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    void stop_accepting() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }

private:
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    asio::steady_timer gate_;
    ruvia::worker_signal progress_;
    std::vector<asio::ip::tcp::socket::lowest_layer_type*> sockets_{};
    std::exception_ptr failure_;
    std::size_t step_{0};
    bool finished_{false};
};

// Runs `operation`, then always shuts the client down and joins the requests
// it spawned into `requests` and `origin`, so a failed check cannot leave the
// origin, the client or a request pending.
template <typename operation_type>
[[nodiscard]] ruvia::task<void> run_against(ruvia::http_client& client, scripted_origin& origin, ruvia::task_scope& requests, operation_type&& operation) {
    std::exception_ptr failure;
    try {
        co_await operation();
    } catch (...) {
        failure = std::current_exception();
    }
    origin.stop_accepting();
    co_await client.shutdown();
    try {
        co_await requests.join();
        co_await origin.join();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http_client_pool_test

}  // namespace
