#pragma once

#include <memory_resource>
#include <system_error>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/server/tls/HttpServerAlpn.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/http2/CleartextUpgrade.h"
#include "ruvia/web/detail/server/session/HttpServerStreamSession.h"
#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/server/tls/HttpServerTlsHandshake.h"

// Member-template definitions for HttpServer, kept out of its header so the
// class stays readable. Included as an ordinary header: everything used here is
// included here.

namespace ruvia::detail {

// Defined inline: this is a header, and a header may be included by more than
// one translation unit. The member templates below need no such marking.
inline Task<void> HttpServer::handleSession(AcceptedConnectionLease connection) {
    auto& socket = connection.socket();
    // Declared outside the try so the failure report below can name the peer.
    // It stays empty if the failure happened before the address was resolved.
    std::pmr::string remoteAddress(memory_.allocator<char>());
    try {
        std::error_code remoteEc;
        const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
        if (!remoteEc) {
            assignRemoteAddress(remoteAddress, remoteEndpoint.address());
        }
        ContextServices baseServices = ContextServices(&dataAccess_.databases(), &dataAccess_.redis(), &rateLimiter_, options_.maxBufferedBodyBytes, &workerHandle_, &httpClients_)
            .withStopToken(stopToken_)
            .withWorkerStates(workerStates_)
            .withBlockingPool(options_.blockingPool)
            .withPrecompressedStaticFiles(options_.compression.has_value())
            .withTrustedProxies(options_.trustedProxies.empty() ? nullptr : &options_.trustedProxies);
        if (options_.env != nullptr) {
            baseServices = baseServices.withEnv(*options_.env);
        }
        if (options_.tls() != nullptr) {
            asio::ssl::stream<TcpSocket&> tlsStream(socket, *tlsContext_);
            {
                // The TLS handshake has its own initial-read deadline. It must be
                // released the moment the handshake resolves and before the
                // session is dispatched: the session installs and continuously
                // refreshes its own scanner entry, but this handshake entry stays
                // pinned at kReadingInitial with a frozen last-active time. Left
                // registered across the session, the scanner would close an active
                // connection's socket one requestHeaderTimeout after the handshake
                // regardless of session activity -- severing long-lived TLS
                // sessions (WebSocket, keep-alive, slow uploads, streaming).
                ConnectionScanner::Entry handshakeEntry;
                ConnectionScanner::Guard handshakeGuard(&connectionScanner_, handshakeEntry, socket);
                handshakeEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
                const auto handshakeCompletion = co_await asyncAsio(TlsServerHandshakeInitiator{&tlsStream});
                if (handshakeCompletion.errorCode()) {
                    closeSocket(socket);
                    co_return;
                }
            }
            std::pmr::string clientCertificate(memory_.allocator<char>());
            extractTlsClientCertificate(tlsStream.native_handle(), clientCertificate);
            const auto tlsServices = baseServices.withTlsTransport(remoteAddress, clientCertificate);
            if (isHttp2AlpnSelected(tlsStream)) {
                co_await handleHttp2Session(tlsStream, socket, tlsServices);
            } else {
                co_await handleStreamSession(tlsStream, socket, tlsServices);
            }
            closeSocket(socket);
            co_return;
        }
        co_await handleStreamSession(socket, socket, baseServices.withPlainTransport(remoteAddress));
    } catch (...) {
        // Last-resort safety net: any exception that escapes the session
        // body (including bad_alloc, error-handler failures, or framework
        // bugs) must not propagate into asio::detached, which terminates.
        // Socket state may be partially written or completely fine; we
        // cannot safely emit anything new, so just drop the connection.
        //
        // Dropping the connection must not also drop the reason: this is the
        // only place that reason exists, so it goes to the connection-failure
        // sink before the frame unwinds.
        const auto failure = std::current_exception();
        closeSocket(socket);
        options_.connectionFailure.invoke(remoteAddress, failure);
    }
}

template <typename Stream>
Task<void> HttpServer::handleHttp2Session(Stream& stream, TcpSocket& socket, ContextServices services, std::string_view initialBytes) {
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);

    co_await runHttp2ServerSession(
        Http2ServerSessionSetup<Stream>{
            .stream = stream,
            .socket = socket,
            .memory = memory_,
            .routes = routes_,
            .options = options_,
            .scannerEntry = scannerEntry,
            .services = services,
            .workerState = workerState_,
        },
        initialBytes);
}

}  // namespace ruvia::detail
