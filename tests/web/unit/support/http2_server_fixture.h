#pragma once

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/app.h"

namespace ruvia::test {

class http2_server_fixture final {
public:
    explicit http2_server_fixture(asio::io_context& io)
        : runtime_(&shared_runtime(io)) {}

    [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
        return runtime_->endpoint();
    }

    // Call after closing or shutting down every client belonging to this case.
    // The application stays running for subsequent cases in the same executable.
    void finish() {
        runtime_->finish_case();
    }

private:
    class server_runtime final {
    public:
        explicit server_runtime(asio::io_context& io)
            : startup_(std::make_shared<startup_state>()) {
            asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
            endpoint_ = reservation.local_endpoint();
            reservation.close();
            app().server({.worker_count_ = 1, .connection_scan_interval_ = std::chrono::milliseconds(5)}).listen({.address_ = "127.0.0.1", .http_ = endpoint_.port()}).on_start([startup = std::weak_ptr<startup_state>(startup_)] {
                if (auto state = startup.lock()) {
                    state->announced_ = true;
                    state->ready_.set_value();
                }
            });
            auto ready = startup_->ready_.get_future();
            thread_ = std::thread([this] {
                try {
                    app().run();
                } catch (...) {
                    const auto failure = std::current_exception();
                    {
                        std::lock_guard lock(mutex_);
                        failure_ = failure;
                    }
                    if (!startup_->announced_) {
                        startup_->ready_.set_exception(failure);
                    }
                }
            });
            try {
                ready.get();
            } catch (...) {
                stop_and_join();
                throw;
            }
        }

        ~server_runtime() {
            stop_and_join();
        }

        [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
            return endpoint_;
        }

        void finish_case() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (app().http_stats().active_connections_ != 0) {
                rethrow_failure();
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("HTTP test clients did not retire");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            rethrow_failure();
        }

    private:
        struct startup_state final {
            std::promise<void> ready_;
            bool announced_{false};
        };

        void rethrow_failure() {
            std::lock_guard lock(mutex_);
            if (failure_) {
                std::rethrow_exception(failure_);
            }
        }

        void stop_and_join() {
            if (thread_.joinable()) {
                app().stop();
                thread_.join();
            }
        }

        std::shared_ptr<startup_state> startup_;
        asio::ip::tcp::endpoint endpoint_;
        std::thread thread_;
        std::mutex mutex_;
        std::exception_ptr failure_;
    };

    static server_runtime& shared_runtime(asio::io_context& io) {
        static server_runtime runtime(io);
        return runtime;
    }

    server_runtime* runtime_;
};

}  // namespace ruvia::test
