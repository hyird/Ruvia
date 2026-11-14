#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http2_connection.h"
#include "http_client.h"
#include "http_client_fixture.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace {
// One prior-knowledge HTTP/2 connection served by the sans-I/O core, which
// answers PINGs as it reads. The mode decides each request's answer.
class http2_liveness_peer final {
public:
    enum class mode_type : std::uint8_t {
        answer,
        // The request is answered after the client sent two liveness PINGs.
        answer_after_pings,
        // After the request the peer reads but neither processes nor writes.
        silent_after_request,
    };

    http2_liveness_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, mode_type mode) :
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        guard_(io),
        done_(worker_value),
        io_(io),
        mode_(mode) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            if (socket_) {
                socket_->close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            done_.notify();
        });
    }

    // Completes once the client closed the connection.
    [[nodiscard]] ruvia::task<void> wait() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    // From the last response to the client's close.
    std::chrono::steady_clock::duration idle_for_{};
    // The client sent GOAWAY before it closed (RFC 9113 section 6.8).
    bool goaway_received_{false};

private:
    // A liveness PING the client sent: length 8, type PING, no ACK flag.
    [[nodiscard]] static std::size_t count_pings(std::string_view bytes) noexcept {
        static constexpr std::string_view ping_header("\0\0\x08\x06\0", 5);
        std::size_t count = 0;
        for (auto at = bytes.find(ping_header); at != std::string_view::npos; at = bytes.find(ping_header, at + 1)) {
            ++count;
        }
        return count;
    }

    void respond(ruvia::http2_connection& connection, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body("ok");
        const auto head = connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 liveness peer response rejected");
        }
        responded_ = std::chrono::steady_clock::now();
    }

    asio::awaitable<void> serve() {
        socket_.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        std::vector<ruvia::http2_request_head_event> stalled{};
        std::optional<ruvia::http2_request_head_event> pending;
        std::size_t pings = 0;
        bool silent = false;
        for (;;) {
            std::error_code error;
            if (!silent && connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                // The core answers a client GOAWAY with its own, which may
                // reach a client that already closed.
                co_await asio::async_write(*socket_, asio::buffer(output), asio::redirect_error(asio::use_awaitable, error));
            }
            std::size_t count = 0;
            if (!error) {
                count = co_await socket_->async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error) {
                idle_for_ = std::chrono::steady_clock::now() - responded_;
                co_return;
            }
            if (silent) {
                continue;
            }
            const auto bytes = std::string_view(input.data(), count);
            if (pending) {
                pings += count_pings(bytes);
            }
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 liveness peer input");
                }
                while (auto event = connection.next_event()) {
                    if (event->goaway() != nullptr) {
                        goaway_received_ = true;
                    }
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    switch (mode_) {
                        case mode_type::answer:
                            respond(connection, std::move(*request));
                            break;
                        case mode_type::answer_after_pings:
                            pending.emplace(std::move(*request));
                            break;
                        case mode_type::silent_after_request:
                            stalled.push_back(std::move(*request));
                            silent = true;
                            break;
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
            if (pending && pings >= 2) {
                respond(connection, std::move(*pending));
                pending.reset();
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    ruvia::worker_signal done_;
    asio::io_context& io_;
    mode_type mode_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::chrono::steady_clock::time_point responded_{std::chrono::steady_clock::now()};
    std::exception_ptr failure_;
    bool finished_{false};
};

[[nodiscard]] ruvia::http_client_config local_http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(20);
    return config;
}

namespace http_client_pool_test {

// One server connection of a scripted HTTP/2 origin over TCP or TLS, driven by
// the sans-I/O core, which answers SETTINGS and PING itself. Request heads
// queue in arrival order.
template <typename stream_type>
class http2_origin_connection final {
public:
    http2_origin_connection(scripted_origin& origin, stream_type stream) :
        stream_(std::move(stream)) {
        origin.watch(stream_.lowest_layer());
    }

    [[nodiscard]] ruvia::http2_connection& connection() noexcept {
        return connection_;
    }
    [[nodiscard]] bool goaway_received() const noexcept {
        return goaway_received_;
    }

    // Submit capacity settings through the protocol engine so its ACK accounting
    // stays synchronized with the peer, then write every pending frame.
    [[nodiscard]] asio::awaitable<bool> flush(std::optional<std::uint8_t> stream_limit = std::nullopt) {
        if (stream_limit && connection_.submit_max_concurrent_streams(*stream_limit) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 scripted origin settings rejected");
        }
        ruvia::test::drain_output(connection_, output_);
        if (output_.empty()) {
            co_return true;
        }
        std::error_code error;
        co_await asio::async_write(stream_, asio::buffer(output_), asio::redirect_error(asio::use_awaitable, error));
        co_return !error;
    }
    [[nodiscard]] asio::awaitable<ruvia::http2_request_head_event> next_request() {
        while (requests_.empty()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection before a request");
            }
        }
        auto request = std::move(requests_.front());
        requests_.pop_front();
        co_return request;
    }
    // Reads until the drain this side started is final and written.
    [[nodiscard]] asio::awaitable<void> finish_drain() {
        while (!connection_.drain_final() || connection_.wants_write()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection during its drain");
            }
        }
    }
    [[nodiscard]] asio::awaitable<void> read_until_end() {
        while (co_await read()) {
        }
    }
    void respond(ruvia::http2_request_head_event&& request, std::string_view body) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body(body);
        const auto head = connection_.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection_.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection_.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("scripted HTTP/2 response rejected");
        }
    }
    void refuse(ruvia::http2_request_head_event&& request, ruvia::http2_error_code error) {
        if (connection_.submit_reset(request.stream_id(), error) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("scripted HTTP/2 reset rejected");
        }
        (void)connection_.release(std::move(request));
    }
    // An abrupt end of the connection, without GOAWAY.
    void close() {
        std::error_code ignored;
        stream_.lowest_layer().close(ignored);
    }
    // Sends FIN; reading continues until the client closes.
    void end_output() {
        std::error_code ignored;
        stream_.lowest_layer().shutdown(asio::socket_base::shutdown_send, ignored);
    }

