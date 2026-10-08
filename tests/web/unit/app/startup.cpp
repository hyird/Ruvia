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

#include "ruvia/core/Timer.h"
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

#include "test_harness.h"
#include "test_tls_identity.h"

namespace {

using namespace std::chrono_literals;

enum class startup_outcome { fail,
    stop_in_hook,
    stop_from_caller,
    serve };

struct startup_state final {
    startup_outcome outcome{};
    std::atomic<unsigned> live_workers{};
    std::atomic<unsigned> destroyed_workers{};
    std::atomic<unsigned> requests{};
    std::atomic<bool> initialized{};
    std::atomic<bool> startup_job_cancelled{};
    std::binary_semaphore hook_entered{0};
    std::binary_semaphore release_hook{0};
    unsigned second_hooks{};
    unsigned stop_hooks{};
    std::thread::id start_thread{};
    std::thread::id stop_thread{};
};

struct worker_state final {
    explicit worker_state(std::shared_ptr<startup_state> owner)
        : state(std::move(owner)) {
        state->live_workers.fetch_add(1);
    }
    ~worker_state() {
        state->live_workers.fetch_sub(1);
        state->destroyed_workers.fetch_add(1);
    }
    std::shared_ptr<startup_state> state;
};

class startup_controller final : public ruvia::Controller<startup_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/ready", ready);
    RUVIA_ROUTES_END

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& context) {
        auto& state = *context.workerState<worker_state>().state;
        state.requests.fetch_add(1);
        co_return context.text(std::string_view(state.initialized.load() ? "initialized" : "premature"));
    }
};

class app_run final {
public:
    explicit app_run(ruvia::App& app, startup_state& state)
        : app_(app),
          state_(state),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure = std::current_exception();
              }
          }) {}

    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            state_.release_hook.release();
            thread_.join();
        }
    }
    void join() {
        thread_.join();
    }
    [[nodiscard]] std::thread::id id() const noexcept {
        return thread_.get_id();
    }
    std::exception_ptr failure;

private:
    ruvia::App& app_;
    startup_state& state_;
    std::thread thread_;
};

