#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_datagram_stream.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/http_udp_tunnel.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"
#include "ruvia/web/websocket_client.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace http3_client_connection_test {

using namespace std::chrono_literals;

class test_identity_files final {
    struct storage final {
        storage()
            : identity_("localhost"),
              private_key_(identity_.ca_file_.parent_path() / "key.pem") {
            std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
            if (!output || ruvia::test::write_tls_private_key(output.get(), SSL_CTX_get0_privatekey(identity_.context_.native_handle())) != 1) {
                throw std::runtime_error("cannot write HTTP/3 test private key");
            }
        }
        ruvia::test::tls_identity identity_;
        std::filesystem::path private_key_;
    };

    [[nodiscard]] static storage& shared_identity() {
        static storage identity;
        return identity;
    }

public:
    test_identity_files() {
        (void)shared_identity();
    }
    [[nodiscard]] const std::filesystem::path& certificate() const {
        return shared_identity().identity_.ca_file_;
    }
    [[nodiscard]] const std::filesystem::path& private_key() const {
        return shared_identity().private_key_;
    }
};

struct peer_state final {
    std::atomic<bool> allow_final_part_{};
    std::atomic<bool> client_end_observed_{};
    std::atomic<std::size_t> tunnel_bytes_{};
    std::atomic<unsigned> websocket_messages_{};
    std::atomic<std::size_t> request_payload_bytes_{};
    std::atomic<bool> upload_trailer_observed_{};
    std::atomic<bool> echo_{};
};

struct peer_state_source final {
    std::atomic<std::shared_ptr<peer_state>> current_{std::make_shared<peer_state>()};
};

struct peer_worker_state final {
    explicit peer_worker_state(std::shared_ptr<peer_state_source> source)
        : source_(std::move(source)) {}
    [[nodiscard]] std::shared_ptr<peer_state> state() const {
        return source_->current_.load();
    }
    std::shared_ptr<peer_state_source> source_;
};

class tunnel_metadata final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        // CONNECT commits its response head before invoking the handler.
        context.header("x-tunnel", "owned-metadata");
        co_await next();
    }
};

// Publishes an interim head before rejecting the upgrade, so the final
// rejection must follow those already-published response bytes.
class informed_reject final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next&) {
        co_await context.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "upgrade rejected after interim response"});
    }
};

// Opts a read-only route into HTTP/3 0-RTT replay.
class replay_safe_route final : public ruvia::middleware {
public:
    static constexpr bool ruvia_replay_safe = true;
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next) {
        co_await next();
    }
};