private:
    // One read: feeds the core, queues request heads and writes its answers.
    [[nodiscard]] asio::awaitable<bool> read() {
        if (!co_await flush()) {
            co_return false;
        }
        std::error_code error;
        const auto count = co_await stream_.async_read_some(asio::buffer(input_), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return false;
        }
        const auto bytes = std::string_view(input_.data(), count);
        for (;;) {
            const auto status = connection_.feed(bytes);
            if (status == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("invalid client HTTP/2 input");
            }
            while (auto event = connection_.next_event()) {
                if (event->goaway() != nullptr) {
                    goaway_received_ = true;
                }
                if (auto* request = event->request_head()) {
                    requests_.push_back(std::move(*request));
                }
            }
            if (status != ruvia::http2_feed_result::events_pending) {
                break;
            }
        }
        co_return co_await flush();
    }

    stream_type stream_;
    ruvia::http2_connection connection_{ruvia::http2_connection::server()};
    std::array<char, 16384> input_{};
    std::string output_;
    std::deque<ruvia::http2_request_head_event> requests_{};
    bool goaway_received_{false};
};

using tcp_origin_connection = http2_origin_connection<asio::ip::tcp::socket>;

[[nodiscard]] ruvia::http_client_config http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(5);
    return config;
}

[[nodiscard]] ruvia::task<void> send_and_discard(ruvia::http_client& client, std::string_view target) {
    try {
        auto response = co_await client.client().get(target).send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error&) {
        // Ended by the client's shutdown.
    }
}

[[nodiscard]] ruvia::task<void> send_and_keep(ruvia::http_client& client, std::string_view target, std::optional<std::string>& body, bool& done, ruvia::worker_signal& done_signal) {
    try {
        auto response = co_await client.client().get(target).send();
        const auto bytes_value = co_await response.stream().read_all();
        body.emplace(response_text(bytes_value));
    } catch (const ruvia::http_client_error&) {
        // `body` stays unset; the caller's check reports it.
    }
    done = true;
    done_signal.notify();
}

}  // namespace http_client_pool_test

}  // namespace

