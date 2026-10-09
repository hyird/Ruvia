#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace {

using namespace std::chrono_literals;

enum class startup_outcome { fail,
    stop_in_hook,
    stop_from_caller,
    serve };

struct startup_state final {
    startup_outcome outcome_{};
    std::atomic<unsigned> live_workers_{};
    std::atomic<unsigned> destroyed_workers_{};
    std::atomic<unsigned> requests_{};
    std::atomic<bool> initialized_{};
    std::atomic<bool> startup_job_cancelled_{};
    std::binary_semaphore hook_entered_{0};
    std::binary_semaphore release_hook_{0};
    unsigned second_hooks_{};
    unsigned stop_hooks_{};
    std::thread::id start_thread_{};
    std::thread::id stop_thread_{};
};

struct worker_state final {
    explicit worker_state(std::shared_ptr<startup_state> owner_value)
        : state_(std::move(owner_value)) {
        state_->live_workers_.fetch_add(1);
    }
    ~worker_state() {
        state_->live_workers_.fetch_sub(1);
        state_->destroyed_workers_.fetch_add(1);
    }
    std::shared_ptr<startup_state> state_;
};

class startup_controller final : public ruvia::controller<startup_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/ready", ready);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ready(ruvia::context& context) {
        auto& state_value = *context.worker_state<worker_state>().state_;
        state_value.requests_.fetch_add(1);
        co_return context.text(std::string_view(state_value.initialized_.load() ? "initialized" : "premature"));
    }
};

class app_run final {
public:
    explicit app_run(ruvia::application& app, startup_state& state_value)
        : app_(app),
          state_(state_value),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }) {}

    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            state_.release_hook_.release();
            thread_.join();
        }
    }
    void join() {
        thread_.join();
    }
    [[nodiscard]] std::thread::id id() const noexcept {
        return thread_.get_id();
    }
    std::exception_ptr failure_;

private:
    ruvia::application& app_;
    startup_state& state_;
    std::thread thread_;
};

std::string read_response(asio::ip::tcp::socket& socket) {
    std::string response;
    std::array<char, 1024> buffer;
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline_value) {
        asio::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (response.find("initialized") != std::string::npos ||
            (error && error != asio::error::would_block && error != asio::error::try_again)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return response;
}

}  // namespace

