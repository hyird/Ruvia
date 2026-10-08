#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <string>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "ruvia/core/Async.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Socket.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/web/HttpClient.h"

#include "http/HttpSocketTunnelTransport.h"
#include "http/TlsTunnelOutput.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
void certificate(asio::ssl::context& context) {
    const auto require = [](bool valid) {
        if (!valid) {
            throw std::runtime_error("test TLS certificate creation failed");
        }
    };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), &EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), &X509_free);
    require(key && cert);
    require(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) != nullptr);
    require(X509_set_pubkey(cert.get(), key.get()) == 1);
    auto* name = X509_get_subject_name(cert.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_issuer_name(cert.get(), name) == 1);
    require(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0);
    require(SSL_CTX_use_certificate(context.native_handle(), cert.get()) == 1);
    require(SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) == 1);
}

template <typename Stream>
ruvia::Task<void> writeChunks(Stream& stream, ruvia::detail::TlsTunnelOutput& output, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto chunk = bytes.substr(0, 4096);
        const auto written = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) {
            asio::async_write(stream, asio::buffer(chunk), std::move(handler));
        });
        if (written.errorCode()) {
            throw std::system_error(written.errorCode());
        }
        if (const auto error = co_await output.flush()) {
            throw std::system_error(error);
        }
        bytes.remove_prefix(chunk.size());
    }
}
template <typename Stream>
ruvia::Task<std::string> readToEnd(Stream& stream) {
    std::array<char, 4096> bytes{};
    std::string received;
    for (;;) {
        const auto read = co_await ruvia::asyncAsio<std::size_t>([&](auto handler) {
            stream.async_read_some(asio::buffer(bytes), std::move(handler));
        });
        if (read.errorCode() == asio::error::eof) {
            co_return received;
        }
        if (read.errorCode()) {
            throw std::system_error(read.errorCode());
        }
        received.append(bytes.data(), read.result());
    }
}

