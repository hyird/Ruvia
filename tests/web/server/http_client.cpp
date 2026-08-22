#include <array>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/web/detail/util/CallableRef.h"

namespace {

using namespace std::chrono_literals;

struct SelfSignedPem { std::string cert; std::string key; };

void reportStage(const char* stage) noexcept {
    std::fprintf(stderr, "[http-client-stage] %s\n", stage);
    std::fflush(stderr);
}

class OneShotFailingResource final : public std::pmr::memory_resource {
public:
    void failNextAllocationAtLeast(std::size_t bytes) noexcept {
        minimumFailureBytes_ = bytes;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (minimumFailureBytes_ != 0 && bytes >= minimumFailureBytes_) {
            minimumFailureBytes_ = 0;
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(
        void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t minimumFailureBytes_{0};
};

std::string gzipContent(std::string_view body) {
    auto encoded = ruvia::encodeHttpContent(
        ruvia::HttpContentCoding::kGzip, body,
        {.maxEncodedBytes = body.size() + 1024, .resource = std::pmr::get_default_resource()});
    if (!encoded.encoded()) throw std::runtime_error("failed to encode test gzip body");
    const auto bytes = encoded.encoded()->bytes();
    return {bytes.data(), bytes.size()};
}

ruvia::Task<void> sendMultiplexed(
    const ruvia::HttpClientHandle& client,
    std::array<int, 16>& results,
    std::size_t index) {
    auto request = ruvia::HttpClientRequestView{.method = "POST", .target = "/multiplex", .content = ruvia::HttpClientRequestContentView::bytes("multiplex")};
    auto response = co_await client.send(std::move(request));
    results[index] = co_await response.body().readAll() == "multiplex" ? 1 : -1;
}

ruvia::Task<void> sendSlowWithTimeout(const ruvia::HttpClientHandle& client, bool& timedOut) {
    try {
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/slow"};
        (void)co_await client.send(request, {.timeout = 20ms});
    } catch (const ruvia::HttpClientError& error) {
        timedOut = error.code() == ruvia::HttpClientError::Code::kTimeout;
    }
}

ruvia::Task<void> requestStopAfterDelay(
    const ruvia::WorkerHandle& worker,
    ruvia::StopSource& source) {
    (void)co_await ruvia::sleepFor(worker, 20ms);
    source.requestStop();
}

ruvia::Task<void> sendSlowWithCancellation(
    const ruvia::HttpClientHandle& client,
    const ruvia::WorkerHandle& worker,
    std::pmr::memory_resource* resource,
    bool& cancelled) {
    ruvia::StopSource source;
    ruvia::TaskScope cancellation(worker, {.resource = resource});
    cancellation.spawn(requestStopAfterDelay(worker, source));
    try {
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/slow"};
        (void)co_await client.send(request, {
            .timeout = 2s,
            .stopToken = source.token(),
        });
    } catch (const ruvia::HttpClientError& error) {
        cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
    }
    co_await cancellation.join();
}

ruvia::Task<void> sendFastAlongsideCancelled(const ruvia::HttpClientHandle& client, bool& completed) {
    auto request = ruvia::HttpClientRequestView{.method = "POST", .target = "/echo", .content = ruvia::HttpClientRequestContentView::bytes("still-open")};
    auto response = co_await client.send(request, {.timeout = 2s});
    completed = co_await response.body().readAll() == "still-open";
}

SelfSignedPem makeSelfSignedPem(const char* subjectAlternativeName = "DNS:localhost") {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509V3_CTX extensionContext;
    X509V3_set_ctx_nodb(&extensionContext);
    X509V3_set_ctx(&extensionContext, x509, x509, nullptr, nullptr, 0);
    const auto addExtension = [&](int nid, const char* value) {
        X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &extensionContext, nid, value);
        if (extension == nullptr || X509_add_ext(x509, extension, -1) != 1) {
            if (extension != nullptr) X509_EXTENSION_free(extension);
            throw std::runtime_error("failed to create self-signed certificate extension");
        }
        X509_EXTENSION_free(extension);
    };
    addExtension(NID_basic_constraints, "critical,CA:TRUE");
    addExtension(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
    addExtension(NID_ext_key_usage, "serverAuth,clientAuth");
    addExtension(NID_subject_alt_name, subjectAlternativeName);
    X509_sign(x509, pkey, EVP_sha256());
    SelfSignedPem result;
    BIO* cert = BIO_new(BIO_s_mem());
    BIO* key = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert, x509);
    PEM_write_bio_PrivateKey(key, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    auto size = BIO_get_mem_data(cert, &data);
    result.cert.assign(data, static_cast<std::size_t>(size));
    size = BIO_get_mem_data(key, &data);
    result.key.assign(data, static_cast<std::size_t>(size));
    BIO_free(cert);
    BIO_free(key);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return result;
}

class TruncatedTlsServer final {
public:
    TruncatedTlsServer(const std::filesystem::path& certificateChainFile, const std::filesystem::path& privateKeyFile)
        : tlsContext_(asio::ssl::context::tls_server),
          acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
        tlsContext_.use_certificate_chain_file(certificateChainFile.string());
        tlsContext_.use_private_key_file(privateKeyFile.string(), asio::ssl::context::pem);
        thread_ = std::thread([this] {
            asio::ssl::stream<asio::ip::tcp::socket> stream(io_, tlsContext_);
            std::error_code error;
            acceptor_.accept(stream.next_layer(), error);
            if (error) return;
            stream.handshake(asio::ssl::stream_base::server, error);
            if (error) return;
            asio::streambuf request;
            asio::read_until(stream, request, "\r\n\r\n", error);
            if (error) return;
            static constexpr std::string_view response =
                "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial";
            asio::write(stream, asio::buffer(response), error);
            if (error) return;
            stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, error);
            stream.next_layer().close(error);
        });
    }

    ~TruncatedTlsServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    TruncatedTlsServer(const TruncatedTlsServer&) = delete;
    TruncatedTlsServer& operator=(const TruncatedTlsServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    asio::io_context io_;
    asio::ssl::context tlsContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

int selectH2Alpn(SSL*, const unsigned char** output, unsigned char* outputLength,
    const unsigned char* input, unsigned int inputLength, void*) noexcept {
    static constexpr unsigned char protocols[] = {2, 'h', '2'};
    return SSL_select_next_proto(const_cast<unsigned char**>(output), outputLength,
               protocols, static_cast<unsigned int>(sizeof(protocols)), input, inputLength) ==
            OPENSSL_NPN_NEGOTIATED
        ? SSL_TLSEXT_ERR_OK
        : SSL_TLSEXT_ERR_NOACK;
}

class TruncatedHttp2TlsServer final {
public:
    TruncatedHttp2TlsServer(const std::filesystem::path& certificateChainFile,
        const std::filesystem::path& privateKeyFile)
        : tlsContext_(asio::ssl::context::tls_server),
          acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
        tlsContext_.use_certificate_chain_file(certificateChainFile.string());
        tlsContext_.use_private_key_file(privateKeyFile.string(), asio::ssl::context::pem);
        SSL_CTX_set_alpn_select_cb(tlsContext_.native_handle(), &selectH2Alpn, nullptr);
        thread_ = std::thread([this] {
            asio::ssl::stream<asio::ip::tcp::socket> stream(io_, tlsContext_);
            std::error_code error;
            acceptor_.accept(stream.next_layer(), error);
            if (error) return;
            stream.handshake(asio::ssl::stream_base::server, error);
            if (error) return;

            std::array<char, 24> clientPreface{};
            asio::read(stream, asio::buffer(clientPreface), error);
            if (error || std::string_view(clientPreface.data(), clientPreface.size()) !=
                    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") return;

            if (!readFrame(stream, error)) return;  // Client SETTINGS.
            static constexpr std::array<unsigned char, 9> serverSettings{
                0, 0, 0, 4, 0, 0, 0, 0, 0};
            asio::write(stream, asio::buffer(serverSettings), error);
            if (error) return;

            // Wait until the request HEADERS is on the wire so the client has a
            // live pending stream when the TLS transport is truncated.
            for (;;) {
                const auto frame = readFrame(stream, error);
                if (!frame || error) return;
                if (frame->type == 1 && frame->streamId != 0) break;
            }
            stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, error);
            stream.next_layer().close(error);
        });
    }

    ~TruncatedHttp2TlsServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    TruncatedHttp2TlsServer(const TruncatedHttp2TlsServer&) = delete;
    TruncatedHttp2TlsServer& operator=(const TruncatedHttp2TlsServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    struct Frame final {
        unsigned char type{0};
        std::uint32_t streamId{0};
    };

    static std::optional<Frame> readFrame(
        asio::ssl::stream<asio::ip::tcp::socket>& stream, std::error_code& error) {
        std::array<unsigned char, 9> header{};
        asio::read(stream, asio::buffer(header), error);
        if (error) return std::nullopt;
        const auto length = (static_cast<std::size_t>(header[0]) << 16U) |
            (static_cast<std::size_t>(header[1]) << 8U) |
            static_cast<std::size_t>(header[2]);
        std::vector<unsigned char> payload(length);
        if (!payload.empty()) {
            asio::read(stream, asio::buffer(payload), error);
            if (error) return std::nullopt;
        }
        const auto streamId =
            (static_cast<std::uint32_t>(header[5] & 0x7fU) << 24U) |
            (static_cast<std::uint32_t>(header[6]) << 16U) |
            (static_cast<std::uint32_t>(header[7]) << 8U) |
            static_cast<std::uint32_t>(header[8]);
        return Frame{.type = header[3], .streamId = streamId};
    }

    asio::io_context io_;
    asio::ssl::context tlsContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

int runTlsTruncationCheck(
    const std::filesystem::path& certificateChainFile,
    const std::filesystem::path& privateKeyFile) {
    TruncatedTlsServer server(certificateChainFile, privateKeyFile);
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1"};
    publicConfig.port = server.port();
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.tlsPeerVerification = ruvia::HttpClientTlsPeerVerificationPolicy::kSkipVerification;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 1;
        try {
            auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto response = co_await client.send(request, {.timeout = 2s});
            (void)co_await response.body().readAll();
        } catch (const ruvia::HttpClientError& error) {
            result = error.code() == ruvia::HttpClientError::Code::kTlsFailed ? 0 : 2;
        }
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int runHttp2TlsTruncationCheck(
    const std::filesystem::path& certificateChainFile,
    const std::filesystem::path& privateKeyFile) {
    TruncatedHttp2TlsServer server(certificateChainFile, privateKeyFile);
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1"};
    publicConfig.port = server.port();
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    publicConfig.tlsPeerVerification = ruvia::HttpClientTlsPeerVerificationPolicy::kSkipVerification;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 1;
        try {
            auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            (void)co_await client.send(request, {.timeout = 2s});
        } catch (const ruvia::HttpClientError& error) {
            result = error.code() == ruvia::HttpClientError::Code::kTlsFailed ? 0 :
                error.code() == ruvia::HttpClientError::Code::kIoError ? 2 : 3;
        }
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(
        io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int runVerifiedTlsClient(
    std::uint16_t port,
    std::string_view host,
    const std::filesystem::path& caFile,
    const std::filesystem::path* certificateChainFile = nullptr,
    const std::filesystem::path* privateKeyFile = nullptr,
    bool expectSuccess = true,
    bool allowIoFailure = false) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{
        .scheme = ruvia::HttpScheme::kHttps,
        .host = std::string(host),
    };
    publicConfig.port = port;
    publicConfig.protocol = ruvia::HttpClientProtocol::kNegotiate;
    publicConfig.caFile = caFile.string();
    if (certificateChainFile != nullptr && privateKeyFile != nullptr) {
        publicConfig.certificateChainFile = certificateChainFile->string();
        publicConfig.privateKeyFile = privateKeyFile->string();
    }
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 0;
        try {
            auto request = ruvia::HttpClientRequestView{.method = "POST", .target = "/echo", .content = ruvia::HttpClientRequestContentView::bytes("verified")};
            auto response = co_await client.send(request, {.timeout = 2s});
            if (!expectSuccess || co_await response.body().readAll() != "verified" ||
                response.protocolVersion() != ruvia::HttpProtocolVersion::kHttp2) {
                result = 1;
            }
        } catch (const ruvia::HttpClientError& error) {
            const bool expectedFailure =
                error.code() == ruvia::HttpClientError::Code::kTlsFailed ||
                (allowIoFailure && error.code() == ruvia::HttpClientError::Code::kIoError);
            if (expectSuccess || !expectedFailure) {
                std::fprintf(stderr, "verified TLS client failed with code %u: %s\n",
                    static_cast<unsigned int>(error.code()), error.what());
                result = 2;
            }
        }
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

int runClient(std::uint16_t port, ruvia::HttpScheme scheme, ruvia::HttpClientProtocol protocol) {
    const bool isHttp2 = protocol == ruvia::HttpClientProtocol::kHttp2Only;
    reportStage(isHttp2 ? "run-client.h2.begin" : "run-client.h1.begin");
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = scheme == ruvia::HttpScheme::kHttps
        ? ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1"}
        : ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    publicConfig.port = port;
    publicConfig.protocol = protocol;
    if (scheme == ruvia::HttpScheme::kHttps) {
        publicConfig.tlsPeerVerification = ruvia::HttpClientTlsPeerVerificationPolicy::kSkipVerification;
    }
    publicConfig.connectionsPerWorker = 1;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    OneShotFailingResource operationResource;
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(&operationResource, scope);
        auto runRequests = [&]() -> ruvia::Task<int> {
            if (client.host() != "127.0.0.1" || client.port() != port || client.scheme() != scheme) co_return 3;
            int result = 0;
            reportStage(isHttp2 ? "run-client.h2.basic" : "run-client.h1.basic");
            for (int i = 0; i < 2; ++i) {
                auto request = ruvia::HttpClientRequestView{.method = "POST", .target = "/echo", .content = ruvia::HttpClientRequestContentView::bytes("payload")};
                auto operation = client.send(std::move(request));
                auto response = co_await std::move(operation);
                if (response.status() != ruvia::http_status::kOk || co_await response.body().readAll() != "payload" || response.protocolVersion() != (protocol == ruvia::HttpClientProtocol::kHttp2Only ? ruvia::HttpProtocolVersion::kHttp2 : ruvia::HttpProtocolVersion::kHttp11)) {
                    result = 1;
                    break;
                }
            }
            if (result == 0) {
                reportStage(isHttp2 ? "run-client.h2.header" : "run-client.h1.header");
                const std::array h1Headers{ruvia::HttpHeaderView{"X-Mixed-Case", "preserved"}};
                const std::array h2Headers{
                    ruvia::HttpHeaderView{"X-Mixed-Case", "preserved"},
                    ruvia::HttpHeaderView{"TE", "Trailers"},
                };
                auto mixedCaseResponse = protocol == ruvia::HttpClientProtocol::kHttp2Only
                    ? co_await client.send({.target = "/header", .headers = h2Headers})
                    : co_await client.send({.target = "/header", .headers = h1Headers});
                if (co_await mixedCaseResponse.body().readAll() != "preserved") result = 5;
            }
            if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
                reportStage("run-client.h2.multiplex");
                std::array<int, 16> results{};
                ruvia::TaskScope batch(worker, {.resource = memory.resource()});
                const auto started = std::chrono::steady_clock::now();
                for (std::size_t i = 0; i < results.size(); ++i) {
                    batch.spawn(sendMultiplexed(client, results, i));
                }
                co_await batch.join();
                const auto elapsed = std::chrono::steady_clock::now() - started;
                if (!std::ranges::all_of(results, [](int value) { return value == 1; }) || elapsed >= 400ms) result = 3;
            }
            if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
                reportStage("run-client.h2.timeout-cancel");
                bool slowTimedOut = false;
                bool slowCancelled = false;
                bool fastCompleted = false;
                ruvia::TaskScope mixed(worker, {.resource = memory.resource()});
                mixed.spawn(sendSlowWithTimeout(client, slowTimedOut));
                mixed.spawn(sendSlowWithCancellation(client, worker, memory.resource(), slowCancelled));
                mixed.spawn(sendFastAlongsideCancelled(client, fastCompleted));
                co_await mixed.join();
                if (!slowTimedOut || !slowCancelled || !fastCompleted) result = 4;
            }
            if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
                reportStage("run-client.h2.decode-error");
                bool preservedDecoderError = false;
                try {
                    auto request = ruvia::HttpClientRequestView{.target = "/invalid-content-encoding"};
                    (void)co_await client.send(std::move(request));
                } catch (const ruvia::HttpClientError& error) {
                    preservedDecoderError =
                        error.code() == ruvia::HttpClientError::Code::kProtocolError &&
                        std::string_view(error.what()) ==
                            "invalid HTTP response Content-Encoding";
                    if (!preservedDecoderError) {
                        std::fprintf(
                            stderr,
                            "unexpected HTTP/2 decoder error (%u): %s\n",
                            static_cast<unsigned int>(error.code()),
                            error.what());
                    }
                }
                if (!preservedDecoderError) result = 6;
            }
            if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
                reportStage("run-client.h2.allocation-failure");
                const std::string requestBody(4096, 'x');
                auto request = ruvia::HttpClientRequestView{
                    .method = "POST",
                    .target = "/echo",
                    .content = ruvia::HttpClientRequestContentView::bytes(requestBody),
                };
                // MSVC's Debug STL allocates small iterator proxies while constructing
                // empty PMR containers. Fail the first real response-storage request
                // instead, so every standard library exercises the recoverable path.
                operationResource.failNextAllocationAtLeast(sizeof(ruvia::HttpClientResponseHeader));
                bool preservedAllocationFailure = false;
                try {
                    (void)co_await client.send(std::move(request));
                } catch (const std::bad_alloc&) {
                    preservedAllocationFailure = true;
                }
                if (!preservedAllocationFailure) result = 7;
                if (result == 0) {
                    auto recoveryRequest = ruvia::HttpClientRequestView{.method = "POST", .target = "/echo", .content = ruvia::HttpClientRequestContentView::bytes("after-allocation-failure")};
                    auto recoveryResponse = co_await client.send(std::move(recoveryRequest));
                    if (co_await recoveryResponse.body().readAll() != "after-allocation-failure") result = 8;
                }
            }
            co_return result;
        };

        int result = 0;
        std::exception_ptr failure;
        try {
            result = co_await runRequests();
        } catch (...) {
            failure = std::current_exception();
        }
        reportStage(isHttp2 ? "run-client.h2.scope-close" : "run-client.h1.scope-close");
        scope.close();
        reportStage(isHttp2 ? "run-client.h2.registry-close" : "run-client.h1.registry-close");
        registry.closeNow();
        reportStage(isHttp2 ? "run-client.h2.registry-join" : "run-client.h1.registry-join");
        try {
            co_await registry.join();
        } catch (...) {
            if (failure == nullptr) failure = std::current_exception();
        }
        reportStage(isHttp2 ? "run-client.h2.registry-joined" : "run-client.h1.registry-joined");
        if (failure != nullptr) std::rethrow_exception(failure);
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    reportStage(isHttp2 ? "run-client.h2.io-run" : "run-client.h1.io-run");
    io.run();
    reportStage(isHttp2 ? "run-client.h2.io-stopped" : "run-client.h1.io-stopped");
    int result = 0;
    std::exception_ptr failure;
    try {
        result = future.get();
    } catch (...) {
        failure = std::current_exception();
    }
    registry.closeNow();
    dispatcher->detachContext();
    if (failure != nullptr) std::rethrow_exception(failure);
    reportStage(isHttp2 ? "run-client.h2.done" : "run-client.h1.done");
    return result;
}

int runTimeoutReconnect() {
    using namespace std::chrono_literals;
    asio::io_context serverIo;
    asio::ip::tcp::acceptor acceptor(serverIo, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = acceptor.local_endpoint().port();
    std::thread upstream([&] {
        std::error_code ec;
        auto first = acceptor.accept(ec);
        if (!ec) {
            asio::streambuf request;
            asio::read_until(first, request, "\r\n\r\n", ec);
            std::this_thread::sleep_for(150ms);
            first.close(ec);
        }
        ec.clear();
        auto second = acceptor.accept(ec);
        if (!ec) {
            asio::streambuf request;
            asio::read_until(second, request, "\r\n\r\n", ec);
            if (!ec) asio::write(second, asio::buffer(std::string_view("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\nok\r\n0\r\n\r\n")), ec);
        }
    });

    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    publicConfig.port = port;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        bool timedOut = false;
        try {
            auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto first = client.send(request, {.timeout = 50ms});
            (void)co_await std::move(first);
        } catch (const ruvia::HttpClientError& error) {
            timedOut = error.code() == ruvia::HttpClientError::Code::kTimeout;
        }
        if (!timedOut) co_return 1;
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        auto second = client.send(request, {.timeout = 2s});
        auto response = co_await std::move(second);
        const auto body = co_await response.body().readAll();
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return body == "ok" ? 0 : 2;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    upstream.join();
    return result;
}

int runCookies(std::uint16_t port) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    publicConfig.port = port;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 0;
        if (client.host() != "127.0.0.1" || client.port() != port || client.scheme() != ruvia::HttpScheme::kHttp) result = 1;

        auto firstRequest = ruvia::HttpClientRequestView{.method = "GET", .target = "/cookie"};
        auto firstOperation = client.send(std::move(firstRequest));
        auto first = co_await std::move(firstOperation);
        if (co_await first.body().readAll() != "new") result = 2;

        const std::array headers{ruvia::HttpHeaderView{"cookie", "manual=yes"}};
        auto secondRequest = ruvia::HttpClientRequestView{
            .method = "GET", .target = "/cookie", .headers = headers};
        auto secondOperation = client.send(std::move(secondRequest));
        auto second = co_await std::move(secondOperation);
        const auto secondBody = co_await second.body().readAll();
        if (secondBody.find("sid=abc") == std::string_view::npos || secondBody.find("manual=yes") == std::string_view::npos) result = 3;
        const auto stats = client.stats();
        if (stats.bufferedRequests != 0 || stats.inFlightRequests != 0 ||
            stats.completedRequests != 2 || stats.bytesSent == 0 ||
            stats.bytesReceived == 0) result = 4;
        scope.close();
        try {
            (void)client.stats();
            result = 5;
        } catch (const std::logic_error&) {
        }
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int runHostPrefixedCookies(std::uint16_t port) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1"};
    publicConfig.port = port;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    publicConfig.tlsPeerVerification = ruvia::HttpClientTlsPeerVerificationPolicy::kSkipVerification;
    publicConfig.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 0;
        auto firstRequest = ruvia::HttpClientRequestView{.method = "GET", .target = "/host-prefix-cookie"};
        auto first = co_await client.send(firstRequest, {.timeout = 2s});
        if (co_await first.body().readAll() != "new") {
            result = 1;
        } else {
            auto secondRequest = ruvia::HttpClientRequestView{.method = "GET", .target = "/host-prefix-cookie"};
            auto second = co_await client.send(secondRequest, {.timeout = 2s});
            const auto secondBody = co_await second.body().readAll();
            if (secondBody.find("__Host-good=good") == std::string_view::npos) {
                result = 2;
            } else if (secondBody.find("__Host-bad=bad") != std::string_view::npos) {
                result = 3;
            }
        }
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int runBoundedBuffer() {
    using namespace std::chrono_literals;
    asio::io_context serverIo;
    asio::ip::tcp::acceptor acceptor(serverIo, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = acceptor.local_endpoint().port();
    std::thread upstream([&] {
        for (int i = 0; i < 2; ++i) {
            std::error_code ec;
            auto socket = acceptor.accept(ec);
            if (ec) return;
            asio::streambuf request;
            asio::read_until(socket, request, "\r\n\r\n", ec);
            if (i == 0) std::this_thread::sleep_for(100ms);
            if (!ec) asio::write(socket, asio::buffer(std::string_view("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")), ec);
        }
    });

    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto publicConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    publicConfig.port = port;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.connectionsPerWorker = 1;
    publicConfig.maxBufferedRequestsPerWorker = 1;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    std::size_t remainingOperations = 3;
    auto send = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        int result = 2;
        try {
            auto operation = client.send(request, {.timeout = 2s});
            auto response = co_await std::move(operation);
            const auto body = co_await response.body().readAll();
            scope.close();
            result = body == "ok" ? 0 : 2;
        } catch (const ruvia::HttpClientError& error) {
            scope.close();
            result = error.code() == ruvia::HttpClientError::Code::kQueueFull ? 1 : 2;
        }
        if (--remainingOperations == 0) {
            registry.closeNow();
            co_await registry.join();
        }
        co_return result;
    };
    auto first = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(send()), asio::use_future);
    auto second = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(send()), asio::use_future);
    auto third = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(send()), asio::use_future);
    io.run();
    const auto result = first.get() == 0 && second.get() == 0 && third.get() == 1 ? 0 : 1;
    registry.closeNow();
    dispatcher->detachContext();
    upstream.join();
    return result;
}

int runHttp2GoawayRetry() {
    asio::io_context serverIo;
    asio::ip::tcp::acceptor acceptor(serverIo, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = acceptor.local_endpoint().port();
    std::thread upstream([&] {
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::error_code ec;
            auto socket = acceptor.accept(ec);
            if (ec) return;
            std::pmr::monotonic_buffer_resource resource;
            ruvia::detail::Http2Connection connection(&resource);
            connection.beginConnection();
            const auto flush = [&] {
                while (connection.wantsWrite() && !ec) {
                    const auto output = connection.pendingOutput();
                    asio::write(socket, asio::buffer(output), ec);
                    if (!ec) (void)connection.consumeOutput(output.size());
                }
            };
            flush();
            std::array<char, 16384> input{};
            bool requestEnded = false;
            std::uint32_t streamId = 0;
            while (!ec && !requestEnded) {
                const auto bytes = socket.read_some(asio::buffer(input), ec);
                if (ec) break;
                for (;;) {
                    const auto status = connection.feed(std::string_view(input.data(), bytes));
                    while (auto event = connection.nextEvent()) {
                        if (const auto* end = event->messageEnd()) {
                            requestEnded = true;
                            streamId = end->streamId();
                        }
                    }
                    flush();
                    if (status != ruvia::detail::Http2FeedResult::kEventsPending) break;
                }
            }
            if (ec || !requestEnded) return;
            if (attempt == 0) {
                const std::array<unsigned char, 17> goaway{
                    0, 0, 8, 7, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0, 0,
                };
                asio::write(socket, asio::buffer(goaway), ec);
                socket.close(ec);
                continue;
            }
            ruvia::HttpResponse response({.resource = &resource});
            const auto encodedBody = gzipContent("retried");
            response.header("Content-Encoding", "gzip");
            const auto* stream = connection.stream(streamId);
            if (stream == nullptr) return;
            const auto submitted = connection.submitStreamingResponseHead(
                streamId, std::move(response), ruvia::detail::ResponseStreamKind::kGeneric,
                ruvia::detail::ResponseTrailerIntent::kPresent);
            if (!submitted.submitted()) return;
            if (connection.submitData(streamId, encodedBody, ruvia::detail::Http2EndStream::kKeepOpen) != ruvia::detail::Http2DataSubmitStatus::kAccepted) return;
            const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{"server-timing", "db;dur=4"}};
            const auto validated = ruvia::detail::httpResponseTrailerSection(trailers);
            if (validated.section() == nullptr ||
                connection.finishResponse(streamId, *validated.section()) != ruvia::detail::Http2FinishSubmitStatus::kAccepted) return;
            flush();
        }
    });

    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto config = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    config.port = port;
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    ruvia::detail::HttpClientConfigStorage storedConfig(config, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(storedConfig)};
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        auto response = co_await client.send(request, {.timeout = 2s});
        int result = 0;
        if (co_await response.body().readAll() != "retried") result = 1;
        else if (response.trailer("server-timing") != std::optional<std::string_view>("db;dur=4")) result = 2;
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    dispatcher->detachContext();
    upstream.join();
    return result;
}

}  // namespace

