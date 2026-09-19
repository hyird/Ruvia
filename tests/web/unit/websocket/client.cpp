#include <array>
#include <exception>
#include <memory>
#include <string>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/web/WebSocketClient.h"

#include "test_harness.h"

namespace {

void checkExchangeAndClose(ruvia::testing::TestContext& ruvia_ctx, bool replyClose) {
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peerFailure;
    bool sawClose = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const std::string_view keyHeader = "Sec-WebSocket-Key: ";
        const auto keyBegin = request.find(keyHeader);
        if (keyBegin == std::string::npos) {
            throw std::runtime_error("missing WebSocket key");
        }
        const auto start = keyBegin + keyHeader.size();
        ruvia::detail::WebSocketAcceptKey accept{};
        ruvia::detail::encodeWebSocketAccept(accept, std::string_view(request).substr(start, request.find("\r\n", start) - start));
        // The first frame can arrive in the same transport read as the upgrade.
        const auto response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ") +
                              std::string(accept.data(), accept.size()) + "\r\n\r\n" + "\x82\x05hello";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::use_awaitable);
        sawClose = header[0] == 0x88 && (header[1] & 0x80) != 0;
        std::array<unsigned char, 129> payload{};
        co_await asio::async_read(socket, asio::buffer(payload.data(), 4 + (header[1] & 0x7f)), asio::use_awaitable);
        if (replyClose) {
            const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
            co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peerFailure = failure; });
    bool succeeded = false;
    bool protocolError = false;
    const auto runClient = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.scheme = ruvia::WebSocketScheme::kWs,
                                                             .host = "127.0.0.1",
                                                             .port = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            const auto greeting = co_await client.read();
            RUVIA_CHECK(greeting.has_value());
            if (greeting) {
                RUVIA_CHECK_EQ(greeting->payload(), std::string_view("hello"));
            }
            co_await client.close({});
            succeeded = true;
        } catch (const ruvia::WebSocketClientError& error) {
            protocolError = error.code() == ruvia::WebSocketClientError::Code::kProtocolError;
            if (!protocolError) {
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
    auto root = attachment.loop().start(runClient());
    attachment.run();
    root.get();
    if (peerFailure) {
        std::rethrow_exception(peerFailure);
    }
    RUVIA_CHECK(sawClose);
    RUVIA_CHECK_EQ(succeeded, replyClose);
    RUVIA_CHECK_EQ(protocolError, !replyClose);
}

}  // namespace

RUVIA_TEST(websocket_client_close_requires_peer_close) {
    checkExchangeAndClose(ruvia_ctx, false);
    checkExchangeAndClose(ruvia_ctx, true);
}

RUVIA_TEST(websocket_client_rejects_untrusted_tls_peer) {
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const auto require = [](bool success) {
        if (!success) {
            throw std::runtime_error("could not generate test certificate");
        }
    };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    require(generator != nullptr);
    require(EVP_PKEY_keygen_init(generator.get()) == 1);
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* rawKey = nullptr;
    require(EVP_PKEY_keygen(generator.get(), &rawKey) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    require(certificate != nullptr);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    auto* name = X509_get_subject_name(certificate.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_issuer_name(certificate.get(), name) == 1);
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    require(SSL_CTX_use_certificate(tls.native_handle(), certificate.get()) == 1);
    require(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()) == 1);

    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool handshakeRejected = false;
    std::exception_ptr peerFailure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, error));
        handshakeRejected = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peerFailure = failure; });
    bool tlsFailure = false;
    const auto runClient = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.host = "127.0.0.1",
                                                             .port = peer.local_endpoint().port()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
        } catch (const ruvia::WebSocketClientError& error) {
            tlsFailure = error.code() == ruvia::WebSocketClientError::Code::kTlsFailed;
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
    auto root = attachment.loop().start(runClient());
    attachment.run();
    root.get();
    if (peerFailure) {
        std::rethrow_exception(peerFailure);
    }
    RUVIA_CHECK(tlsFailure);
    RUVIA_CHECK(handshakeRejected);
}