class peer_controller final : public ruvia::controller<peer_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/early", early, replay_safe_route);
    RUVIA_POST_STREAM("/upload", upload);
    RUVIA_CONNECT("target.test:443", tunnel, tunnel_metadata);
    RUVIA_CONNECT_PROTOCOL("test-protocol", "/tunnel", tunnel, tunnel_metadata);
    const ruvia::http_tunnel_route_config options{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL_OPTIONS("test-datagram", "/datagrams", datagrams, options);
    RUVIA_CONNECT_PROTOCOL_OPTIONS("connect-udp", "/udp/:host/:port", udp_echo, options);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp", udp_greeting);
    const ruvia::websocket_route_config websocket_options{.subprotocols_ = {"chat"}, .deflate_ = {.enabled_ = true}};
    RUVIA_GET_WS_OPTIONS("/ws", websocket, websocket_options);
    RUVIA_GET_WS("/ws-informed-reject", websocket, informed_reject);
    RUVIA_ROUTES_END

private:
    static void require_loopback_peer(ruvia::context& context) {
        const auto connection = context.conn();
        if (connection.remote().address() != "127.0.0.1" || connection.remote().port() == 0 ||
            connection.tls() == nullptr || connection.scheme() != ruvia::http_scheme::https) {
            throw std::runtime_error("HTTP/3 request lost its authenticated peer metadata");
        }
    }

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text("hello");
    }

    // Reports whether QUIC delivered the request in 0-RTT.
    ruvia::task<ruvia::http_response> early(ruvia::context& context) {
        co_return context.text(
            std::string_view(context.early_data_info().received_from_early_data() ? "early" : "full"));
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& context) {
        const auto state_owner = context.worker_state<peer_worker_state>().state();
        auto& state = *state_owner;
        std::string body;
        auto& reader = context.req().get_body_reader();
        while (auto chunk = co_await reader.text()) {
            body.append(*chunk);
        }
        state.request_payload_bytes_.store(body.size());
        state.upload_trailer_observed_.store(context.req().trailer("x-end") == "retained");
        co_return context.text(std::string_view(body));
    }

    ruvia::task<void> websocket(ruvia::context& context) {
        require_loopback_peer(context);
        const auto state_owner = context.worker_state<peer_worker_state>().state();
        auto& state = *state_owner;
        auto& websocket = context.get_websocket();
        const std::string expected(100000, 'c');
        co_await websocket.binary(std::string(100000, 'w'), {.compress_ = false});
        for (unsigned index = 0; index != 2; ++index) {
            auto message = co_await websocket.read();
            if (!message || message->opcode() != ruvia::websocket_opcode::binary || message->payload() != expected) {
                throw std::runtime_error("incorrect HTTP/3 WebSocket payload");
            }
            co_await websocket.binary(message->payload(), {.compress_ = index != 0});
            state.websocket_messages_.fetch_add(1);
        }
        co_await websocket.close();
        state.client_end_observed_.store(true);
    }

    ruvia::task<void> tunnel(ruvia::context& context) {
        require_loopback_peer(context);
        const auto state_owner = context.worker_state<peer_worker_state>().state();
        auto& state = *state_owner;
        auto& tunnel = context.tunnel();
        if (state.echo_.load()) {
            while (auto bytes = co_await tunnel.read()) {
                co_await tunnel.write(std::move(*bytes));
            }
            co_await tunnel.finish();
            co_return;
        }
        ruvia::task_scope outputs(context.worker());
        const auto stop = ruvia::combine_stop_tokens(context.get_stop_token(), outputs.get_stop_token());
        auto send = [&]() -> ruvia::task<void> {
            try {
                co_await tunnel.write(std::string(100003, 's'));
                while (!state.allow_final_part_.load()) {
                    if (co_await ruvia::sleep_for(context.worker(), 1ms, stop) != ruvia::timer_sleep_result::elapsed) {
                        throw std::runtime_error("HTTP/3 peer stopped before tunnel completion");
                    }
                }
                co_await tunnel.write("ended");
                co_await tunnel.finish();
            } catch (...) {
                tunnel.abort();
                throw;
            }
        };
        std::exception_ptr failure;
        try {
            outputs.spawn(send());
            // Keep the receive lane active while the large response is flow-controlled.
            // A local FIN ends only the sender; the reader owns the peer FIN observation.
            while (auto bytes = co_await tunnel.read()) {
                if (!std::ranges::all_of(*bytes, [](char byte) { return byte == 't'; })) {
                    throw std::runtime_error("incorrect HTTP/3 tunnel payload");
                }
                state.tunnel_bytes_.fetch_add(bytes->size());
            }
            state.client_end_observed_.store(true);
        } catch (...) {
            failure = std::current_exception();
            tunnel.abort();
            outputs.request_stop();
        }
        try {
            co_await outputs.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::task<void> datagrams(ruvia::context& context) {
        require_loopback_peer(context);
        auto stream = context.tunnel().datagrams();
        while (auto packet = co_await stream.read()) {
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }

    ruvia::task<void> udp_echo(ruvia::context& context) {
        require_loopback_peer(context);
        ruvia::http_udp_tunnel stream(context.tunnel().datagrams());
        while (auto packet = co_await stream.read()) {
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }

    ruvia::task<void> udp_greeting(ruvia::context& context) {
        require_loopback_peer(context);
        const auto state_owner = context.worker_state<peer_worker_state>().state();
        auto& state = *state_owner;
        ruvia::http_udp_tunnel stream(context.tunnel().datagrams({.send_policy_ = ruvia::http_datagram_send_policy::capsule}));
        co_await stream.send(std::string(16003, 's'));
        co_await stream.send("");
        co_await stream.finish();
        while (auto packet = co_await stream.read()) {
            state.tunnel_bytes_.fetch_add(packet->payload().size());
        }
        state.client_end_observed_.store(true);
    }
};

class local_http3_peer final {
    class server_lifetime final {
    public:
        explicit server_lifetime(const test_identity_files& identity)
            : states_(std::make_shared<peer_state_source>()) {
            auto& app = ruvia::app();
            // Closing QUIC connections may drain while later cases run.
            app.server({.worker_count_ = 2, .worker_queue_capacity_ = 32, .max_connections_per_worker_ = 32})
                .get_blocking_pool(nullptr)
                .use_worker_state<peer_worker_state>([states = states_] { return states; })
                .on_start([this] {
                    readiness_published_ = true;
                    ready_.set_value();
                });
            asio::io_context io;
            asio::ip::tcp::acceptor tcp(io, {asio::ip::address_v4::loopback(), 0});
            port_ = tcp.local_endpoint().port();
            asio::ip::udp::socket udp(io, {asio::ip::address_v4::loopback(), port_});
            tcp.close();
            udp.close();
            // Early data only admits replay-safe routes; other cases never offer it.
            app.listen({.address_ = "127.0.0.1", .https_ = port_, .tls_ = {.certificate_chain_file_ = identity.certificate(), .private_key_file_ = identity.private_key(), .http3_early_data_ = true}, .http3_ = {.mode_ = ruvia::http3_mode::enabled}});
            auto ready = ready_.get_future();
            thread_ = std::thread([this, &app] {
                try {
                    app.run();
                } catch (...) {
                    const auto failure = std::current_exception();
                    {
                        std::lock_guard lock(mutex_);
                        failure_ = failure;
                    }
                    if (!readiness_published_) {
                        ready_.set_exception(failure);
                    }
                }
            });
            try {
                if (ready.wait_for(5s) != std::future_status::ready) {
                    throw std::runtime_error("HTTP/3 public server startup timed out");
                }
                ready.get();
            } catch (...) {
                app.stop();
                thread_.join();
                throw;
            }
        }

        ~server_lifetime() {
            ruvia::app().stop();
            thread_.join();
        }
        void rethrow_if_failed() const {
            std::lock_guard lock(mutex_);
            if (failure_) {
                std::rethrow_exception(failure_);
            }
        }

        std::shared_ptr<peer_state_source> states_;
        std::uint16_t port_{};

    private:
        mutable std::mutex mutex_;
        std::promise<void> ready_;
        std::exception_ptr failure_;
        bool readiness_published_{};
        std::thread thread_;
    };

    [[nodiscard]] static server_lifetime& shared_server(const test_identity_files& identity) {
        static server_lifetime server(identity);
        return server;
    }

public:
    explicit local_http3_peer(const test_identity_files& identity, bool echo = false)
        : server_(shared_server(identity)),
          state_(std::make_shared<peer_state>()) {
        server_.rethrow_if_failed();
        state_->echo_.store(echo);
        // Each handler retains its case's observations. QUIC draining is not
        // a controller-completion barrier and must not gate the next case.
        server_.states_->current_.store(state_);
    }
    local_http3_peer(const local_http3_peer&) = delete;
    local_http3_peer& operator=(const local_http3_peer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return server_.port_;
    }
    [[nodiscard]] bool client_end_observed() const noexcept {
        return state_->client_end_observed_.load();
    }
    [[nodiscard]] unsigned websocket_messages() const noexcept {
        return state_->websocket_messages_.load();
    }
    [[nodiscard]] std::size_t tunnel_bytes() const noexcept {
        return state_->tunnel_bytes_.load();
    }
    [[nodiscard]] std::size_t request_payload_bytes() const noexcept {
        return state_->request_payload_bytes_.load();
    }
    [[nodiscard]] bool upload_trailer_observed() const noexcept {
        return state_->upload_trailer_observed_.load();
    }
    void allow_final_part() noexcept {
        state_->allow_final_part_.store(true);
    }
    void rethrow_if_failed() const {
        server_.rethrow_if_failed();
    }

private:
    server_lifetime& server_;
    std::shared_ptr<peer_state> state_;
};

// A failed constructor, operation, or shutdown still retires the attachment.
// The public root owner is joined before the caller reclaims captured inputs.
inline ruvia::task<void> stop_after_client_task(ruvia::event_loop_attachment& attachment, ruvia::task<void> work) {
    std::exception_ptr failure;
    try {
        co_await std::move(work);
    } catch (...) {
        failure = std::current_exception();
    }
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

inline void run_client_task(ruvia::event_loop_attachment& attachment, ruvia::task<void> work) {
    auto root = attachment.loop().start(stop_after_client_task(attachment, std::move(work)));
    attachment.run();
    root.get();
}

inline ruvia::task<bool> write_after_finish_is_rejected(ruvia::http_client_tunnel& tunnel, std::string_view bytes) {
    try {
        co_await tunnel.write(bytes);
    } catch (const ruvia::http_client_error& error) {
        if (error.code() != ruvia::http_client_error::code_type::cancelled) {
            throw;
        }
        co_return true;
    }
    co_return false;
}

template <typename predicate_type>
inline ruvia::task<bool> wait_for_peer(const ruvia::worker_handle& worker, local_http3_peer& peer,
    predicate_type predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        peer.rethrow_if_failed();
        if (std::chrono::steady_clock::now() >= deadline ||
            co_await ruvia::sleep_for(worker, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.rethrow_if_failed();
    co_return true;
}

}  // namespace http3_client_connection_test

using namespace http3_client_connection_test;  // NOLINT(google-build-using-namespace)
