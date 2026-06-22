#include "HttpServer.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
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

#include "ConnectionScanner.h"
#include "HttpConnectionState.h"
#include "HttpServerOptionsValidation.h"
#include "../../runtime/AsioAwait.h"

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
        .idleTimeoutMs = options.idleTimeout.count(),
        .headerTimeoutMs = options.headerTimeout.count(),
        .bodyTimeoutMs = options.bodyTimeout.count(),
        .writeTimeoutMs = options.writeTimeout.count()};
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
          std::span<const HttpClientDefinition>{},
          std::move(options)) {}

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    HttpServerOptions options)
    : HttpServer(
          std::move(endpoint), routes, databases, redis,
          std::span<const HttpClientDefinition>{},
          std::move(options)) {}

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    std::span<const HttpClientDefinition> httpClients,
    HttpServerOptions options)
    // One worker thread runs all I/O on this context; cross-thread access is
    // limited to stop()'s asio::post, which UNSAFE_IO keeps locked. Only the
    // reactor's per-descriptor I/O locking is elided.
    : ioContext_(ASIO_CONCURRENCY_HINT_UNSAFE_IO),
      acceptor_(ioContext_),
      endpoint_(std::move(endpoint)),
      routes_(routes),
      options_(validatedHttpServerOptions(std::move(options))),
      databases_(ioContext_, memory_.resource(), databases),
      redis_(ioContext_, memory_.resource(), redis),
      httpClients_(ioContext_, memory_.resource(), httpClients),
      connectionScanner_(ioContext_.get_executor(), makeConnectionScannerOptions(options_)),
      workSetPool_(memory_) {
    if (databases_.hasAnyTimeout()) {
        connectionScanner_.setWorkerScanner(&databases_, [](void* target) noexcept {
            static_cast<DbRegistry*>(target)->scanDeadlines();
        });
    }
    if (redis_.hasAnyTimeout()) {
        connectionScanner_.setWorkerScanner(&redis_, [](void* target) noexcept {
            static_cast<RedisRegistry*>(target)->scanDeadlines();
        });
    }
    if (!httpClients_.empty()) {
        connectionScanner_.setWorkerScanner(&httpClients_, [](void* target) noexcept {
            static_cast<HttpClientRegistry*>(target)->scanDeadlines();
        });
    }
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("http server worker is already started");
    }

    try {
        resetStartupState();
        configureAcceptor();
        configureTlsContext();
        connectionScanner_.start();
        asio::co_spawn(
            ioContext_,
            taskAsAwaitable(runWorker()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
        workerThread_ = std::jthread([this] { runIoContext(); });
        waitForStartupReady();
    } catch (...) {
        started_ = false;
        if (workerThread_.joinable()) {
            workerThread_.join();
        } else {
            stopOnContext();
        }
        throw;
    }
}

void HttpServer::stop() {
    if (!started_.exchange(false)) {
        return;
    }

    asio::post(ioContext_, [this] { stopOnContext(); });
}

void HttpServer::join() {
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

TcpEndpoint HttpServer::localEndpoint() const {
    return endpoint_;
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
    if (!options_.tls.enabled) {
        tlsContext_.reset();
        return;
    }
    if (options_.tls.certificateChainFile.empty() || options_.tls.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS requires certificate chain and private key files");
    }

    tlsContext_.emplace(asio::ssl::context::tls_server);
    auto& context = *tlsContext_;
    context.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 |
        asio::ssl::context::no_tlsv1 |
        asio::ssl::context::no_tlsv1_1 |
        asio::ssl::context::single_dh_use);
    SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_ENABLE_KTLS
    // Offload symmetric record encryption/decryption to the kernel (OpenSSL 3 +
    // Linux). This removes the userspace encrypt/decrypt staging copy on the
    // normal data path and lets the file body be served with sendfile() over TLS
    // (the kernel adds the TLS records). Where the kernel/NIC/cipher does not
    // support kTLS, OpenSSL silently falls back to userspace crypto.
    SSL_CTX_set_options(context.native_handle(), SSL_OP_ENABLE_KTLS);
#endif
    SSL_CTX_set_alpn_select_cb(context.native_handle(), selectAlpnProtocol, nullptr);

    if (!options_.tls.privateKeyPassword.empty()) {
        SSL_CTX_set_default_passwd_cb(context.native_handle(), copyPrivateKeyPassword);
        SSL_CTX_set_default_passwd_cb_userdata(context.native_handle(), &options_.tls.privateKeyPassword);
    }
    useCertificateChainFile(context, options_.tls.certificateChainFile);
    usePrivateKeyFile(context, options_.tls.privateKeyFile);
    if (!options_.tls.verifyFile.empty()) {
        loadVerifyFile(context, options_.tls.verifyFile);
        context.set_verify_mode(asio::ssl::verify_peer);
    }
}

void HttpServer::stopOnContext() noexcept {
    std::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);

    connectionScanner_.stop();
    connectionScanner_.closeAll();

    databases_.closeNow();
    redis_.closeNow();
    httpClients_.closeNow();
}

void HttpServer::resetStartupState() {
    std::lock_guard lock(startupMutex_);
    startupException_ = nullptr;
    startupReady_ = false;
}

void HttpServer::completeStartup(std::exception_ptr exception) noexcept {
    {
        std::lock_guard lock(startupMutex_);
        if (startupReady_) {
            return;
        }

        startupException_ = exception;
        startupReady_ = true;
    }
    startupCv_.notify_all();
}

void HttpServer::waitForStartupReady() {
    std::unique_lock lock(startupMutex_);
    startupCv_.wait(lock, [this] { return startupReady_; });
    if (startupException_ != nullptr) {
        std::rethrow_exception(startupException_);
    }
}

void HttpServer::runIoContext() noexcept {
    try {
        ioContext_.run();
    } catch (...) {
        started_.store(false, std::memory_order_relaxed);
        stopOnContext();
        completeStartup(std::current_exception());
        return;
    }

    completeStartup(std::make_exception_ptr(
        std::runtime_error("http server worker stopped before startup completed")));
}

Task<void> HttpServer::runWorker() {
    try {
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
        if (!httpClients_.empty()) {
            co_await httpClients_.connect();
        }
        completeStartup();
        co_await acceptLoop();
    } catch (...) {
        started_.store(false, std::memory_order_relaxed);
        stopOnContext();
        completeStartup(std::current_exception());
    }
}

}  // namespace ruvia::detail
