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
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/websocket.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace {

using namespace std::chrono_literals;

enum class startup_outcome { fail,
    stop_during_worker_startup,
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
    std::atomic<bool> worker_startup_blocked_{};
    std::binary_semaphore hook_entered_{0};
    std::binary_semaphore release_hook_{0};
    unsigned second_hooks_{};
    unsigned stop_hooks_{};
    std::thread::id start_thread_{};
    std::thread::id stop_thread_{};
    std::atomic<bool> websocket_message_{};
    std::atomic<bool> websocket_end_{};
    std::atomic<bool> websocket_failed_{};
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
    RUVIA_POST("/limited", limited, ruvia::body_limit<16>);
    RUVIA_GET_WS("/socket", read_websocket);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ready(ruvia::context& context) {
        auto& state_value = *context.worker_state<worker_state>().state_;
        state_value.requests_.fetch_add(1);
        co_return context.text(std::string_view(state_value.initialized_.load() ? "initialized" : "premature"));
    }

    ruvia::task<ruvia::http_response> limited(ruvia::context& context) {
        co_return context.text(std::string_view("accepted"));
    }

    ruvia::task<void> read_websocket(ruvia::context& context) {
        auto& state_value = *context.worker_state<worker_state>().state_;
        auto& websocket_value = context.get_websocket();
        try {
            const auto first = co_await websocket_value.read();
            state_value.websocket_message_ = first.has_value() && first->payload() == std::string_view("hi");
            const auto end = co_await websocket_value.read();
            state_value.websocket_end_ = !end.has_value();
        } catch (...) {
            state_value.websocket_failed_ = true;
        }
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

// The listener's ticket keys are shared by every worker, so each resumption
// attempt succeeds regardless of which worker the acceptor selects. Offering
// the session must never fail a handshake.
bool resume_tls_session(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session(nullptr, SSL_SESSION_free);
    for (int attempt = 0; attempt < 6; ++attempt) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        asio::error_code error;
        stream.next_layer().connect(endpoint, error);
        if (!error && session && SSL_set_session(stream.native_handle(), session.get()) != 1) {
            return false;
        }
        if (!error) {
            stream.handshake(asio::ssl::stream_base::client, error);
        }
        if (error) {
            return false;
        }
        if (!session) {
            // TLS 1.3 tickets precede the response, so the first response
            // bytes mean they were processed. Capture the session before the
            // server can close the connection.
            constexpr std::string_view request = "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n";
            asio::write(stream, asio::buffer(request), error);
            std::array<char, 1024> buffer;
            if (error || stream.read_some(asio::buffer(buffer), error) == 0 || error) {
                return false;
            }
            session.reset(SSL_get1_session(stream.native_handle()));
            if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
                return false;
            }
        } else if (SSL_session_reused(stream.native_handle()) != 1) {
            return false;
        }
        // OpenSSL marks the session of an SSL freed without a local shutdown
        // as unresumable; record a clean local close so later attempts can offer it.
        SSL_set_shutdown(stream.native_handle(), SSL_SENT_SHUTDOWN);
    }
    return true;
}

// Reads until the peer ends the stream and returns the terminal error.
template <typename stream_type>
asio::error_code read_to_end(stream_type& stream, std::string& received) {
    std::array<char, 4096> buffer;
    asio::error_code error;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), size);
    }
    return error;
}

// RFC 8446 §6.1: closing after a Connection: close response sends close_notify,
// so the client observes a clean TLS end instead of a truncated stream.
bool tls_close_sends_close_notify(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view request = "GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    std::string response;
    const auto end = read_to_end(stream, response);
    return response.starts_with("HTTP/1.1 404 ") && end == asio::error::eof &&
           (SSL_get_shutdown(stream.native_handle()) & SSL_RECEIVED_SHUTDOWN) != 0;
}

