#include "server/worker_connections.h"

#include <cstdint>
#include <memory_resource>
#include <system_error>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/Async.h"
#include "ruvia/core/Socket.h"
#include "ruvia/http/HttpAscii.h"

#include "context/ContextServices.h"
#include "http2/CleartextUpgrade.h"
#include "http2/Http2SansIoSession.h"
#include "integration/WorkerCapabilities.h"
#include "server/HttpServerAlpn.h"
#include "server/HttpServerConnectionGuards.h"
#include "server/HttpServerStreamSession.h"
#include "server/HttpServerTlsHandshake.h"
#include "server/HttpServerTlsIdentity.h"

namespace ruvia::detail {
namespace {

int selectAlpnProtocol(SSL*, const unsigned char** out, unsigned char* outLength,
    const unsigned char* in, unsigned int inLength, void*) noexcept {
    // This is the TCP TLS context, so it offers only h2 and http/1.1. HTTP/3
    // negotiates "h3" on this worker's separate QUIC/TLS context and is never
    // advertised through this TCP callback.
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLength, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, inLength) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

// RFC 6066 SNI: switch the connection to the per-host SSL_CTX when the client's
// server name matches a configured certificate; otherwise keep the default.
int selectSniContext(SSL* ssl, int*, void* arg) noexcept {
    if (ssl == nullptr || arg == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (name == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const auto& lookup = *static_cast<const SniContextLookup*>(arg);
    for (const auto& [host, context] : lookup) {
        if (httpAsciiEqualsIgnoreCase(host, name)) {
            SSL_set_SSL_CTX(ssl, context->native_handle());
            break;
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

[[nodiscard]] ruvia::ConnectionScannerOptions makeConnectionScannerOptions(
    const HttpServerOptions& options) noexcept {
    return ruvia::ConnectionScannerOptions{.scanInterval = options.scanInterval,
        .idle_timeout = options.idle_timeout,
        .initialReadTimeout = options.request_header_timeout,
        .payloadReadTimeout = options.request_body_timeout,
        .write_timeout = options.write_timeout,
        .initial_read_completion_timeout = options.header_completion_timeout,
        .payload_read_completion_timeout = options.body_completion_timeout};
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const WorkerHandle& worker,
    WorkerMemory& memory, const RouteTable& routes, WorkerCapabilities& capabilities,
    HttpServerOptions& options, const StopToken& stop_token, HttpServerWorkerState& state,
    TaskScope& tasks, std::span<const HttpServerListenerDefinition> listeners)
    : io_(io),
      worker_(worker),
      memory_(memory),
      routes_(routes),
      capabilities_(capabilities),
      options_(options),
      stop_token_(stop_token),
      state_(state),
      tasks_(tasks),
      listeners_(memory.resource()),
      scanner_(worker, makeConnectionScannerOptions(options)),
      work_sets_(memory) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(makePmrObject<HttpServerSessionConfig>(memory.resource(), listener, memory.resource()));
    }
    options_.connectionFailure.counter = &failures_;
}
void worker_connections::prepare() {
    for (auto& listener : listeners_) {
        configure_tls(*listener);
    }
    scanner_.start();
}
void worker_connections::open_admission() noexcept {
    serving_.store(true, std::memory_order_release);
}
void worker_connections::stop() noexcept {
    serving_.store(false, std::memory_order_release);
    scanner_.stop();
    scanner_.closeAll();
}
void worker_connections::retire_tls() noexcept {
    for (auto& listener : listeners_) {
        listener->tlsContext.reset();
        listener->sniLookup.clear();
        listener->sniContexts.clear();
    }
}
bool worker_connections::available() const noexcept {
    return serving_.load(std::memory_order_acquire) &&
           (!options_.maxConnections || active_.load(std::memory_order_relaxed) < *options_.maxConnections);
}
HttpServerStats worker_connections::stats() const noexcept {
    HttpServerStats result;
    result.activeConnections = active_.load(std::memory_order_relaxed);
    result.connectionsRefused = refused_.load(std::memory_order_relaxed);
    result.connectionFailures = failures_.load(std::memory_order_relaxed);
    result.acceptFailures = accept_failures_.load(std::memory_order_relaxed);
    return result;
}

void worker_connections::configure_tls(HttpServerSessionConfig& listener) {
    listener.sniContexts.clear();
    listener.sniLookup.clear();
    const auto* tls = listener.tls();
    if (tls == nullptr) {
        listener.tlsContext.reset();
        return;
    }
    const auto configure = [tls](asio::ssl::context& context,
                               const HttpServerListenerDefinition::TlsIdentity& identity) {
        context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_alpn_select_cb(context.native_handle(), selectAlpnProtocol, nullptr);
        configureHttpServerTlsIdentity(
            context.native_handle(), identity, tls->clientCertificates);
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    listener.sniContexts.reserve(tls->sniIdentities.size());
    for (const auto& sni : tls->sniIdentities) {
        auto& context = listener.sniContexts.emplace_back(asio::ssl::context::tls_server);
        configure(context, sni.identity);
    }
    listener.sniLookup.reserve(tls->sniIdentities.size());
    for (std::size_t i = 0; i < tls->sniIdentities.size(); ++i) {
        listener.sniLookup.emplace_back(tls->sniIdentities[i].host, &listener.sniContexts[i]);
    }

    listener.tlsContext.emplace(asio::ssl::context::tls_server);
    auto& context = *listener.tlsContext;
    configure(context, tls->identity);
    if (!listener.sniLookup.empty()) {
        SSL_CTX_set_tlsext_servername_callback(context.native_handle(), &selectSniContext);
        SSL_CTX_set_tlsext_servername_arg(context.native_handle(), &listener.sniLookup);
    }
}

Task<void> worker_connections::run_session(
    HttpServerSessionConfig& listener, AcceptedConnectionLease connection) {
    auto& socket = connection.socket();
    // Declared outside the try so the failure report below can name the peer.
    // It stays empty if the failure happened before the address was resolved.
    std::pmr::string remoteAddress(memory_.allocator<char>());
    std::uint16_t remotePort = 0;
    try {
        std::error_code remoteEc;
        const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
        if (!remoteEc) {
            ruvia::assignRemoteAddress(remoteAddress, remoteEndpoint.address());
            remotePort = remoteEndpoint.port();
        }
        ContextServices baseServices = capabilities_.contextServices(stop_token_);
        if (listener.tls() != nullptr) {
            asio::ssl::stream<TcpSocket&> tlsStream(socket, *listener.tlsContext);
            {
                // The TLS handshake has its own initial-read deadline. It must be
                // released the moment the handshake resolves and before the
                // session is dispatched: the session installs and continuously
                // refreshes its own scanner entry, but this handshake entry stays
                // pinned at kReadingInitial with a frozen last-active time. Left
                // registered across the session, the scanner would close an active
                // connection's socket one request_header_timeout after the handshake
                // regardless of session activity -- severing long-lived TLS
                // sessions (WebSocket, keep-alive, slow uploads, streaming).
                ruvia::ConnectionScanner::Entry handshakeEntry;
                ruvia::ConnectionScanner::Guard handshakeGuard(
                    &scanner_, handshakeEntry, socket);
                handshakeEntry.setPhase(ruvia::ConnectionScanner::Phase::kReadingInitial);
                const auto handshakeCompletion =
                    co_await ruvia::asyncAsio(TlsServerHandshakeInitiator{&tlsStream});
                if (handshakeCompletion.errorCode()) {
                    ruvia::closeSocket(socket);
                    co_return;
                }
            }
            std::pmr::string clientCertificate(memory_.allocator<char>());
            extractTlsClientCertificate(tlsStream.native_handle(), clientCertificate);
            const auto tlsServices = baseServices
                                         .withTlsTransport(
                                             remoteAddress, clientCertificate, remotePort)
                                         .withAutomaticAltSvc(listener.tls()->altSvc);
            if (isHttp2AlpnSelected(tlsStream)) {
                co_await run_http2(tlsStream, socket, tlsServices);
            } else {
                co_await run_stream(listener, tlsStream, socket, tlsServices);
            }
            ruvia::closeSocket(socket);
            co_return;
        }
        co_await run_stream(
            listener, socket, socket, baseServices.withPlainTransport(remoteAddress, remotePort));
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
        ruvia::closeSocket(socket);
        options_.connectionFailure.invoke(remoteAddress, failure);
    }
}

template <typename Stream>
Task<void> worker_connections::run_http2(
    Stream& stream, TcpSocket& socket, ContextServices services, std::string_view initialBytes) {
    ruvia::ConnectionScanner::Entry scannerEntry;
    ruvia::ConnectionScanner::Guard scannerGuard(&scanner_, scannerEntry, socket);

    co_await runHttp2ServerSession(
        Http2ServerSessionSetup<Stream>{
            .stream = stream,
            .socket = socket,
            .memory = memory_,
            .routes = routes_,
            .options = options_,
            .scannerEntry = scannerEntry,
            .services = services,
            .workerState = state_,
        },
        initialBytes);
}

void worker_connections::accept_socket(std::size_t listenerIndex, TcpSocket socket) {
    if (!httpServerWorkerRunning(state_)) {
        return;
    }
    if (listenerIndex >= listeners_.size()) {
        return;
    }
    if (options_.maxConnections.has_value() &&
        active_.load(std::memory_order_relaxed) >= *options_.maxConnections) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        ruvia::configureAcceptedSocket(socket);
        AcceptedConnectionLease connection(std::move(socket), active_);
        tasks_.spawn(run_session(*listeners_[listenerIndex], std::move(connection)));
    } catch (...) {
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.connectionFailure.invoke({}, std::current_exception());
    }
}

void worker_connections::accept(NativeAcceptedSocketTicket&& ticket) noexcept {
    if (!ticket.valid()) {
        return;
    }
    const auto listenerIndex = ticket.listenerIndex();
    if (!httpServerWorkerRunning(state_) || listenerIndex >= listeners_.size()) {
        return;
    }

    try {
        TcpSocket socket(io_);
        asio::error_code error;
        try {
            socket.assign(ticket.protocol(), ticket.nativeHandle(), error);
        } catch (...) {
            // Some Asio implementations can take the handle before reporting an
            // exception. Disarm the ticket before socket's RAII cleanup in that case.
            if (socket.is_open() && socket.native_handle() == ticket.nativeHandle()) {
                static_cast<void>(ticket.release());
            }
            accept_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (error) {
            if (socket.is_open() && socket.native_handle() == ticket.nativeHandle()) {
                static_cast<void>(ticket.release());
            }
            accept_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        static_cast<void>(ticket.release());  // ownership now belongs to socket.
        accept_socket(listenerIndex, std::move(socket));
    } catch (...) {
        // Includes TcpSocket construction and any unexpected accept-path failure.
        // Until assign transfers ownership the ticket remains responsible for close.
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace ruvia::detail