TEST(Ruvia, http2_client_uses_available_stream_capacity_across_connections) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::array<std::optional<asio::ip::tcp::socket>, 2> sockets;
    std::array<ruvia::http2_connection, 2> connections{ruvia::http2_connection::server(), ruvia::http2_connection::server()};
    ruvia::worker_signal peer_done(worker.handle_);
    asio::steady_timer deadline(io);
    std::exception_ptr peer_failure;
    bool peer_finished = false;

    const auto flush = [&](std::size_t index, std::optional<std::uint32_t> stream_limit = std::nullopt) -> asio::awaitable<void> {
        if (stream_limit && connections[index].submit_max_concurrent_streams(*stream_limit) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 capacity settings rejected");
        }
        std::string output;
        ruvia::test::drain_output(connections[index], output);
        if (!output.empty()) {
            co_await asio::async_write(*sockets[index], asio::buffer(output), asio::use_awaitable);
        }
    };
    const auto read_request = [&](std::size_t index) -> asio::awaitable<ruvia::http2_request_head_event> {
        std::array<char, 16384> input{};
        for (;;) {
            while (auto event = connections[index].next_event()) {
                if (auto* request = event->request_head()) {
                    co_return std::move(*request);
                }
            }
            const auto count = co_await sockets[index]->async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connections[index].feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 capacity test input");
                }
                while (auto event = connections[index].next_event()) {
                    if (auto* request = event->request_head()) {
                        co_return std::move(*request);
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    const auto respond = [&](std::size_t index, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        const auto submitted = connections[index].submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted) || connections[index].release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 capacity test response rejected");
        }
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        // Concurrent requests of one route share the first connection's
        // negotiation; its single stream slot then sends the next request to
        // a second connection, which has two.
        sockets[0].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(0, 1);
        sockets[1].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(1, 2);

        auto first_warm = co_await read_request(0);
        auto second_warm = co_await read_request(1);
        respond(1, std::move(second_warm));
        co_await flush(1);
        respond(0, std::move(first_warm));
        co_await flush(0);

        auto saturated = co_await read_request(0);
        auto available = co_await read_request(1);
        auto rerouted = co_await read_request(1);
        if (rerouted.build_request().target() != "/probe") {
            throw std::runtime_error("HTTP/2 request was not routed to available stream capacity");
        }
        respond(0, std::move(saturated));
        respond(1, std::move(available));
        respond(1, std::move(rerouted));
        co_await flush(0);
        co_await flush(1);
        for (auto& socket : sockets) {
            std::error_code ignored;
            socket->close(ignored);
        }
    };
    deadline.expires_after(std::chrono::seconds(3));
    deadline.async_wait([&](const std::error_code& error) {
        if (error) {
            return;
        }
        std::error_code ignored;
        acceptor.close(ignored);
        for (auto& socket : sockets) {
            if (socket) {
                socket->close(ignored);
            }
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        deadline.cancel();
        peer_done.notify();
    });

    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(2);
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send = [&](ruvia::http_client& client_value, std::string_view target) -> ruvia::task<void> {
        auto response = co_await client_value.client().get(target).send();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        EXPECT_TRUE((co_await response.stream().read_all()).empty());
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            ruvia::task_scope warm(worker.handle_);
            warm.spawn(send(client, "/warm-first"));
            warm.spawn(send(client, "/warm-second"));
            co_await warm.join();
        }
        {
            ruvia::task_scope requests(worker.handle_);
            requests.spawn(send(client, "/hold-saturated"));
            requests.spawn(send(client, "/hold-available"));
            requests.spawn(send(client, "/probe"));
            co_await requests.join();
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http2_cold_start_cancelled_waiter_does_not_close_active_session) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::stop_source cancel_second;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        // Cancel the second queued request while the first is completing the
        // HTTP/2 connection preface, so the second wakes from http1_scheduler_
        // after protocol_ is already http2.
        cancel_second.request_stop();
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        bool answered = false;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            if (answered && !connection.wants_write()) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto stream_id = request->stream_id();
                    const auto built = request->build_request();
                    ruvia::http_response response;
                    response.body("first-ok");
                    const auto head = connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ != ruvia::http2_data_submit_status::complete || connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                        throw std::runtime_error("HTTP/2 response submission failed");
                    }
                    answered = true;
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send_first = [](ruvia::http_client& c, bool& ok_out) -> ruvia::task<void> {
        auto response = co_await c.client().get("/first").send();
        const auto body = co_await response.stream().read_all();
        ok_out = response.status() == ruvia::http_status::ok && std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()) == "first-ok";
    };
    const auto send_second = [](ruvia::http_client& c, ruvia::stop_token token, bool& cancelled_out) -> ruvia::task<void> {
        try {
            (void)co_await c.client().with_options({.stop_token_ = std::move(token)}).get("/second").send();
        } catch (const ruvia::http_client_error& error) {
            cancelled_out = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope tasks(worker.handle_);
        bool first_ok = false;
        bool second_cancelled = false;
        tasks.spawn(send_first(client, first_ok));
        tasks.spawn(send_second(client, cancel_second.token(), second_cancelled));
        co_await tasks.join();
        EXPECT_TRUE(first_ok);
        EXPECT_TRUE(second_cancelled);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_alpn_renegotiates_after_http2_disconnect_and_closes_on_mismatch) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    std::atomic<bool> offer_h2{true};
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void* arg) -> int {
        const bool want_h2 = static_cast<std::atomic<bool>*>(arg)->load();
        const std::string_view target = want_h2 ? "h2" : "http/1.1";
        unsigned int pos = 0;
        while (pos < inlen) {
            const unsigned int len = in[pos++];
            if (pos + len <= inlen && std::string_view(reinterpret_cast<const char*>(in + pos), len) == target) {
                *out = in + pos;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            pos += len;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }, &offer_h2);

    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool mismatch_socket_closed = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        // 1st connection: negotiate h2, serve one request, then close.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            auto connection = ruvia::http2_connection::server();
            std::array<char, 16384> input{};
            std::string output;
            bool answered = false;
            for (;;) {
                if (connection.wants_write()) {
                    ruvia::test::drain_output(connection, output);
                    co_await asio::async_write(stream, asio::buffer(output), asio::use_awaitable);
                }
                // The drain ends with the final GOAWAY sent after the client's
                // PING ACK, or earlier when the retiring client closes first.
                if (answered && connection.drain_final() && !connection.wants_write()) {
                    break;
                }
                std::error_code read_error;
                const auto count = co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, read_error));
                if (read_error && answered) {
                    break;
                }
                if (read_error) {
                    throw std::system_error(read_error);
                }
                (void)connection.feed(std::string_view(input.data(), count));
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto stream_id = request->stream_id();
                        const auto built = request->build_request();
                        ruvia::http_response response;
                        response.body("h2-ok");
                        (void)connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                        (void)connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535);
                        (void)connection.release(std::move(*request));
                        // GOAWAY precedes the close so the client retires this
                        // session deterministically: seen before the next send it
                        // opens a new connection; the PING round trip then
                        // advertises last_stream_id = 1 before the close.
                        connection.begin_drain();
                        answered = true;
                    }
                }
            }
            offer_h2.store(false);
            std::error_code ignored;
            stream.lowest_layer().close(ignored);
        }
        // 2nd connection: negotiate http/1.1 for the negotiate client.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nh1-ok";
            co_await asio::async_write(stream, asio::buffer(reply), asio::use_awaitable);
        }
        // 3rd connection: http2_only client rejects http/1.1 ALPN and closes socket.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 16> buf{};
            std::error_code read_ec;
            (void)co_await stream.next_layer().async_read_some(asio::buffer(buf), asio::redirect_error(asio::use_awaitable, read_ec));
            mismatch_socket_closed = read_ec == asio::error::eof || read_ec == asio::error::connection_reset;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{};
    ruvia::http_client negotiate_client(worker.attachment_.loop(), config);

    auto h2_only_config = config;
    h2_only_config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client h2_only_client(worker.attachment_.loop(), h2_only_config);

    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto first = co_await negotiate_client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http2);
            const auto first_body = co_await first.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()), "h2-ok");
        }
        {
            auto second = co_await negotiate_client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            const auto second_body = co_await second.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(second_body.bytes().data()), second_body.size()), "h1-ok");
        }
        bool protocol_unavailable = false;
        try {
            (void)co_await h2_only_client.client().get("/mismatch").send();
        } catch (const ruvia::http_client_error& error) {
            protocol_unavailable = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_unavailable);
        }
        EXPECT_TRUE(protocol_unavailable);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(mismatch_socket_closed);
        co_await negotiate_client.shutdown();
        co_await h2_only_client.shutdown();
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
}