int main() {
    reportStage("main.begin");
    ruvia::detail::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    auto echo = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await context.req().text();
        co_return context.text(body);
    };
    auto cookie = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto value = context.req().header("cookie");
        if (!value) {
            context.setCookie({.name = "sid", .value = "abc"});
            co_return context.text("new");
        }
        co_return context.text(*value);
    };
    auto hostPrefixCookie = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto value = context.req().header("cookie");
        if (value) co_return context.text(*value);
        auto response = context.text("new");
        response.header("Set-Cookie", "__Host-bad=bad; Secure; Path=relative", ruvia::HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
        response.header("Set-Cookie", "__Host-good=good; Secure; Path=/", ruvia::HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
        co_return response;
    };
    auto slow = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        (void)co_await ruvia::sleepFor(context.worker(), 100ms);
        co_return context.text("slow");
    };
    auto multiplex = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await context.req().text();
        (void)co_await ruvia::sleepFor(context.worker(), 50ms);
        co_return context.text(body);
    };
    auto header = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        co_return context.text(context.req().header("x-mixed-case").value_or("missing"));
    };
    auto clientState = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        std::string body(context.req().header("user-agent").value_or("missing"));
        body.push_back('|');
        body.append(context.req().header("cookie").value_or("missing"));
        co_return context.text(std::string_view(body));
    };
    auto invalidContentEncoding = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        auto response = context.text("not-a-gzip-stream");
        response.header("Content-Encoding", "gzip");
        co_return response;
    };
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/echo", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(echo), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/cookie", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(cookie), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/host-prefix-cookie", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(hostPrefixCookie), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/slow", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(slow), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/multiplex", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(multiplex), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/header", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(header), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/client-state", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(clientState), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/invalid-content-encoding", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(invalidContentEncoding), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.finalize();

    reportStage("main.plain-start");
    ruvia::detail::WebWorkerRuntime plain(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable());
    plain.start();
    reportStage("main.http1-client");
    const auto httpResult = runClient(plain.localEndpoint(ruvia::ListenerId{1}).port(), ruvia::HttpScheme::kHttp, ruvia::HttpClientProtocol::kHttp1Only);
    reportStage("main.http1-cookies");
    const auto cookieResult = runCookies(plain.localEndpoint(ruvia::ListenerId{1}).port());
    auto gatewayClientConfig = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    gatewayClientConfig.port = plain.localEndpoint(ruvia::ListenerId{1}).port();
    gatewayClientConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    ruvia::detail::Router gatewayRouter;
    auto& gatewayRouterImpl = ruvia::detail::RouterImpl::from(gatewayRouter);
    auto gatewayHandler = [gatewayClientConfig](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        auto client = context.httpClient(gatewayClientConfig);
        auto request = ruvia::HttpClientRequestView{.method = "POST", .target = "/echo", .content = ruvia::HttpClientRequestContentView::bytes("context-client")};
        auto operation = client.send(std::move(request));
        auto response = co_await std::move(operation);
        co_return context.text(co_await response.body().readAll());
    };
    gatewayRouterImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/call", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(gatewayHandler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    gatewayRouterImpl.finalize();
    ruvia::detail::WebWorkerRuntime gateway(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        gatewayRouterImpl.routeTable());
    reportStage("main.gateway-start");
    gateway.start();
    asio::io_context gatewayClientIo;
    asio::ip::tcp::socket gatewaySocket(gatewayClientIo);
    std::error_code gatewayEc;
    reportStage("main.gateway-exchange");
    gatewaySocket.connect(gateway.localEndpoint(ruvia::ListenerId{1}), gatewayEc);
    asio::write(gatewaySocket, asio::buffer(std::string_view("GET /call HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")), gatewayEc);
    std::string gatewayResponse;
    std::array<char, 1024> gatewayBuffer{};
    while (!gatewayEc) {
        const auto bytes = gatewaySocket.read_some(asio::buffer(gatewayBuffer), gatewayEc);
        gatewayResponse.append(gatewayBuffer.data(), bytes);
    }
    reportStage("main.gateway-response");
    gateway.stop();
    gateway.join();
    reportStage("main.plain-stop");
    plain.stop();
    plain.join();
    reportStage("main.http1-done");
    if (httpResult != 0 || cookieResult != 0 || !gatewayResponse.ends_with("context-client")) {
        std::fprintf(stderr, "HTTP/1 client exchange failed (h1=%d, cookie=%d)\n",
            httpResult, cookieResult);
        return 2;
    }

    reportStage("main.pem-generate");
    const auto pem = makeSelfSignedPem("IP:127.0.0.1");
    const auto mismatchPem = makeSelfSignedPem("DNS:localhost");
    const auto directory = std::filesystem::temp_directory_path() / "ruvia_http_client_tls";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    const auto certPath = directory / "cert.pem";
    const auto keyPath = directory / "key.pem";
    const auto mismatchCertPath = directory / "mismatch-cert.pem";
    const auto mismatchKeyPath = directory / "mismatch-key.pem";
    writeFile(certPath, pem.cert);
    writeFile(keyPath, pem.key);
    writeFile(mismatchCertPath, mismatchPem.cert);
    writeFile(mismatchKeyPath, mismatchPem.key);
    reportStage("main.tls-truncation-h1");
    const auto tlsTruncationResult = runTlsTruncationCheck(certPath, keyPath);
    reportStage("main.tls-truncation-h1-done");
    reportStage("main.tls-truncation-h2");
    const auto h2TlsTruncationResult = runHttp2TlsTruncationCheck(certPath, keyPath);
    reportStage("main.tls-truncation-h2-done");
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    tls.identity.privateKeyFile = std::pmr::string(keyPath.string(), std::pmr::get_default_resource());
    ruvia::detail::WebWorkerRuntime secure(
        ruvia::detail::HttpServerListenerDefinition(
            ruvia::ListenerId{1},
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            std::move(tls)),
        routerImpl.routeTable(), {}, std::move(options));
    reportStage("main.secure-start");
    secure.start();
    reportStage("main.http2-client");
    const auto h2Result = runClient(secure.localEndpoint(ruvia::ListenerId{1}).port(), ruvia::HttpScheme::kHttps, ruvia::HttpClientProtocol::kHttp2Only);
    reportStage("main.host-cookie");
    const auto hostPrefixCookieResult = runHostPrefixedCookies(secure.localEndpoint(ruvia::ListenerId{1}).port());
    reportStage("main.verified-tls");
    const auto verifiedResult = runVerifiedTlsClient(
        secure.localEndpoint(ruvia::ListenerId{1}).port(), "127.0.0.1", certPath);
    reportStage("main.secure-stop");
    secure.stop();
    secure.join();
    ruvia::detail::HttpServerOptions mismatchOptions;
    ruvia::detail::HttpServerListenerDefinition::Tls mismatchTls;
    mismatchTls.identity.certificateChainFile = std::pmr::string(mismatchCertPath.string(), std::pmr::get_default_resource());
    mismatchTls.identity.privateKeyFile = std::pmr::string(mismatchKeyPath.string(), std::pmr::get_default_resource());
    ruvia::detail::WebWorkerRuntime mismatchSecure(
        ruvia::detail::HttpServerListenerDefinition(
            ruvia::ListenerId{1},
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            std::move(mismatchTls)),
        routerImpl.routeTable(), {}, std::move(mismatchOptions));
    reportStage("main.hostname-mismatch-start");
    mismatchSecure.start();
    reportStage("main.hostname-mismatch");
    const auto hostnameFailureResult = runVerifiedTlsClient(
        mismatchSecure.localEndpoint(ruvia::ListenerId{1}).port(), "127.0.0.1", mismatchCertPath, nullptr, nullptr, false);
    mismatchSecure.stop();
    mismatchSecure.join();
    reportStage("main.hostname-mismatch-done");
    if (tlsTruncationResult != 0 || h2TlsTruncationResult != 0 || h2Result != 0 ||
        hostPrefixCookieResult != 0 ||
        verifiedResult != 0 || hostnameFailureResult != 0) {
        std::fprintf(stderr,
            "HTTP/2 or verified TLS client exchange failed (h1-truncation=%d, "
            "h2-truncation=%d, h2=%d, host-cookie=%d, verified=%d, hostname=%d)\n",
            tlsTruncationResult, h2TlsTruncationResult, h2Result,
            hostPrefixCookieResult, verifiedResult, hostnameFailureResult);
        return 3;
    }

    ruvia::detail::HttpServerOptions mtlsOptions;
    ruvia::detail::HttpServerListenerDefinition::Tls mtls;
    mtls.identity.certificateChainFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    mtls.identity.privateKeyFile = std::pmr::string(keyPath.string(), std::pmr::get_default_resource());
    mtls.clientCertificates.emplace(
        std::pmr::get_default_resource(), ruvia::TlsClientCertificateRequirement::kRequired);
    mtls.clientCertificates->verifyFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    ruvia::detail::WebWorkerRuntime mtlsServer(
        ruvia::detail::HttpServerListenerDefinition(
            ruvia::ListenerId{1},
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
            std::move(mtls)),
        routerImpl.routeTable(), {}, std::move(mtlsOptions));
    reportStage("main.mtls-start");
    mtlsServer.start();
    reportStage("main.mtls-missing-cert");
    const auto missingClientCertificate = runVerifiedTlsClient(
        mtlsServer.localEndpoint(ruvia::ListenerId{1}).port(), "127.0.0.1", certPath, nullptr, nullptr, false, true);
    reportStage("main.mtls-client-cert");
    const auto mtlsResult = runVerifiedTlsClient(
        mtlsServer.localEndpoint(ruvia::ListenerId{1}).port(), "127.0.0.1", certPath, &certPath, &keyPath);
    reportStage("main.mtls-stop");
    mtlsServer.stop();
    mtlsServer.join();
    std::filesystem::remove_all(directory, ignored);
    if (missingClientCertificate != 0 || mtlsResult != 0) {
        std::fprintf(stderr, "HTTP client mutual TLS exchange failed (missing=%d, mtls=%d)\n",
            missingClientCertificate, mtlsResult);
        return 4;
    }
    reportStage("main.timeout-reconnect");
    if (runTimeoutReconnect() != 0) {
        std::fputs("HTTP client timeout did not discard and reconnect its socket\n", stderr);
        return 5;
    }
    reportStage("main.timeout-reconnect-done");
    reportStage("main.bounded-buffer");
    if (runBoundedBuffer() != 0) {
        std::fputs("HTTP client request buffer did not enforce its configured bound\n", stderr);
        return 6;
    }
    reportStage("main.bounded-buffer-done");
    reportStage("main.goaway-retry");
    if (const auto goawayResult = runHttp2GoawayRetry(); goawayResult != 0) {
        std::fprintf(stderr, "HTTP/2 GOAWAY retry/trailer exchange failed (%d)\n", goawayResult);
        return 7;
    }
    reportStage("main.goaway-retry-done");
    reportStage("main.done");
    return 0;
}
