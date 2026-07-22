#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"

#include "ruvia/web/detail/server/HttpServerTlsVerify.h"

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/system_error.hpp>
#include <cerrno>
#include <cstring>
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;

namespace {

int selectAlpnProtocol(
    SSL*,
    const unsigned char** out,
    unsigned char* outLength,
    const unsigned char* in,
    unsigned int inLength,
    void*) noexcept {
    // Only h2 and http/1.1 are offered. HTTP/3 / QUIC is explicitly not supported (no "h3"
    // token, no UDP/QUIC listener); a peer offering only h3 falls back to http/1.1 or fails ALPN.
    static constexpr unsigned char protocols[] = {
        2, 'h', '2',
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_select_next_proto(
            const_cast<unsigned char**>(out),
            outLength,
            protocols,
            static_cast<unsigned int>(sizeof(protocols)),
            in,
            inLength) == OPENSSL_NPN_NEGOTIATED) {
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

int copyPrivateKeyPassword(char* buffer, int bufferSize, int, void* userData) noexcept {
    if (buffer == nullptr || bufferSize <= 0 || userData == nullptr) {
        return 0;
    }

    const auto& password = *static_cast<const std::pmr::string*>(userData);
    const auto capacity = static_cast<std::size_t>(bufferSize);
    if (password.size() >= capacity) {
        return 0;
    }

    std::memcpy(buffer, password.data(), password.size());
    buffer[password.size()] = '\0';
    return static_cast<int>(password.size());
}

[[nodiscard]] asio::error_code translateOpenSslError(unsigned long error) {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(
            static_cast<int>(ERR_GET_REASON(error)),
            asio::error::get_system_category());
    }
#endif
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throwTlsContextFileError(const char* operation) {
    throw asio::system_error(translateOpenSslError(::ERR_get_error()), operation);
}

void useCertificateChainFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate_chain_file(context.native_handle(), filename.c_str()) != 1) {
        throwTlsContextFileError("use_certificate_chain_file");
    }
}

void usePrivateKeyFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey_file(context.native_handle(), filename.c_str(), SSL_FILETYPE_PEM) != 1) {
        throwTlsContextFileError("use_private_key_file");
    }
}

void loadVerifyFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_load_verify_locations(context.native_handle(), filename.c_str(), nullptr) != 1) {
        throwTlsContextFileError("load_verify_file");
    }
}

[[nodiscard]] ConnectionScannerOptions makeConnectionScannerOptions(const HttpServerOptions& options) noexcept {
    return ConnectionScannerOptions{
        .scanInterval = options.scanInterval,
        .idleTimeout = options.keepaliveTimeout,
        .initialReadTimeout = options.clientHeaderTimeout,
        .payloadReadTimeout = options.clientBodyTimeout,
        .writeTimeout = options.sendTimeout};
}

}  // namespace

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    HttpServerOptions options)
    : HttpServer(
          std::move(endpoint), routes, databases,
          std::span<const RedisDefinition>{},
          std::move(options)) {}

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    HttpServerOptions options)
    : HttpServer(
          std::move(endpoint), routes, databases, redis,
          std::span<const WorkerStateDefinition>{},
          std::move(options)) {}

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    std::span<const WorkerStateDefinition> workerStates,
    HttpServerOptions options)
    : HttpServer(
          ValidatedOptionsTag{},
          std::move(endpoint),
          routes,
          databases,
          redis,
          workerStates,
          validatedHttpServerOptions(std::move(options))) {}