// A stream timeout on a session that read nothing since the stream's head
// evicts the session: the client closes the black-holed connection instead of
// returning it to idle, and the next request opens a new one.
TEST(Ruvia, http2_client_stream_timeout_evicts_a_silent_session) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    unsigned connections = 0;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tcp_origin_connection silent(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
            ++connections;
            (void)co_await silent.flush();
            // A complete exchange settles the SETTINGS handshake first.
            silent.respond(co_await silent.next_request(), "prime");
            (void)co_await silent.flush();
            // Then the peer never answers or writes again. Only the client's
            // eviction ends this connection before the next one is served.
            [[maybe_unused]] const auto held = co_await silent.next_request();
            co_await silent.read_until_end();
        }
        tcp_origin_connection fresh(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        ++connections;
        (void)co_await fresh.flush();
        fresh.respond(co_await fresh.next_request(), "fresh");
        (void)co_await fresh.flush();
        co_await fresh.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.read_timeout_ = std::chrono::milliseconds(200);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto prime = co_await client.client().get("/prime").send();
            EXPECT_EQ(response_text(co_await prime.stream().read_all()), "prime");
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().get("/silent").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
            auto fresh = co_await client.client().get("/fresh").send();
            EXPECT_EQ(response_text(co_await fresh.stream().read_all()), "fresh");
        });
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

