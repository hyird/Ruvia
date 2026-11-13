#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/server_config.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"

namespace ruvia::test {

class http_server_fixture final {
public:
    // The application is shared by every case in the executable, so the first
    // fixture constructed fixes its server_config.
    explicit http_server_fixture(asio::io_context& io,
        server_config config = {.worker_count_ = 1, .connection_scan_interval_ = std::chrono::milliseconds(5)})
        : runtime_(&shared_runtime(io, config)) {}

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
        server_runtime(asio::io_context& io, const server_config& config)
            : reservation_(io),
              endpoint_(reservation_.endpoint()),
              startup_(std::make_shared<startup_state>()) {
            app().server(config).listen({.address_ = "127.0.0.1", .http_ = endpoint_.port()}).on_start([startup = std::weak_ptr<startup_state>(startup_)] {
                if (auto state = startup.lock()) {
                    state->ready_.set_value();
                }
            });
            auto ready = startup_->ready_.get_future();
            runner_ = std::make_unique<app_run>(app());
            try {
                wait_until([&] {
                    return ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready ||
                           runner_->failure() != nullptr;
                },
                    std::chrono::seconds(5));
                rethrow_failure();
                ready.get();
            } catch (...) {
                runner_.reset();
                throw;
            }
        }

        [[nodiscard]] const asio::ip::tcp::endpoint& endpoint() const noexcept {
            return endpoint_;
        }

        void finish_case() {
            wait_until([&] {
                rethrow_failure();
                return app().stats().active_connections_ == 0;
            },
                std::chrono::seconds(5));
        }

    private:
        struct startup_state final {
            std::promise<void> ready_;
        };

        void rethrow_failure() {
            if (const auto failure = runner_->failure()) {
                std::rethrow_exception(failure);
            }
        }

        tcp_port_reservation reservation_;
        asio::ip::tcp::endpoint endpoint_;
        std::shared_ptr<startup_state> startup_;
        std::unique_ptr<app_run> runner_;
    };

    static server_runtime& shared_runtime(asio::io_context& io, const server_config& config) {
        static server_runtime runtime(io, config);
        return runtime;
    }

    server_runtime* runtime_;
};

}  // namespace ruvia::test