HttpServer::HttpServer(
    ValidatedOptionsTag,
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    std::span<const WorkerStateDefinition> workerStates,
    HttpServerOptions validatedOptions)
    // One worker thread runs all I/O on this context; cross-thread access is
    // limited to stop()'s asio::post, which UNSAFE_IO keeps locked. Only the
    // reactor's per-descriptor I/O locking is elided.
    : ioContext_(ASIO_CONCURRENCY_HINT_UNSAFE_IO),
      workerDispatcher_(std::make_shared<WorkerDispatcher>(
          ioContext_, validatedOptions.workerMailboxCapacity)),
      workerHandle_(WorkerHandleAccess::make(workerDispatcher_)),
      acceptor_(ioContext_),
      endpoint_(std::move(endpoint)),
      routes_(routes),
      memory_(validatedOptions.memoryConfig),
      sniContexts_(memory_.resource()),
      sniLookup_(memory_.resource()),
      options_(std::move(validatedOptions)),
      connectionScanner_(workerHandle_, makeConnectionScannerOptions(options_)),
      dataAccess_(
          ioContext_,
          memory_.resource(),
          databases,
          redis,
          connectionScanner_),
      workerStates_(memory_.resource(), workerStates),
      webWorkerDispatch_(std::make_shared<WebWorkerDispatch>(
          ioContext_.get_executor(),
          workerHandle_,
          memory_.resource(),
          dataAccess_.databases(),
          dataAccess_.redis(),
          workerStates_,
          [this](std::exception_ptr failure) {
              failWorker(std::move(failure));
          })),
      rateLimiter_(
          options_.defaultRateLimitPerWorker,
          routes_.hasRouteRateLimit()
              ? RouteRateLimitPresence::kPresent
              : RouteRateLimitPresence::kAbsent,
          options_.rateLimitSlotsPerWorker,
          memory_.resource()),
      workSetPool_(memory_) {}

HttpServer::~HttpServer() {
    stop();
    try {
        join();
    } catch (...) {
    }
    // Retire the execution context first. Failure shutdown already releases
    // abandoned mailbox tasks on the worker; detach defensively releases any
    // task left by a context that stopped outside the managed run loop. Each
    // dropped WebWorker task reconciles its outstanding_ reservation before
    // retire() checks it. Public handles may outlive this server, so detach also
    // leaves them a terminal endpoint before Asio objects are destroyed.
    workerDispatcher_->detachContext();
    webWorkerDispatch_->retire();
}

void HttpServer::start() {
    if (!lifecycle_.start()) {
        throw std::logic_error("http server worker cannot be restarted");
    }
    workerState_ = HttpServerWorkerState::kRunning;

    try {
        configureAcceptor();
        configureTlsContext();
        workerThread_ = std::thread([this] { runIoContext(); });
        workerCompletion_.waitForStartup();
    } catch (...) {
        (void)lifecycle_.requestStop();
        if (workerThread_.joinable()) {
            workerThread_.join();
        } else {
            stopOnContext();
        }
        lifecycle_.completeStop();
        throw;
    }
}

void HttpServer::stop() {
    if (!lifecycle_.requestStop()) {
        return;
    }

    webWorkerDispatch_->close();
    workerDispatcher_->close();
    asio::post(ioContext_, [this] { stopOnContext(); });
}