// A stream timeout on a session that delivered bytes for another stream since
// the timed-out stream's head resets only that stream: the next request
// reuses the session.
TEST(Ruvia, http2_client_stream_timeout_keeps_a_session_that_answers_other_streams) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        // The only connection: a second one would never be served.
        tcp_origin_connection only(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await only.flush();
        [[maybe_unused]] const auto stalled = co_await only.next_request();
        origin.advance();
        only.respond(co_await only.next_request(), "answered");
        (void)co_await only.flush();
        only.respond(co_await only.next_request(), "after");
        (void)co_await only.flush();
        co_await only.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.read_timeout_ = std::chrono::milliseconds(300);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::error_code> stalled_code;
    bool stalled_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    const auto send_stalled = [&]() -> ruvia::task<void> {
        try {
            (void)co_await client.client().get("/stall").send();
        } catch (const ruvia::http_client_error& error) {
            stalled_code = error.code();
        }
        stalled_done = true;
        done_signal.notify();
    };
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_stalled());
            // The stalled head reached the peer before the answered one.
            co_await origin.reach(1);
            auto answered = co_await client.client().get("/answer").send();
            EXPECT_EQ(response_text(co_await answered.stream().read_all()), "answered");
            while (!stalled_done) {
                co_await done_signal.wait();
            }
            EXPECT_TRUE(stalled_code == std::make_error_code(std::errc::timed_out));
            auto after = co_await client.client().get("/after").send();
            EXPECT_EQ(response_text(co_await after.stream().read_all()), "after");
        });
    };
    run_operation(worker, io, operation);
}

// idle_timeout_ closes an HTTP/2 connection once its last stream is gone,
// with GOAWAY before the close (RFC 9113 section 6.8).
TEST(Ruvia, http2_client_idle_timeout_closes_a_connection_without_streams) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.idle_timeout_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/ok").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        // Only the idle bound closes the connection before the guard does.
        co_await peer.wait();
        EXPECT_TRUE(peer.idle_for_ < std::chrono::seconds(5));
        EXPECT_TRUE(peer.goaway_received_);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// http2_ping_interval_: a connection that stays silent for an interval gets a