// application is a process singleton with sealed controller registration. This functional
// test runs in its own executable and exercises retries against the same config.
RUVIA_TEST(app_startup_barrier_orders_hooks_and_rolls_back) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write startup test private key");
        }
    }
    auto state_value = std::make_shared<startup_state>();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2, .worker_queue_capacity_ = 16, .max_connections_per_worker_ = 4})
        .get_blocking_pool(nullptr)
        .use_worker_state<worker_state>([state_value] { return state_value; });
    app.on_start([state_value, &app] {
        state_value->start_thread_ = std::this_thread::get_id();
        auto workers = app.workers();
        if (workers.size() != 2 || state_value->live_workers_.load() != 2) {
            throw std::runtime_error("start hook ran before worker initialization");
        }
        for (auto& worker : workers) {
            struct job_result final {
                std::binary_semaphore completed_{0};
                bool valid_{};
            };
            auto result_value = std::make_shared<job_result>();
            const auto posted = worker.post([state_value, result_value](ruvia::web_worker_context& context_value) -> ruvia::task<void> {
                result_value->valid_ = context_value.worker().is_current() &&
                                       context_value.worker_state<worker_state>().state_ == state_value;
                result_value->completed_.release();
                co_return;
            });
            if (!posted.accepted() || !result_value->completed_.try_acquire_for(3s) || !result_value->valid_) {
                throw std::runtime_error("ready worker could not execute a startup job");
            }
        }
        if (state_value->outcome_ == startup_outcome::stop_from_caller) {
            std::promise<void> completion;
            auto done = completion.get_future();
            const auto posted = workers.front().post([state_value, completion = std::move(completion)](ruvia::web_worker_context& context_value) mutable -> ruvia::task<void> {
                state_value->hook_entered_.release();
                const auto result_value = co_await ruvia::sleep_for(context_value.worker(), 1h, context_value.get_stop_token());
                state_value->startup_job_cancelled_.store(result_value == ruvia::timer_sleep_result::stop_requested);
                completion.set_value();
            });
            if (!posted.accepted()) {
                throw std::runtime_error("ready worker rejected startup initialization");
            }
            // The lifecycle caller can wait for cancellable worker initialization.
            // application::stop() must wake it without requiring this caller to join first.
            done.get();
            return;
        }
        state_value->hook_entered_.release();
        state_value->release_hook_.acquire();
        if (state_value->outcome_ == startup_outcome::fail) {
            throw std::runtime_error("startup hook failed");
        }
        if (state_value->outcome_ == startup_outcome::stop_in_hook) {
            app.stop();
        }
    });
    app.on_start([state_value] {
        ++state_value->second_hooks_;
        state_value->initialized_.store(true);
    });
    app.on_stop([state_value] {
        ++state_value->stop_hooks_;
        state_value->stop_thread_ = std::this_thread::get_id();
    });

    for (const auto outcome : {startup_outcome::fail, startup_outcome::stop_in_hook,
             startup_outcome::stop_from_caller, startup_outcome::serve}) {
        state_value->outcome_ = outcome;
        state_value->initialized_.store(false);
        state_value->startup_job_cancelled_.store(false);
        state_value->requests_.store(0);
        const auto second_hooks = state_value->second_hooks_;
        const auto stop_hooks = state_value->stop_hooks_;
        const auto destroyed_workers = state_value->destroyed_workers_.load();
        asio::io_context io;
        asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
        asio::ip::tcp::acceptor tls_reservation(io, {asio::ip::address_v4::loopback(), 0});
        const auto endpoint = reservation.local_endpoint();
        const auto tls_endpoint = tls_reservation.local_endpoint();
        reservation.close();
        tls_reservation.close();
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}, .http3_ = {.mode_ = ruvia::http3_mode::enabled, .stream_buffer_capacity_ = 3, .datagram_input_capacity_ = 5, .datagram_output_capacity_ = 2}});
        app_run run(app, *state_value);
        const auto owner_thread = run.id();
        const bool entered = state_value->hook_entered_.try_acquire_for(3s);
        RUVIA_CHECK(entered);
        if (!entered) {
            return;
        }
        RUVIA_CHECK(state_value->start_thread_ == owner_thread);

        // A completed TCP handshake can queue in the kernel backlog, but neither
        // an accept nor a request dispatch is allowed before hooks finish.
        asio::ip::tcp::socket socket(io);
        socket.connect(endpoint);
        constexpr std::string_view request = "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        asio::write(socket, asio::buffer(request));
        socket.non_blocking(true);
        asio::ip::udp::socket udp(io, {asio::ip::address_v4::loopback(), 0});
        udp.non_blocking(true);
        std::array<std::byte, 1200> unsupported_version{};
        unsupported_version[0] = std::byte{0xc0};
        unsupported_version[4] = std::byte{2};
        unsupported_version[5] = std::byte{8};
        unsupported_version[14] = std::byte{8};
        for (std::size_t i = 0; i < 8; ++i) {
            unsupported_version[6 + i] = static_cast<std::byte>(0x10 + i);
            unsupported_version[15 + i] = static_cast<std::byte>(0x20 + i);
        }
        const asio::ip::udp::endpoint quic_endpoint(tls_endpoint.address(), tls_endpoint.port());
        udp.send_to(asio::buffer(unsupported_version), quic_endpoint);
        std::this_thread::sleep_for(25ms);
        std::array<char, 256> premature;
        asio::error_code error;
        RUVIA_CHECK_EQ(socket.read_some(asio::buffer(premature), error), std::size_t{0});
        RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
        asio::ip::udp::endpoint source;
        std::array<std::byte, 1200> quic_response;
        RUVIA_CHECK_EQ(udp.receive_from(asio::buffer(quic_response), source, 0, error), std::size_t{0});
        RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
        RUVIA_CHECK_EQ(state_value->requests_.load(), 0U);
        RUVIA_CHECK_EQ(app.http_stats().active_connections_, std::size_t{0});
        auto escaped_worker = app.workers().front();
        if (outcome == startup_outcome::stop_from_caller) {
            app.stop();
        } else {
            state_value->release_hook_.release();
        }
        if (outcome == startup_outcome::serve) {
            RUVIA_CHECK(read_response(socket).find("initialized") != std::string::npos);
            const auto deadline_value = std::chrono::steady_clock::now() + 3s;
            std::size_t size{};
            do {
                size = udp.receive_from(asio::buffer(quic_response), source, 0, error);
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline_value);
            RUVIA_CHECK(!error);
            RUVIA_CHECK(size > 5);
            RUVIA_CHECK(source == quic_endpoint);
            if (size > 5) {
                RUVIA_CHECK(quic_response[1] == std::byte{} && quic_response[2] == std::byte{} &&
                            quic_response[3] == std::byte{} && quic_response[4] == std::byte{});
            }
            app.stop();
        }
        run.join();
        RUVIA_CHECK(state_value->stop_thread_ == owner_thread);
        RUVIA_CHECK_EQ(state_value->stop_hooks_, stop_hooks + 1);
        RUVIA_CHECK_EQ(state_value->live_workers_.load(), 0U);
        RUVIA_CHECK_EQ(state_value->destroyed_workers_.load(), destroyed_workers + 2);
        RUVIA_CHECK(!escaped_worker.valid());
        RUVIA_CHECK(!escaped_worker.accepting());
        RUVIA_CHECK_EQ(state_value->second_hooks_, second_hooks + (outcome == startup_outcome::serve ? 1 : 0));
        RUVIA_CHECK_EQ(state_value->requests_.load(), outcome == startup_outcome::serve ? 1U : 0U);
        RUVIA_CHECK_EQ(state_value->startup_job_cancelled_.load(), outcome == startup_outcome::stop_from_caller);
        if (outcome == startup_outcome::fail) {
            RUVIA_CHECK(run.failure_ != nullptr);
            if (run.failure_) {
                try {
                    std::rethrow_exception(run.failure_);
                } catch (const std::runtime_error& failure) {
                    RUVIA_CHECK_EQ(std::string(failure.what()), std::string("startup hook failed"));
                }
            }
        } else {
            RUVIA_CHECK(run.failure_ == nullptr);
        }
    }
}