std::string read_response(asio::ip::tcp::socket& socket) {
    std::string response;
    std::array<char, 1024> buffer;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
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

// App is a process singleton with sealed controller registration. This functional
// test runs in its own executable and exercises retries against the same config.
RUVIA_TEST(app_startup_barrier_orders_hooks_and_rolls_back) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || PEM_write_bio_PrivateKey(output.get(), SSL_CTX_get0_privatekey(identity.context.native_handle()), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            throw std::runtime_error("cannot write startup test private key");
        }
    }
    auto state = std::make_shared<startup_state>();
    auto& app = ruvia::app();
    app.server({.worker_count = 2, .worker_queue_capacity = 16, .max_connections_per_worker = 4})
        .blockingPool(nullptr)
        .useWorkerState<worker_state>([state] { return state; });
    app.onStart([state, &app] {
        state->start_thread = std::this_thread::get_id();
        auto workers = app.workers();
        if (workers.size() != 2 || state->live_workers.load() != 2) {
            throw std::runtime_error("start hook ran before worker initialization");
        }
        for (auto& worker : workers) {
            struct job_result final {
                std::binary_semaphore completed{0};
                bool valid{};
            };
            auto result = std::make_shared<job_result>();
            const auto posted = worker.post([state, result](ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
                result->valid = context.worker().isCurrent() &&
                                context.workerState<worker_state>().state == state;
                result->completed.release();
                co_return;
            });
            if (!posted.accepted() || !result->completed.try_acquire_for(3s) || !result->valid) {
                throw std::runtime_error("ready worker could not execute a startup job");
            }
        }
        if (state->outcome == startup_outcome::stop_from_caller) {
            std::promise<void> completion;
            auto done = completion.get_future();
            const auto posted = workers.front().post([state, completion = std::move(completion)](ruvia::WebWorkerContext& context) mutable -> ruvia::Task<void> {
                state->hook_entered.release();
                const auto result = co_await ruvia::sleepFor(context.worker(), 1h, context.stopToken());
                state->startup_job_cancelled.store(result == ruvia::TimerSleepResult::kStopRequested);
                completion.set_value();
            });
            if (!posted.accepted()) {
                throw std::runtime_error("ready worker rejected startup initialization");
            }
            // The lifecycle caller can wait for cancellable worker initialization.
            // App::stop() must wake it without requiring this caller to join first.
            done.get();
            return;
        }
        state->hook_entered.release();
        state->release_hook.acquire();
        if (state->outcome == startup_outcome::fail) {
            throw std::runtime_error("startup hook failed");
        }
        if (state->outcome == startup_outcome::stop_in_hook) {
            app.stop();
        }
    });
    app.onStart([state] {
        ++state->second_hooks;
        state->initialized.store(true);
    });
    app.onStop([state] {
        ++state->stop_hooks;
        state->stop_thread = std::this_thread::get_id();
    });

    for (const auto outcome : {startup_outcome::fail, startup_outcome::stop_in_hook,
             startup_outcome::stop_from_caller, startup_outcome::serve}) {
        state->outcome = outcome;
        state->initialized.store(false);
        state->startup_job_cancelled.store(false);
        state->requests.store(0);
        const auto second_hooks = state->second_hooks;
        const auto stop_hooks = state->stop_hooks;
        const auto destroyed_workers = state->destroyed_workers.load();
        asio::io_context io;
        asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
        asio::ip::tcp::acceptor tls_reservation(io, {asio::ip::address_v4::loopback(), 0});
        const auto endpoint = reservation.local_endpoint();
        const auto tls_endpoint = tls_reservation.local_endpoint();
        reservation.close();
        tls_reservation.close();
        app.listen({.address = "127.0.0.1", .http = endpoint.port(), .https = tls_endpoint.port(), .tls = {.certificateChainFile = identity.ca_file, .privateKeyFile = private_key}, .http3 = {.mode = ruvia::Http3Mode::kEnabled, .stream_buffer_capacity = 3, .datagram_input_capacity = 5, .datagram_output_capacity = 2}});
        app_run run(app, *state);
        const auto owner_thread = run.id();
        const bool entered = state->hook_entered.try_acquire_for(3s);
        RUVIA_CHECK(entered);
        if (!entered) {
            return;
        }
        RUVIA_CHECK(state->start_thread == owner_thread);

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
        RUVIA_CHECK_EQ(state->requests.load(), 0U);
        RUVIA_CHECK_EQ(app.httpStats().activeConnections, std::size_t{0});
        auto escaped_worker = app.workers().front();
        if (outcome == startup_outcome::stop_from_caller) {
            app.stop();
        } else {
            state->release_hook.release();
        }
        if (outcome == startup_outcome::serve) {
            RUVIA_CHECK(read_response(socket).find("initialized") != std::string::npos);
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            std::size_t size{};
            do {
                size = udp.receive_from(asio::buffer(quic_response), source, 0, error);
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
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
        RUVIA_CHECK(state->stop_thread == owner_thread);
        RUVIA_CHECK_EQ(state->stop_hooks, stop_hooks + 1);
        RUVIA_CHECK_EQ(state->live_workers.load(), 0U);
        RUVIA_CHECK_EQ(state->destroyed_workers.load(), destroyed_workers + 2);
        RUVIA_CHECK(!escaped_worker.valid());
        RUVIA_CHECK(!escaped_worker.accepting());
        RUVIA_CHECK_EQ(state->second_hooks, second_hooks + (outcome == startup_outcome::serve ? 1 : 0));
        RUVIA_CHECK_EQ(state->requests.load(), outcome == startup_outcome::serve ? 1U : 0U);
        RUVIA_CHECK_EQ(state->startup_job_cancelled.load(), outcome == startup_outcome::stop_from_caller);
        if (outcome == startup_outcome::fail) {
            RUVIA_CHECK(run.failure != nullptr);
            if (run.failure) {
                try {
                    std::rethrow_exception(run.failure);
                } catch (const std::runtime_error& failure) {
                    RUVIA_CHECK_EQ(std::string(failure.what()), std::string("startup hook failed"));
                }
            }
        } else {
            RUVIA_CHECK(run.failure == nullptr);
        }
    }
}