// RFC 9112 §9.6: a response sent before the request content was read (413)
// is followed by a staged close, so the client can finish sending and still
// read the complete response instead of a connection reset.
bool rejected_upload_reads_complete_response(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    const std::string body(std::size_t{1024} * 1024, 'x');
    const auto request = "POST /limited HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    std::string response;
    const auto end = read_to_end(socket, response);
    return response.starts_with("HTTP/1.1 413 ") && response.find("\r\n\r\n") != std::string::npos &&
           end == asio::error::eof;
}

// A TLS peer that drops TCP without close_notify ends the server websocket
// read side exactly like an orderly close instead of failing the read.
bool tls_websocket_truncation_ends_read(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const startup_state& state_value) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view upgrade =
        "GET /socket HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(upgrade), error);
    }
    std::string head;
    char byte{};
    while (!error && !head.ends_with("\r\n\r\n")) {
        if (stream.read_some(asio::buffer(&byte, 1), error) == 1) {
            head.push_back(byte);
        }
    }
    if (error || !head.starts_with("HTTP/1.1 101 ")) {
        return false;
    }
    // One masked text frame (zero masking key), then a TCP FIN without close_notify.
    constexpr std::array<char, 8> frame{'\x81', '\x82', '\0', '\0', '\0', '\0', 'h', 'i'};
    asio::write(stream, asio::buffer(frame), error);
    if (error) {
        return false;
    }
    stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, error);
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    while (!state_value.websocket_end_.load() && !state_value.websocket_failed_.load() &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(1ms);
    }
    return state_value.websocket_message_.load() && state_value.websocket_end_.load() &&
           !state_value.websocket_failed_.load();
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
        .use_worker_state<worker_state>([state_value] {
            if (state_value->outcome_ == startup_outcome::stop_during_worker_startup &&
                !state_value->worker_startup_blocked_.exchange(true)) {
                // Hold one worker inside startup while the lifecycle caller waits for it.
                state_value->hook_entered_.release();
                state_value->release_hook_.acquire();
            }
            return state_value;
        });
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

    for (const auto outcome : {startup_outcome::fail, startup_outcome::stop_during_worker_startup,
             startup_outcome::stop_in_hook, startup_outcome::stop_from_caller, startup_outcome::serve}) {
        state_value->outcome_ = outcome;
        state_value->initialized_.store(false);
        state_value->startup_job_cancelled_.store(false);
        state_value->worker_startup_blocked_.store(false);
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
        app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key, .client_certificates_ = {.verify_file_ = identity.ca_file_}}, .http3_ = {.mode_ = ruvia::http3_mode::enabled, .stream_buffer_capacity_ = 3, .datagram_input_capacity_ = 5, .datagram_output_capacity_ = 2}});
        app_run run(app, *state_value);
        const auto owner_thread = run.id();
        const bool entered = state_value->hook_entered_.try_acquire_for(3s);
        RUVIA_CHECK(entered);
        if (!entered) {
            return;
        }
        if (outcome == startup_outcome::stop_during_worker_startup) {
            // A stop request that cancels worker startup is not a startup
            // failure: run() returns normally and leaves no partial service.
            std::this_thread::sleep_for(25ms);
            app.stop();
            state_value->release_hook_.release();
            run.join();
            RUVIA_CHECK(run.failure_ == nullptr);
            RUVIA_CHECK_EQ(state_value->stop_hooks_, stop_hooks + 1);
            RUVIA_CHECK_EQ(state_value->live_workers_.load(), 0U);
            RUVIA_CHECK_EQ(state_value->destroyed_workers_.load(), destroyed_workers + 2);
            RUVIA_CHECK_EQ(state_value->second_hooks_, second_hooks);
            RUVIA_CHECK_EQ(state_value->requests_.load(), 0U);
            continue;
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
            // Optional client-certificate verification must not turn session
            // resumption into a fatal handshake error, and every resumption
            // succeeds whichever of the two workers accepts it.
            RUVIA_CHECK(resume_tls_session(io, tls_endpoint));
            RUVIA_CHECK(tls_close_sends_close_notify(io, tls_endpoint));
            RUVIA_CHECK(rejected_upload_reads_complete_response(io, endpoint));
            RUVIA_CHECK(tls_websocket_truncation_ends_read(io, tls_endpoint, *state_value));
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