// PING, and one that stays silent for another fails with timeout.
TEST(Ruvia, http2_client_ping_fails_a_silent_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::silent_after_request);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::error_code> code;
        try {
            (void)co_await client.client().get("/silent").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        }
        EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        // The failed session closed its transport.
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// A peer that acknowledges the liveness PINGs keeps a quiet connection alive
// beyond two intervals.
TEST(Ruvia, http2_client_ping_ack_keeps_a_quiet_connection_alive) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer_after_pings);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/slow").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        co_await client.shutdown();
        co_await peer.wait();
    };
    run_operation(worker, io, operation);
}

// A draining HTTP/2 session never holds a new request beyond acquire_timeout_,
// even without a request timeout: the wait for the slot's reconnect is a
// connection acquisition (Pingora never reuses a shutting-down connection).
TEST(Ruvia, http2_client_draining_session_bounds_a_new_request_by_acquire_timeout) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await first.flush();
        // The held request stays open across the drain.
        [[maybe_unused]] const auto held = co_await first.next_request();
        first.connection().begin_drain();
        co_await first.finish_drain();
        origin.advance();
        co_await first.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(200);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/next").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
        });
    };
    run_operation(worker, io, operation);
}

// A request routed to a draining HTTP/2 session uses a free, unconnected slot
// instead of waiting for the drain. With two connections of two streams each,
// leases alternate between the slots: /b and /c complete in that order, so
// /next takes the lease of the draining slot.
TEST(Ruvia, http2_client_draining_session_yields_to_a_free_slot) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection draining(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await draining.flush();
        [[maybe_unused]] const auto held = co_await draining.next_request();
        origin.advance();
        // Requests share the session while its two streams last.
        auto c = co_await draining.next_request();
        origin.advance();
        tcp_origin_connection closing(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await closing.flush();
        auto b = co_await closing.next_request();
        origin.advance();
        closing.respond(std::move(b), "b");
        (void)co_await closing.flush();
        // The peer ends this connection: its slot becomes free and unconnected.
        closing.end_output();
        co_await origin.wait_for_release();
        draining.respond(std::move(c), "c");
        (void)co_await draining.flush();
        draining.connection().begin_drain();
        co_await draining.finish_drain();
        origin.advance();
        tcp_origin_connection fresh(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await fresh.flush();
        fresh.respond(co_await fresh.next_request(), "next");
        (void)co_await fresh.flush();
        co_await closing.read_until_end();
        co_await fresh.read_until_end();
        co_await draining.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.acquire_timeout_ = std::chrono::milliseconds(300);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> b_body;
    std::optional<std::string> c_body;
    bool b_done = false;
    bool c_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            requests.spawn(send_and_keep(client, "/c", c_body, c_done, done_signal));
            co_await origin.reach(2);
            requests.spawn(send_and_keep(client, "/b", b_body, b_done, done_signal));
            co_await origin.reach(3);
            while (!b_done) {
                co_await done_signal.wait();
            }
            // Its driver releases the lease after the body completed.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!c_done) {
                co_await done_signal.wait();
            }
            co_await origin.reach(4);
            // The ended connection's session retires meanwhile.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            EXPECT_TRUE(b_body == std::optional<std::string>(std::string("b")));
            EXPECT_TRUE(c_body == std::optional<std::string>(std::string("c")));
            auto next = co_await client.client().get("/next").send();
            EXPECT_EQ(response_text(co_await next.stream().read_all()), "next");
        });
    };
    run_operation(worker, io, operation);
}

// An idempotent request whose reused HTTP/2 connection fails at the transport
// after its head was sent is retried once on a new connection, like an HTTP/1
// exchange (Pingora RetryType::ReusedOnly); a non-idempotent one is not.
TEST(Ruvia, http2_client_retries_only_idempotent_requests_after_a_reused_connection_fails) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    unsigned connections = 0;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
            ++connections;
            (void)co_await first.flush();
            first.respond(co_await first.next_request(), "prime");
            (void)co_await first.flush();
            (void)co_await first.next_request();
            // No GOAWAY: an L4 middlebox reset, for example.
            first.close();
        }
        tcp_origin_connection second(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        ++connections;
        (void)co_await second.flush();
        second.respond(co_await second.next_request(), "retried");
        (void)co_await second.flush();
        (void)co_await second.next_request();
        second.close();
        std::error_code accept_error;
        [[maybe_unused]] auto extra = co_await origin.acceptor().async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (!accept_error) {
            ++connections;
        }
    };
    origin.start(script());
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    ruvia::http_client client(worker.attachment_.loop(), http2_client_config(client_url));
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto prime = co_await client.client().get("/prime").send();
            EXPECT_EQ(response_text(co_await prime.stream().read_all()), "prime");
            auto retried = co_await client.client().get("/retry").send();
            EXPECT_EQ(response_text(co_await retried.stream().read_all()), "retried");
            std::optional<std::error_code> post_failure;
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
            EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.completed_requests_, std::size_t{2});
            EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        });
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