void HttpServer::join() {
    if (workerHandle_.isCurrent()) {
        throw std::logic_error(
            "cannot join an HTTP server from its worker");
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    lifecycle_.completeStop();
    const auto failure = workerCompletion_.workerFailure();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

TcpEndpoint HttpServer::localEndpoint() const {
    return endpoint_;
}

WebWorkerHandle HttpServer::webWorker() const {
    return webWorkerDispatch_->handle();
}
void HttpServer::configureAcceptor() {
    std::error_code ec;

    acceptor_.open(endpoint_.protocol(), ec);
    if (ec) {
        throw std::runtime_error("failed to open acceptor: " + ec.message());
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("failed to enable SO_REUSEADDR: " + ec.message());
    }

#if defined(SO_REUSEPORT) && !defined(_WIN32)
    int enabled = 1;
    if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to enable SO_REUSEPORT");
    }
#elif !defined(_WIN32)
    throw std::runtime_error("SO_REUSEPORT is required but not available on this platform/toolchain");
#endif

    acceptor_.bind(endpoint_, ec);
    if (ec) {
        throw std::runtime_error("failed to bind acceptor: " + ec.message());
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("failed to listen: " + ec.message());
    }

    endpoint_ = acceptor_.local_endpoint(ec);
    if (ec) {
        throw std::runtime_error("failed to read local endpoint: " + ec.message());
    }
}

void HttpServer::configureTlsContext() {
    sniContexts_.clear();
    sniLookup_.clear();
    const auto* tls = options_.tls();
    if (tls == nullptr) {
        tlsContext_.reset();
        return;
    }
    if (tls->identity.certificateChainFile.empty() ||
        tls->identity.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS requires certificate chain and private key files");
    }

    const auto configure = [tls](
                               asio::ssl::context& context,
                               const std::pmr::string& certificateChainFile,
                               const std::pmr::string& privateKeyFile,
                               const std::pmr::string& privateKeyPassword) {
        context.set_options(
            asio::ssl::context::default_workarounds |
            asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 |
            asio::ssl::context::no_tlsv1 |
            asio::ssl::context::no_tlsv1_1 |
            asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_alpn_select_cb(context.native_handle(), selectAlpnProtocol, nullptr);
        if (!privateKeyPassword.empty()) {
            SSL_CTX_set_default_passwd_cb(context.native_handle(), copyPrivateKeyPassword);
            SSL_CTX_set_default_passwd_cb_userdata(
                context.native_handle(), const_cast<std::pmr::string*>(&privateKeyPassword));
        }
        useCertificateChainFile(context, certificateChainFile);
        usePrivateKeyFile(context, privateKeyFile);
        if (tls->clientCertificates.has_value()) {
            loadVerifyFile(context, tls->clientCertificates->verifyFile);
            context.set_verify_mode(
                httpServerTlsVerifyMode(tls->clientCertificates->requirement));
        }
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    sniContexts_.reserve(tls->sniIdentities.size());
    for (const auto& sni : tls->sniIdentities) {
        auto& context = sniContexts_.emplace_back(asio::ssl::context::tls_server);
        configure(
            context,
            sni.identity.certificateChainFile,
            sni.identity.privateKeyFile,
            sni.identity.privateKeyPassword);
    }
    sniLookup_.reserve(tls->sniIdentities.size());
    for (std::size_t i = 0; i < tls->sniIdentities.size(); ++i) {
        sniLookup_.emplace_back(tls->sniIdentities[i].host, &sniContexts_[i]);
    }

    tlsContext_.emplace(asio::ssl::context::tls_server);
    auto& context = *tlsContext_;
    configure(
        context,
        tls->identity.certificateChainFile,
        tls->identity.privateKeyFile,
        tls->identity.privateKeyPassword);
    if (!sniLookup_.empty()) {
        SSL_CTX_set_tlsext_servername_callback(context.native_handle(), &selectSniContext);
        SSL_CTX_set_tlsext_servername_arg(context.native_handle(), &sniLookup_);
    }
}

void HttpServer::stopOnContext() noexcept {
    if (!httpServerWorkerRunning(workerState_)) {
        return;
    }

    workerState_ = HttpServerWorkerState::kStopped;
    webWorkerDispatch_->close();
    workerDispatcher_->close();
    std::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    connectionScanner_.stop();
    connectionScanner_.closeAll();
    dataAccess_.closeNow();
    workerDispatcher_->stopTimers();
}

void HttpServer::failWorker(std::exception_ptr failure) noexcept {
    if (!workerCompletion_.recordWorkerFailure(failure)) {
        return;
    }
    (void)lifecycle_.requestStop();
    options_.workerFailure.notify(failure);
    stopOnContext();
}

void HttpServer::runIoContext() noexcept {
    bool workerFailed = false;
    try {
        workerDispatcher_->runContext(
            [this] {
                workerStates_.initialize();
                asio::co_spawn(
                    ioContext_,
                    taskAsAwaitable(runWorker()),
                    asio::bind_allocator(
                        asio::recycling_allocator<void>(), asio::detached));
            },
            [this, &workerFailed](std::exception_ptr failure) noexcept {
                workerFailed = true;
                (void)workerCompletion_.markStartupFailed(failure);
                failWorker(std::move(failure));
                lifecycle_.completeStop();
                workerState_ = HttpServerWorkerState::kStopped;
            },
            [this]() noexcept { workerStates_.shutdown(); });
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
        lifecycle_.completeStop();
        workerState_ = HttpServerWorkerState::kStopped;
        return;
    }
    if (workerFailed) {
        return;
    }

    lifecycle_.completeStop();
    workerState_ = HttpServerWorkerState::kStopped;
    (void)workerCompletion_.markStartupFailed(std::make_exception_ptr(
        std::runtime_error("http server worker stopped before startup completed")));
}

Task<void> HttpServer::runWorker() {
    if (!httpServerWorkerRunning(workerState_)) {
        co_return;
    }
    try {
        connectionScanner_.start();
        co_await dataAccess_.connect();
        (void)workerCompletion_.markStartupReady();
        co_await acceptLoop();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
    }
}

}  // namespace ruvia::detail
