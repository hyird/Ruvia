#include "http/tls_tunnel_output.h"

#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <string>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/http_client.h"

#include "http/http_socket_tunnel_transport.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {
void certificate(asio::ssl::context& context_value) {
    const auto require = [](bool valid) {
        if (!valid) {
            throw std::runtime_error("test TLS certificate creation failed");
        }
    };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048}), &EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new_ex(nullptr, nullptr), &X509_free);
    require(key && cert);
    require(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) != nullptr);
    require(X509_set_pubkey(cert.get(), key.get()) == 1);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    require(name != nullptr);
    require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_subject_name(cert.get(), name.get()) == 1);
    require(X509_set_issuer_name(cert.get(), name.get()) == 1);
    require(ruvia::test::sign_tls_certificate(cert.get(), key.get()) > 0);
    require(SSL_CTX_use_certificate(context_value.native_handle(), cert.get()) == 1);
    require(SSL_CTX_use_PrivateKey(context_value.native_handle(), key.get()) == 1);
}

template <typename stream_type>
ruvia::task<void> write_chunks(stream_type& stream, ruvia::detail::tls_tunnel_output& output, std::string_view bytes_value) {
    while (!bytes_value.empty()) {
        const auto chunk = bytes_value.substr(0, 4096);
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(stream, asio::buffer(chunk), std::move(handler));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
        if (const auto error = co_await output.flush()) {
            throw std::system_error(error);
        }
        bytes_value.remove_prefix(chunk.size());
    }
}
template <typename stream_type>
ruvia::task<std::string> read_to_end(stream_type& stream) {
    std::array<char, 4096> bytes_value{};
    std::string received;
    for (;;) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            stream.async_read_some(asio::buffer(bytes_value), std::move(handler));
        });
        if (read.error_code() == asio::error::eof) {
            co_return received;
        }
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        received.append(bytes_value.data(), read.result());
    }
}

void exercise_half_close(ruvia::testing::test_context& ruvia_ctx, int version) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource resource;
    asio::ssl::context server_tls(asio::ssl::context::tls_server);
    asio::ssl::context client_tls(asio::ssl::context::tls_client);
    certificate(server_tls);
    for (auto* context : {&server_tls, &client_tls}) {
        RUVIA_CHECK(SSL_CTX_set_min_proto_version(context->native_handle(), version) == 1);
        RUVIA_CHECK(SSL_CTX_set_max_proto_version(context->native_handle(), version) == 1);
    }
    client_tls.set_verify_mode(asio::ssl::verify_none);
    const std::string outbound(100003, 's');
    const std::string returned(80003, 'c');
    std::string received;
    bool server_finished{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket client_socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { ruvia::close_socket(client_socket); } });
        ruvia::task_scope tasks(worker_value, {.resource_ = &resource});
        auto serve = [&]() -> ruvia::task<void> {
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto handler) {
                acceptor.async_accept(std::move(handler));
            });
            if (accepted.error_code()) {
                throw std::system_error(accepted.error_code());
            }
            auto socket = std::move(accepted.result());
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, server_tls);
            const auto handshake = co_await ruvia::async_asio([&](auto handler) { stream.async_handshake(asio::ssl::stream_base::server, std::move(handler)); });
            if (handshake.error_code()) {
                throw std::system_error(handshake.error_code());
            }
            ruvia::detail::tls_tunnel_output output(*stream.native_handle(), socket, worker_value, resource);
            output.start();
            std::exception_ptr exception;
            try {
                co_await write_chunks(stream, output, outbound);
                if (const auto error = co_await output.finish()) {
                    throw std::system_error(error);
                }
                server_finished = true;
                received = co_await read_to_end(stream);
            } catch (...) {
                exception = std::current_exception();
                output.abort();
            }
            co_await output.join();
            if (exception) {
                std::rethrow_exception(exception);
            }
        };
        tasks.spawn(serve());
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { client_socket.async_connect(acceptor.local_endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            asio::ssl::stream<asio::ip::tcp::socket&> stream(client_socket, client_tls);
            const auto handshake = co_await ruvia::async_asio([&](auto handler) { stream.async_handshake(asio::ssl::stream_base::client, std::move(handler)); });
            if (handshake.error_code()) {
                throw std::system_error(handshake.error_code());
            }
            ruvia::detail::tls_tunnel_output output(*stream.native_handle(), client_socket, worker_value, resource);
            output.start();
            std::exception_ptr exception;
            try {
                const auto bytes_value = co_await read_to_end(stream);
                RUVIA_CHECK(bytes_value == outbound);
                co_await write_chunks(stream, output, returned);
                if (const auto error = co_await output.finish()) {
                    throw std::system_error(error);
                }
            } catch (...) {
                exception = std::current_exception();
                output.abort();
            }
            co_await output.join();
            if (exception) {
                std::rethrow_exception(exception);
            }
        } catch (...) {
            failure = std::current_exception();
            ruvia::close_socket(client_socket);
            std::error_code ignored;
            acceptor.close(ignored);
        }
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        (void)watchdog_value.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(server_finished);
    RUVIA_CHECK(received == returned);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
}  // namespace