void exerciseHalfClose(ruvia::testing::TestContext& ruvia_ctx, int version) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource resource;
    asio::ssl::context serverTls(asio::ssl::context::tls_server);
    asio::ssl::context clientTls(asio::ssl::context::tls_client);
    certificate(serverTls);
    for (auto* context : {&serverTls, &clientTls}) {
        RUVIA_CHECK(SSL_CTX_set_min_proto_version(context->native_handle(), version) == 1);
        RUVIA_CHECK(SSL_CTX_set_max_proto_version(context->native_handle(), version) == 1);
    }
    clientTls.set_verify_mode(asio::ssl::verify_none);
    const std::string outbound(100003, 's');
    const std::string returned(80003, 'c');
    std::string received;
    bool serverFinished{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket clientSocket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) { if (!error) { ruvia::closeSocket(clientSocket); } });
        ruvia::TaskScope tasks(worker, {.resource = &resource});
        auto serve = [&]() -> ruvia::Task<void> {
            auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto handler) {
                acceptor.async_accept(std::move(handler));
            });
            if (accepted.errorCode()) {
                throw std::system_error(accepted.errorCode());
            }
            auto socket = std::move(accepted.result());
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, serverTls);
            const auto handshake = co_await ruvia::asyncAsio([&](auto handler) { stream.async_handshake(asio::ssl::stream_base::server, std::move(handler)); });
            if (handshake.errorCode()) {
                throw std::system_error(handshake.errorCode());
            }
            ruvia::detail::TlsTunnelOutput output(*stream.native_handle(), socket, worker, resource);
            output.start();
            std::exception_ptr exception;
            try {
                co_await writeChunks(stream, output, outbound);
                if (const auto error = co_await output.finish()) {
                    throw std::system_error(error);
                }
                serverFinished = true;
                received = co_await readToEnd(stream);
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
            const auto connected = co_await ruvia::asyncAsio([&](auto handler) { clientSocket.async_connect(acceptor.local_endpoint(), std::move(handler)); });
            if (connected.errorCode()) {
                throw std::system_error(connected.errorCode());
            }
            asio::ssl::stream<asio::ip::tcp::socket&> stream(clientSocket, clientTls);
            const auto handshake = co_await ruvia::asyncAsio([&](auto handler) { stream.async_handshake(asio::ssl::stream_base::client, std::move(handler)); });
            if (handshake.errorCode()) {
                throw std::system_error(handshake.errorCode());
            }
            ruvia::detail::TlsTunnelOutput output(*stream.native_handle(), clientSocket, worker, resource);
            output.start();
            std::exception_ptr exception;
            try {
                const auto bytes = co_await readToEnd(stream);
                RUVIA_CHECK(bytes == outbound);
                co_await writeChunks(stream, output, returned);
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
            ruvia::closeSocket(clientSocket);
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
        (void)watchdog.cancel();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(serverFinished);
    RUVIA_CHECK(received == returned);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
}  // namespace

RUVIA_TEST(tlsTunnelOutputFlushesCloseNotifyAndReadsPeerDataAfterLocalFinish) {
    exerciseHalfClose(ruvia_ctx, TLS1_3_VERSION);
    exerciseHalfClose(ruvia_ctx, TLS1_2_VERSION);
}

namespace {
template <typename Stream>
ruvia::Task<void> serveClientConnect(Stream& stream, ruvia::detail::TlsTunnelOutput* tls, unsigned mode, std::string& received) {
    std::string head;
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::asyncAsio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.errorCode()) {
            throw std::system_error(read.errorCode());
        }
        head.push_back(byte);
    }
    if (!head.starts_with("CONNECT target.test:443 HTTP/1.1\r\n")) {
        throw std::runtime_error("invalid CONNECT authority-form");
    }
    if (head.find("Content-Length:") != std::string::npos || head.find("Transfer-Encoding:") != std::string::npos) {
        throw std::runtime_error("CONNECT head has HTTP content framing");
    }
    ruvia::detail::HttpSocketTunnelTransport transport(stream, tls);
    const auto send = [&](std::string_view bytes, bool end = false) -> ruvia::Task<void> {
        const auto error = co_await transport.writeBytes(bytes, end ? ruvia::detail::HttpStreamEnd::kEnd : ruvia::detail::HttpStreamEnd::kKeepOpen);
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
    received = co_await readToEnd(stream);
    if (mode == 0) {
        co_await send(std::string(100003, 's'), true);
    }
}
void exerciseClientConnect(ruvia::testing::TestContext& ruvia_ctx, bool encrypted, unsigned mode) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    asio::ssl::context tlsContext(asio::ssl::context::tls_server);
    certificate(tlsContext);
    ruvia::test::CountingMemoryResource resource;
    std::string received;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::TaskScope tasks(worker);
        const auto serve = [&]() -> ruvia::Task<void> {
            auto accepted = co_await ruvia::asyncAsio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
            if (accepted.errorCode()) {
                throw std::system_error(accepted.errorCode());
            }
            auto socket = std::move(accepted.result());
            if (encrypted) {
                asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, tlsContext);
                const auto handshake = co_await ruvia::asyncAsio([&](auto h) { stream.async_handshake(asio::ssl::stream_base::server, std::move(h)); });
                if (handshake.errorCode()) {
                    throw std::system_error(handshake.errorCode());
                }
                ruvia::detail::TlsTunnelOutput output(*stream.native_handle(), socket, worker, resource);
                output.start();
                std::exception_ptr error;
                try {
                    co_await serveClientConnect(stream, &output, mode, received);
                } catch (...) {
                    error = std::current_exception();
                    output.abort();
                }
                co_await output.join();
                if (error) {
                    std::rethrow_exception(error);
                }
            } else {
                co_await serveClientConnect(socket, nullptr, mode, received);
            }
        };
        tasks.spawn(serve());
        ruvia::HttpClient client(attachment.loop(), {.scheme = encrypted ? ruvia::HttpScheme::kHttps : ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = acceptor.local_endpoint().port(), .requestTimeout = std::chrono::seconds(5), .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp1Only, .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
        try {
            auto result = co_await client.openTunnel({.authority = "target.test:443"});
            if (mode == 2) {
                RUVIA_CHECK(!result.tunnel() && result.response());
                if (!result.response()) {
                    throw std::runtime_error("missing CONNECT rejection");
                }
                RUVIA_CHECK(result.response()->status().value() == 407);
                std::string text;
                while (auto bytes = co_await result.response()->body().text()) {
                    text.append(*bytes);
                }
                RUVIA_CHECK(text == "denied");
            } else {
                RUVIA_CHECK(result.tunnel() && !result.response());
                if (!result.tunnel()) {
                    throw std::runtime_error("CONNECT rejected");
                }
                auto tunnel = std::move(*result.tunnel());
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                const auto readGreeting = [&]() -> ruvia::Task<void> {
                    std::string greeting;
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's'));
                };
                if (mode == 1) {
                    co_await readGreeting();
                }
                for (unsigned part = 0; part != 8; ++part) {
                    std::string payload(16384, 't');
                    auto write = tunnel.write(std::string_view(payload));
                    payload.assign("mutated");
                    co_await std::move(write);
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                if (mode == 0) {
                    co_await readGreeting();
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
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
}  // namespace
RUVIA_TEST(http1_client_tunnel_and_rejections_preserve_ownership_and_tcp_tls_half_close) {
    for (bool tls : {false, true}) {
        for (unsigned mode = 0; mode != 3; ++mode) {
            exerciseClientConnect(ruvia_ctx, tls, mode);
        }
    }
}
