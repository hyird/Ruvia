#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <string>
#include <system_error>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/websocket_client.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {

[[nodiscard]] std::string make_server_handshake_response(std::string_view request) {
    const std::string_view key_header = "Sec-WebSocket-Key: ";
    const auto key_begin = request.find(key_header);
    if (key_begin == std::string_view::npos) {
        throw std::runtime_error("missing WebSocket key");
    }
    const auto start = key_begin + key_header.size();
    const auto key_end = request.find("\r\n", start);
    const auto key = request.substr(start, key_end - start);
    const std::array headers{
        ruvia::http_header_view("Host", "127.0.0.1"),
        ruvia::http_header_view("Upgrade", "websocket"),
        ruvia::http_header_view("Connection", "Upgrade"),
        ruvia::http_header_view("Sec-WebSocket-Version", "13"),
        ruvia::http_header_view("Sec-WebSocket-Key", key),
    };
    std::pmr::monotonic_buffer_resource resource;
    auto [parsed_request, parse_error] =
        ruvia::make_parsed_http_request("GET", "/", headers, {}, &resource);
    if (parse_error) {
        throw std::runtime_error("could not parse WebSocket request");
    }
    auto handshake = ruvia::make_websocket_server_handshake(parsed_request, {.resource_ = &resource});
    std::string response;
    handshake.for_each_response_part([&response](std::string_view part) {
        response.append(part);
    });
    return response;
}

void check_peer_exchange(ruvia::testing::test_context& ruvia_ctx, bool reply_close,
    bool truncated_frame = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        // The first frame can arrive in the same transport write as the upgrade.
        std::string response = make_server_handshake_response(request);
        if (truncated_frame) {
            response.append("\x81\x05hi", 4);
        } else {
            response.append("\x82\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        if (truncated_frame) {
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
            co_return;
        }
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> payload_value{};
        co_await asio::async_read(socket, asio::buffer(payload_value.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        if (reply_close) {
            const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
            co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool succeeded = false;
    bool protocol_error = false;
    bool returned_eof = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            const auto greeting = co_await client.read();
            if (truncated_frame) {
                returned_eof = !greeting.has_value();
            } else {
                RUVIA_CHECK(greeting.has_value());
                if (greeting) {
                    RUVIA_CHECK_EQ(greeting->payload(), std::string_view("hello"));
                }
                co_await client.close({});
                succeeded = true;
            }
        } catch (const ruvia::websocket_client_error& error) {
            protocol_error = error.code() == ruvia::websocket_client_error::code_type::protocol_error;
            if (!protocol_error) {
                failure = std::current_exception();
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (truncated_frame) {
        RUVIA_CHECK(!returned_eof);
        RUVIA_CHECK(protocol_error);
    } else {
        RUVIA_CHECK(saw_close);
        RUVIA_CHECK_EQ(succeeded, reply_close);
        RUVIA_CHECK_EQ(protocol_error, !reply_close);
    }
}

}  // namespace

RUVIA_TEST(websocket_client_negotiates_deflate_and_can_skip_individual_messages) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool offered = false;
    bool compressed = false;
    bool uncompressed = false;
    const std::string payload_value(4096, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        offered = request.find("permessage-deflate") != std::string::npos;
        auto response = make_server_handshake_response(request);
        response.insert(response.size() - 2, "Sec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n");
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        ruvia::websocket_connection protocol({.compression_ = {.enabled_ = true}});
        std::array<char, 8192> bytes_value{};
        unsigned messages = 0;
        while (messages != 4) {
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            if (messages == 0) {
                compressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) != 0;
            }
            if (messages == 1) {
                uncompressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) == 0;
            }
            (void)protocol.feed(std::string_view(bytes_value.data(), count));
            while (auto event = protocol.next_event()) {
                if (const auto* message = event->message()) {
                    RUVIA_CHECK_EQ(message->payload(), payload_value);
                    ++messages;
                    RUVIA_CHECK(protocol.submit_frame(ruvia::websocket_opcode::text, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                    const auto output = protocol.output_plan();
                    co_await asio::async_write(socket, asio::buffer(output.bytes()), asio::use_awaitable);
                    (void)protocol.consume_output(output.bytes().size());
                }
            }
        }
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port(),
                                                              .deflate_ = {.enabled_ = true}});
        {
            auto cold = client.connect();
        }
        co_await client.connect();
        const auto rejects_claim = [&](auto&& make_operation, std::string_view message) {
            bool rejected = false;
            try {
                auto overlap = make_operation();
            } catch (const ruvia::websocket_client_error& error) {
                rejected = error.code() == ruvia::websocket_client_error::code_type::invalid_state &&
                           std::string_view(error.what()) == message;
            }
            RUVIA_CHECK(rejected);
        };
        {
            auto writing = client.text("discarded");
            rejects_claim([&] { return client.text("overlap"); }, "concurrent WebSocket client writes are not supported");
            rejects_claim([&] { return client.close({}); }, "WebSocket client close cannot overlap write");
            // A failed close must release its earlier read claim without releasing the live write.
            auto independent_read = client.read();
            rejects_claim([&] { return client.text("overlap"); }, "concurrent WebSocket client writes are not supported");
        }
        {
            auto reading = client.read();
            rejects_claim([&] { return client.read(); }, "concurrent WebSocket client reads are not supported");
            rejects_claim([&] { return client.close({}); }, "WebSocket client close cannot overlap read");
            auto independent_write = client.text("discarded");
        }
        {
            auto closing = client.close({});
            rejects_claim([&] { return client.read(); }, "concurrent WebSocket client reads are not supported");
            rejects_claim([&] { return client.text("overlap"); }, "concurrent WebSocket client writes are not supported");
            rejects_claim([&] { return client.close({}); }, "WebSocket client close cannot overlap read");
        }
        {
            auto reclaimed = client.close({});
        }
        for (unsigned i = 0; i < 4; ++i) {
            {
                auto cold = client.text("discarded");
            }
            std::string source_value = payload_value;
            auto operation = client.text(source_value, {.compress_ = i != 1});
            source_value.assign("changed before start");
            co_await std::move(operation);
            const auto message = co_await client.read();
            RUVIA_CHECK(message.has_value());
            if (message) {
                RUVIA_CHECK_EQ(message->payload(), payload_value);
            }
            RUVIA_CHECK(client.connected());
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(offered);
    RUVIA_CHECK(compressed);
    RUVIA_CHECK(uncompressed);
}

RUVIA_TEST(websocket_client_event_loop_stop_joins_pending_connect) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::steady_timer gate(io, std::chrono::steady_clock::time_point::max());
    std::promise<void> request_received;
    auto received_value = request_received.get_future();
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        request_received.set_value();
        std::error_code ignored;
        co_await gate.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool connect_failed = false;
    const auto connect_client = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port()});
        try {
            co_await client.connect();
        } catch (const ruvia::websocket_client_error&) {
            connect_failed = true;
        }
    };
    auto root = attachment.loop().start(connect_client());
    std::thread driver([&] { attachment.run(); });
    if (received_value.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        asio::post(io, [&gate] { gate.cancel(); });
        driver.join();
        root.get();
        throw std::runtime_error("WebSocket peer did not receive the upgrade request");
    }
    received_value.get();
    attachment.stop();
    asio::post(io, [&gate] { gate.cancel(); });
    driver.join();
    root.get();
    RUVIA_CHECK(connect_failed);
}