RUVIA_TEST(tls_tunnel_output_flushes_close_notify_and_reads_peer_data_after_local_finish) {
    exercise_half_close(ruvia_ctx, TLS1_3_VERSION);
    exercise_half_close(ruvia_ctx, TLS1_2_VERSION);
}

namespace {
template <typename stream_type>
ruvia::task<void> serve_client_connect(stream_type& stream, ruvia::detail::tls_tunnel_output* tls, unsigned mode, std::string& received_value) {
    std::string head;
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    if (!head.starts_with("CONNECT target.test:443 HTTP/1.1\r\n")) {
        throw std::runtime_error("invalid CONNECT authority-form");
    }
    if (head.find("Content-Length:") != std::string::npos || head.find("Transfer-Encoding:") != std::string::npos) {
        throw std::runtime_error("CONNECT head has HTTP content framing");
    }
    ruvia::detail::http_socket_tunnel_transport transport(stream, tls);
    const auto send = [&](std::string_view bytes_value, bool end = false) -> ruvia::task<void> {
        const auto error = co_await transport.write_bytes(bytes_value, end ? ruvia::detail::http_stream_end::end : ruvia::detail::http_stream_end::keep_open);
        if (error) {
            throw std::system_error(error);
        }
    };
    if (mode == 2) {
        co_await send("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nConnection: close\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied", true);
        co_return;
    }
    co_await send("HTTP/1.1 200 Connection Established\r\nX-Tunnel: owned-metadata\r\n\r\n");
    if (mode == 1) {
        co_await send(std::string(100003, 's'), true);
    }
    received_value = co_await read_to_end(stream);
    if (mode == 0) {
        co_await send(std::string(100003, 's'), true);
    }
}
void exercise_client_connect(ruvia::testing::test_context& ruvia_ctx, bool encrypted, unsigned mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ssl::context tls_context(asio::ssl::context::tls_server);
    certificate(tls_context);
    ruvia::test::counting_memory_resource resource;
    std::string received;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
            if (accepted.error_code()) {
                throw std::system_error(accepted.error_code());
            }
            auto socket = std::move(accepted.result());
            if (encrypted) {
                asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, tls_context);
                const auto handshake = co_await ruvia::async_asio([&](auto h) { stream.async_handshake(asio::ssl::stream_base::server, std::move(h)); });
                if (handshake.error_code()) {
                    throw std::system_error(handshake.error_code());
                }
                ruvia::detail::tls_tunnel_output output(*stream.native_handle(), socket, worker_value, resource);
                output.start();
                std::exception_ptr error;
                try {
                    co_await serve_client_connect(stream, &output, mode, received);
                } catch (...) {
                    error = std::current_exception();
                    output.abort();
                }
                co_await output.join();
                if (error) {
                    std::rethrow_exception(error);
                }
            } else {
                co_await serve_client_connect(socket, nullptr, mode, received);
            }
        };
        tasks.spawn(serve());
        ruvia::http_client client(attachment.loop(), {.scheme_ = encrypted ? ruvia::http_scheme::https : ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port(), .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http1_only, .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification});
        try {
            auto result_value = co_await client.open_tunnel({.authority_ = "target.test:443"});
            if (mode == 2) {
                RUVIA_CHECK(!result_value.tunnel() && result_value.response());
                if (!result_value.response()) {
                    throw std::runtime_error("missing CONNECT rejection");
                }
                RUVIA_CHECK(result_value.response()->status().value() == 407);
                std::string text;
                while (auto bytes = co_await result_value.response()->body().text()) {
                    text.append(*bytes);
                }
                RUVIA_CHECK(text == "denied");
            } else {
                RUVIA_CHECK(result_value.tunnel() && !result_value.response());
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                const auto read_greeting = [&]() -> ruvia::task<void> {
                    std::string greeting;
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's'));
                };
                if (mode == 1) {
                    co_await read_greeting();
                }
                for (unsigned part = 0; part != 8; ++part) {
                    std::string payload_value(16384, 't');
                    auto write = tunnel.write(std::string_view(payload_value));
                    payload_value.assign("mutated");
                    co_await std::move(write);
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                if (mode == 0) {
                    co_await read_greeting();
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (mode != 2) {
        RUVIA_CHECK(received == std::string(8 * 16384, 't'));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
}  // namespace
RUVIA_TEST(http1_client_tunnel_and_rejections_preserve_ownership_and_tcp_tls_half_close) {
    for (bool tls : {false, true}) {
        for (unsigned mode = 0; mode != 3; ++mode) {
            exercise_client_connect(ruvia_ctx, tls, mode);
        }
    }
}