// A peer SETTINGS_MAX_CONCURRENT_STREAMS below the local limit parks a request
// for stream capacity at most acquire_timeout_, even without a request
// timeout; a parked request proceeds once a stream closes.
TEST(Ruvia, http2_client_stream_capacity_wait_is_an_acquisition) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection only(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await only.flush(std::uint8_t{1});
        auto held = co_await only.next_request();
        origin.advance();
        co_await origin.wait_for_release();
        only.respond(std::move(held), "held");
        (void)co_await only.flush();
        only.respond(co_await only.next_request(), "after");
        (void)co_await only.flush();
        co_await only.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(500);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> held_body;
    std::optional<std::string> after_body;
    bool held_done = false;
    bool after_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_keep(client, "/held", held_body, held_done, done_signal));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/blocked").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
            requests.spawn(send_and_keep(client, "/after", after_body, after_done, done_signal));
            // Parked for capacity before the held stream closes.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!held_done || !after_done) {
                co_await done_signal.wait();
            }
            EXPECT_TRUE(held_body == std::optional<std::string>(std::string("held")));
            EXPECT_TRUE(after_body == std::optional<std::string>(std::string("after")));
        });
    };
    run_operation(worker, io, operation);
}

// RFC 9113 section 7: RST_STREAM(HTTP_1_1_REQUIRED) before any response head
// makes a negotiate client retry the request over HTTP/1.1, and every later
// connection offers only http/1.1 (Pingora's prefer_h1).
TEST(Ruvia, http_client_retries_over_http11_after_http11_required) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    // Whether each handshake offered h2; the origin selects it when offered.
    std::vector<bool> offered_h2;
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen, void* arg) -> int {
        bool h2 = false;
        const unsigned char* http11 = nullptr;
        for (unsigned int pos = 0; pos < inlen;) {
            const unsigned int len = in[pos++];
            if (pos + len > inlen) {
                break;
            }
            const std::string_view protocol(reinterpret_cast<const char*>(in + pos), len);
            if (protocol == "h2") {
                h2 = true;
                *out = in + pos;
                *outlen = static_cast<unsigned char>(len);
            } else if (protocol == "http/1.1") {
                http11 = in + pos;
            }
            pos += len;
        }
        static_cast<std::vector<bool>*>(arg)->push_back(h2);
        if (h2) {
            return SSL_TLSEXT_ERR_OK;
        }
        if (http11 != nullptr) {
            *out = http11;
            *outlen = 8;
            return SSL_TLSEXT_ERR_OK;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }, &offered_h2);
    scripted_origin origin(io, worker.handle_);
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            http2_origin_connection<tls_stream> refusing(origin, std::move(stream));
            (void)co_await refusing.flush();
            refusing.refuse(co_await refusing.next_request(), ruvia::http2_error_code::http11_required);
            (void)co_await refusing.flush();
            co_await refusing.read_until_end();
        }
        tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
        origin.watch(stream.lowest_layer());
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const std::string_view body : {std::string_view("h1-ok"), std::string_view("h1-again")}) {
            std::string request{};
            co_await asio::async_read_until(stream, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
        std::array<char, 64> input{};
        std::error_code error;
        while (!error) {
            (void)co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        }
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(origin.port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(5);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto first = co_await client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await first.stream().read_all()), "h1-ok");
            auto second = co_await client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await second.stream().read_all()), "h1-again");
        });
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
    EXPECT_TRUE(offered_h2 == std::vector<bool>({true, false}));
}