RUVIA_TEST(websocket_client_event_loop_stop_joins_connected_heartbeat_and_pending_read) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    bool observed_client_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        const auto payload_bytes = static_cast<std::size_t>(header_value[1] & 0x7f);
        if (header_value[0] != 0x89 || (header_value[1] & 0x80) == 0 || payload_bytes > 125) {
            throw std::runtime_error("expected a masked client heartbeat ping");
        }
        std::array<unsigned char, 4> mask{};
        co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
        std::array<unsigned char, 125> payload_value{};
        co_await asio::async_read(
            socket, asio::buffer(payload_value.data(), payload_bytes), asio::use_awaitable);
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
        observed_client_close = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool connected = false;
    bool read_cancelled = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::ws,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port(),
                                                              .heartbeat_ = {
                                                                  .ping_interval_ = std::chrono::milliseconds(20),
                                                                  .pong_timeout_ = std::chrono::seconds(5),
                                                              }});
        co_await client.connect();
        connected = client.connected();
        try {
            (void)co_await client.read();
        } catch (const ruvia::websocket_client_error&) {
            read_cancelled = true;
        }
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        root.get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not send its heartbeat ping");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    root.get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(connected);
    RUVIA_CHECK(read_cancelled);
    RUVIA_CHECK(observed_client_close);
}

RUVIA_TEST(websocket_client_close_requires_peer_close) {
    check_peer_exchange(ruvia_ctx, false);
    check_peer_exchange(ruvia_ctx, true);
}

RUVIA_TEST(websocket_client_read_rejects_eof_during_incomplete_frame) {
    check_peer_exchange(ruvia_ctx, false, true);
}

RUVIA_TEST(websocket_client_rejects_untrusted_tls_peer) {
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const auto require = [](bool success) {
        if (!success) {
            throw std::runtime_error("could not generate test certificate");
        }
    };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    require(generator != nullptr);
    require(EVP_PKEY_keygen_init(generator.get()) == 1);
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_generate(generator.get(), &raw_key) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
    require(certificate != nullptr);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    require(name != nullptr);
    require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_subject_name(certificate.get(), name.get()) == 1);
    require(X509_set_issuer_name(certificate.get(), name.get()) == 1);
    require(ruvia::test::sign_tls_certificate(certificate.get(), key.get()) > 0);
    require(SSL_CTX_use_certificate(tls.native_handle(), certificate.get()) == 1);
    require(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()) == 1);

    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool handshake_rejected = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, error));
        handshake_rejected = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool tls_failure = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.host_ = "127.0.0.1",
                                                              .port_ = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
        } catch (const ruvia::websocket_client_error& error) {
            tls_failure = error.code() == ruvia::websocket_client_error::code_type::tls_failed;
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    root.get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK(tls_failure);
    RUVIA_CHECK(handshake_rejected);
}
