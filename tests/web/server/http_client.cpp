#include <array>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
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
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/util/CallableRef.h"

namespace {

using namespace std::chrono_literals;

struct SelfSignedPem { std::string cert; std::string key; };

ruvia::Task<void> sendMultiplexed(
    const ruvia::HttpClientPtr& client,
    std::array<int, 16>& results,
    std::size_t index) {
    auto request = client->newRequest();
    request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/multiplex").setBody("multiplex");
    auto response = co_await client->sendRequest(std::move(request));
    results[index] = response.body() == "multiplex" ? 1 : -1;
}

ruvia::Task<void> sendSlowWithTimeout(const ruvia::HttpClientPtr& client, bool& timedOut) {
    try {
        auto request = client->newRequest();
        request.setPath("/slow");
        (void)co_await client->sendRequest(std::move(request), 20ms);
    } catch (const ruvia::HttpClientError& error) {
        timedOut = error.code() == ruvia::HttpClientError::Code::kTimeout;
    }
}

ruvia::Task<void> sendFastAlongsideCancelled(const ruvia::HttpClientPtr& client, bool& completed) {
    auto request = client->newRequest();
    request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("still-open");
    auto response = co_await client->sendRequest(std::move(request), 2s);
    completed = response.body() == "still-open";
}

int checkFactoryOrigins() {
    const auto ipv6 = ruvia::HttpClient::newHttpClient("http://[::1]:8080/");
    if (ipv6->host() != "::1" || ipv6->port() != 8080 || ipv6->secure()) return 1;

    const auto mixedCase = ruvia::HttpClient::newHttpClient("HTTPS://Example.COM:8443");
    if (mixedCase->host() != "Example.COM" || mixedCase->port() != 8443 || !mixedCase->secure()) return 2;

    ruvia::HttpClientConfig config;
    config.host = "::1";
    ruvia::detail::HttpClientConfigStorage stored(config, std::pmr::get_default_resource());
    if (ruvia::detail::httpClientWireHost(stored, std::pmr::get_default_resource()) != "[::1]") return 3;

    const auto rejectsConfigHost = [](std::string_view host) {
        try {
            ruvia::HttpClientConfig invalid;
            invalid.host.assign(host);
            ruvia::detail::validateHttpClientConfig(invalid);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    if (!rejectsConfigHost("bad?host")) return 4;
    if (!rejectsConfigHost("::::")) return 5;
    if (!rejectsConfigHost("user@example.com")) return 6;

    const auto rejects = [](std::string_view origin) {
        try {
            (void)ruvia::HttpClient::newHttpClient(origin);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    if (!rejects("http://[not-an-ipv6-address]")) return 7;
    if (!rejects("http://example.com:")) return 8;
    if (!rejects("http://[::1]:")) return 9;
    return 0;
}

SelfSignedPem makeSelfSignedPem() {
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
    addExtension(NID_subject_alt_name, "DNS:localhost");
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

int runTlsTruncationCheck(
    const std::filesystem::path& certificateChainFile,
    const std::filesystem::path& privateKeyFile) {
    TruncatedTlsServer server(certificateChainFile, privateKeyFile);
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host = "127.0.0.1";
    publicConfig.port = server.port();
    publicConfig.scheme = ruvia::HttpScheme::kHttps;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.verifyCertificate = false;
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
            auto request = client.newRequest();
            (void)co_await client.sendRequest(std::move(request), 2s);
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

int runVerifiedTlsClient(
    std::uint16_t port,
    std::string_view host,
    const std::filesystem::path& caFile,
    const std::filesystem::path* certificateChainFile = nullptr,
    const std::filesystem::path* privateKeyFile = nullptr,
    bool expectSuccess = true) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host.assign(host);
    publicConfig.port = port;
    publicConfig.scheme = ruvia::HttpScheme::kHttps;
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
            auto request = client.newRequest();
            request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("verified");
            auto response = co_await client.sendRequest(std::move(request), 2s);
            if (!expectSuccess || response.body() != "verified" ||
                response.protocolVersion() != ruvia::HttpProtocolVersion::kHttp2) {
                result = 1;
            }
        } catch (const ruvia::HttpClientError& error) {
            if (expectSuccess || error.code() != ruvia::HttpClientError::Code::kTlsFailed) {
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
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host = "127.0.0.1";
    publicConfig.port = port;
    publicConfig.scheme = scheme;
    publicConfig.protocol = protocol;
    publicConfig.verifyCertificate = false;
    publicConfig.connectionsPerWorker = 1;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    ruvia::HttpClientConfig dynamicConfig;
    dynamicConfig.protocol = protocol;
    dynamicConfig.verifyCertificate = false;
    const auto origin = std::string(scheme == ruvia::HttpScheme::kHttps ? "https://127.0.0.1:" : "http://127.0.0.1:") + std::to_string(port);
    auto dynamicClient = ruvia::HttpClient::newHttpClient(origin, std::move(dynamicConfig));
    if (dynamicClient->host() != "127.0.0.1" || dynamicClient->port() != port || dynamicClient->secure() != (scheme == ruvia::HttpScheme::kHttps)) return 3;

    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        int result = 0;
        for (int i = 0; i < 2; ++i) {
            auto request = client.newRequest();
            request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("payload");
            auto operation = client.sendRequest(std::move(request));
            auto response = co_await std::move(operation);
            if (response.statusCode() != ruvia::http_status::kOk || response.body() != "payload" || response.protocolVersion() != (protocol == ruvia::HttpClientProtocol::kHttp2Only ? ruvia::HttpProtocolVersion::kHttp2 : ruvia::HttpProtocolVersion::kHttp11)) {
                result = 1;
                break;
            }
        }
        if (result == 0) {
            auto dynamicRequest = dynamicClient->newRequest();
            dynamicRequest.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("dynamic");
            auto dynamicOperation = dynamicClient->sendRequest(std::move(dynamicRequest));
            auto dynamicResponse = co_await std::move(dynamicOperation);
            if (dynamicResponse.body() != "dynamic" || dynamicResponse.protocolVersion() != (protocol == ruvia::HttpClientProtocol::kHttp2Only ? ruvia::HttpProtocolVersion::kHttp2 : ruvia::HttpProtocolVersion::kHttp11)) result = 2;
        }
        if (result == 0) {
            auto mixedCaseRequest = dynamicClient->newRequest();
            mixedCaseRequest.setPath("/header").addHeader("X-Mixed-Case", "preserved");
            if (protocol == ruvia::HttpClientProtocol::kHttp2Only) {
                mixedCaseRequest.addHeader("TE", "Trailers");
            }
            auto mixedCaseResponse = co_await dynamicClient->sendRequest(std::move(mixedCaseRequest));
            if (mixedCaseResponse.body() != "preserved") result = 5;
        }
        if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
            std::array<int, 16> results{};
            ruvia::TaskScope batch(worker, memory.resource());
            const auto started = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < results.size(); ++i) {
                batch.spawn(sendMultiplexed(dynamicClient, results, i));
            }
            co_await batch.join();
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (!std::ranges::all_of(results, [](int value) { return value == 1; }) || elapsed >= 400ms) result = 3;
        }
        if (result == 0 && protocol == ruvia::HttpClientProtocol::kHttp2Only) {
            bool slowTimedOut = false;
            bool fastCompleted = false;
            ruvia::TaskScope mixed(worker, memory.resource());
            mixed.spawn(sendSlowWithTimeout(dynamicClient, slowTimedOut));
            mixed.spawn(sendFastAlongsideCancelled(dynamicClient, fastCompleted));
            co_await mixed.join();
            if (!slowTimedOut || !fastCompleted) result = 4;
        }
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    registry.bindCurrent();
    io.run();
    registry.unbindCurrent();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
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
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host = "127.0.0.1";
    publicConfig.port = port;
    publicConfig.scheme = ruvia::HttpScheme::kHttp;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        bool timedOut = false;
        try {
            auto request = client.newRequest();
            auto first = client.sendRequest(std::move(request), {.timeout = 50ms});
            (void)co_await std::move(first);
        } catch (const ruvia::HttpClientError& error) {
            timedOut = error.code() == ruvia::HttpClientError::Code::kTimeout;
        }
        if (!timedOut) co_return 1;
        auto request = client.newRequest();
        auto second = client.sendRequest(std::move(request), {.timeout = 2s});
        auto response = co_await std::move(second);
        scope.close();
        co_return response.body() == "ok" ? 0 : 2;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    upstream.join();
    return result;
}

int runSharedDynamicClient(std::uint16_t port) {
    auto client = ruvia::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port));
    auto useOnWorker = [&client]() {
        asio::io_context io;
        auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
        auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        ruvia::WorkerMemory memory;
        ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), {});
        auto exercise = [&]() -> ruvia::Task<int> {
            auto request = client->newRequest();
            request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("shared");
            auto response = co_await client->sendRequest(std::move(request));
            co_return response.body() == "shared" ? 0 : 1;
        };
        auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
        registry.bindCurrent();
        io.run();
        registry.unbindCurrent();
        const auto result = future.get();
        registry.closeNow();
        dispatcher->detachContext();
        return result;
    };
    auto first = std::async(std::launch::async, useOnWorker);
    auto second = std::async(std::launch::async, useOnWorker);
    return first.get() == 0 && second.get() == 0 ? 0 : 1;
}

int runCookies(std::uint16_t port) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host = "127.0.0.1";
    publicConfig.port = port;
    publicConfig.scheme = ruvia::HttpScheme::kHttp;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.cookiesEnabled = true;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto exercise = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        if (client.host() != "127.0.0.1" || client.port() != port || client.secure() || client.onDefaultPort()) co_return 1;

        auto firstRequest = client.newRequest();
        firstRequest.setPath("/cookie");
        auto firstOperation = client.sendRequest(std::move(firstRequest));
        auto first = co_await std::move(firstOperation);
        if (first.body() != "new") co_return 2;

        client.addCookie("manual", "yes");
        auto secondRequest = client.newRequest();
        secondRequest.setPath("/cookie");
        auto secondOperation = client.sendRequest(std::move(secondRequest));
        auto second = co_await std::move(secondOperation);
        if (second.body().find("sid=abc") == std::string_view::npos || second.body().find("manual=yes") == std::string_view::npos) co_return 3;
        const auto stats = client.stats();
        if (client.requestsBufferSize() != 0 || client.outstandingRequests() != 0 || stats.completedRequests != 2 || client.bytesSent() == 0 || client.bytesReceived() == 0) co_return 4;
        scope.close();
        co_return 0;
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
    ruvia::HttpClientConfig publicConfig;
    publicConfig.host = "127.0.0.1";
    publicConfig.port = port;
    publicConfig.scheme = ruvia::HttpScheme::kHttp;
    publicConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    publicConfig.connectionsPerWorker = 1;
    publicConfig.maxBufferedRequestsPerWorker = 1;
    ruvia::detail::HttpClientConfigStorage config(publicConfig, memory.resource());
    ruvia::detail::HttpClientDefinition definition{std::pmr::string("default", memory.resource()), std::move(config)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));
    auto send = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(memory.resource(), scope);
        auto request = client.newRequest();
        try {
            auto operation = client.sendRequest(std::move(request), 2s);
            auto response = co_await std::move(operation);
            scope.close();
            co_return response.body() == "ok" ? 0 : 2;
        } catch (const ruvia::HttpClientError& error) {
            scope.close();
            co_return error.code() == ruvia::HttpClientError::Code::kQueueFull ? 1 : 2;
        }
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
            ruvia::HttpResponse response(&resource);
            const auto* stream = connection.stream(streamId);
            if (stream == nullptr) return;
            const auto submitted = connection.submitStreamingResponseHead(
                streamId, std::move(response), ruvia::detail::ResponseStreamKind::kGeneric,
                ruvia::detail::ResponseTrailerIntent::kPresent);
            if (!submitted.submitted()) return;
            if (connection.submitData(streamId, "retried", ruvia::detail::Http2EndStream::kKeepOpen) != ruvia::detail::Http2DataSubmitStatus::kAccepted) return;
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
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(), {});
    ruvia::HttpClientConfig config;
    config.protocol = ruvia::HttpClientProtocol::kHttp2Only;
    auto client = ruvia::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(port), std::move(config));
    auto exercise = [&]() -> ruvia::Task<int> {
        auto request = client->newRequest();
        auto response = co_await client->sendRequest(std::move(request), 2s);
        int result = 0;
        if (response.body() != "retried") result = 1;
        else if (response.getTrailer("server-timing") != std::optional<std::string_view>("db;dur=4")) result = 2;
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    registry.bindCurrent();
    io.run();
    registry.unbindCurrent();
    const auto result = future.get();
    dispatcher->detachContext();
    upstream.join();
    return result;
}

}  // namespace

int main() {
    const auto factoryOriginResult = checkFactoryOrigins();
    if (factoryOriginResult != 0) {
        std::fprintf(stderr, "HTTP client origin factory validation failed (%d)\n", factoryOriginResult);
        return 1;
    }
    ruvia::detail::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    auto echo = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await context.req().text();
        co_return context.text(body);
    };
    auto cookie = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto value = context.req().header("cookie");
        if (!value) {
            context.setCookie("sid", "abc");
            co_return context.text("new");
        }
        co_return context.text(*value);
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
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/echo", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(echo), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/cookie", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(cookie), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/slow", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(slow), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/multiplex", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(multiplex), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/header", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(header), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.finalize();

    ruvia::detail::HttpServer plain(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable());
    plain.start();
    const auto httpResult = runClient(plain.localEndpoint().port(), ruvia::HttpScheme::kHttp, ruvia::HttpClientProtocol::kHttp1Only);
    const auto cookieResult = runCookies(plain.localEndpoint().port());
    const auto sharedClientResult = runSharedDynamicClient(plain.localEndpoint().port());
    ruvia::HttpClientConfig gatewayClientConfig;
    gatewayClientConfig.host = "127.0.0.1";
    gatewayClientConfig.port = plain.localEndpoint().port();
    gatewayClientConfig.scheme = ruvia::HttpScheme::kHttp;
    gatewayClientConfig.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    ruvia::detail::HttpClientConfigStorage gatewayStoredConfig(gatewayClientConfig, std::pmr::get_default_resource());
    ruvia::detail::HttpClientDefinition gatewayClient{std::pmr::string("default", std::pmr::get_default_resource()), std::move(gatewayStoredConfig)};
    ruvia::detail::Router gatewayRouter;
    auto& gatewayRouterImpl = ruvia::detail::RouterImpl::from(gatewayRouter);
    auto gatewayHandler = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        auto client = context.httpClient();
        auto request = client.newRequest();
        request.setMethod(ruvia::HttpKnownMethod::kPost).setPath("/echo").setBody("context-client");
        auto operation = client.sendRequest(std::move(request), {.stopToken = context.stopToken()});
        auto response = co_await std::move(operation);
        co_return context.text(response.body());
    };
    gatewayRouterImpl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/call", std::pmr::get_default_resource()), ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(gatewayHandler), ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    gatewayRouterImpl.finalize();
    ruvia::detail::HttpServer gateway(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), gatewayRouterImpl.routeTable(),
        std::span<const ruvia::detail::DbDefinition>{}, std::span<const ruvia::detail::RedisDefinition>{},
        std::span<const ruvia::detail::WorkerStateDefinition>{}, std::span<const ruvia::detail::HttpClientDefinition>(&gatewayClient, 1));
    gateway.start();
    asio::io_context gatewayClientIo;
    asio::ip::tcp::socket gatewaySocket(gatewayClientIo);
    std::error_code gatewayEc;
    gatewaySocket.connect(gateway.localEndpoint(), gatewayEc);
    asio::write(gatewaySocket, asio::buffer(std::string_view("GET /call HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")), gatewayEc);
    std::string gatewayResponse;
    std::array<char, 1024> gatewayBuffer{};
    while (!gatewayEc) {
        const auto bytes = gatewaySocket.read_some(asio::buffer(gatewayBuffer), gatewayEc);
        gatewayResponse.append(gatewayBuffer.data(), bytes);
    }
    gateway.stop();
    gateway.join();
    plain.stop();
    plain.join();
    if (httpResult != 0 || cookieResult != 0 || sharedClientResult != 0 || !gatewayResponse.ends_with("context-client")) {
        std::fputs("HTTP/1 client exchange failed\n", stderr);
        return 2;
    }

    const auto pem = makeSelfSignedPem();
    const auto directory = std::filesystem::temp_directory_path() / "ruvia_http_client_tls";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    const auto certPath = directory / "cert.pem";
    const auto keyPath = directory / "key.pem";
    writeFile(certPath, pem.cert);
    writeFile(keyPath, pem.key);
    const auto tlsTruncationResult = runTlsTruncationCheck(certPath, keyPath);
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::HttpServerOptions::Tls tls;
    tls.identity.certificateChainFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    tls.identity.privateKeyFile = std::pmr::string(keyPath.string(), std::pmr::get_default_resource());
    options.transport = std::move(tls);
    ruvia::detail::HttpServer secure(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routerImpl.routeTable(), {}, std::move(options));
    secure.start();
    const auto h2Result = runClient(secure.localEndpoint().port(), ruvia::HttpScheme::kHttps, ruvia::HttpClientProtocol::kHttp2Only);
    const auto verifiedResult = runVerifiedTlsClient(
        secure.localEndpoint().port(), "localhost", certPath);
    const auto hostnameFailureResult = runVerifiedTlsClient(
        secure.localEndpoint().port(), "127.0.0.1", certPath, nullptr, nullptr, false);
    secure.stop();
    secure.join();
    if (tlsTruncationResult != 0 || h2Result != 0 || verifiedResult != 0 || hostnameFailureResult != 0) {
        std::fprintf(stderr,
            "HTTP/2 or verified TLS client exchange failed (truncation=%d, h2=%d, verified=%d, hostname=%d)\n",
            tlsTruncationResult, h2Result, verifiedResult, hostnameFailureResult);
        return 3;
    }

    ruvia::detail::HttpServerOptions mtlsOptions;
    ruvia::detail::HttpServerOptions::Tls mtls;
    mtls.identity.certificateChainFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    mtls.identity.privateKeyFile = std::pmr::string(keyPath.string(), std::pmr::get_default_resource());
    mtls.clientCertificates.emplace(
        std::pmr::get_default_resource(), ruvia::TlsClientCertificateRequirement::kRequired);
    mtls.clientCertificates->verifyFile = std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    mtlsOptions.transport = std::move(mtls);
    ruvia::detail::HttpServer mtlsServer(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0),
        routerImpl.routeTable(), {}, std::move(mtlsOptions));
    mtlsServer.start();
    const auto missingClientCertificate = runVerifiedTlsClient(
        mtlsServer.localEndpoint().port(), "localhost", certPath, nullptr, nullptr, false);
    const auto mtlsResult = runVerifiedTlsClient(
        mtlsServer.localEndpoint().port(), "localhost", certPath, &certPath, &keyPath);
    mtlsServer.stop();
    mtlsServer.join();
    std::filesystem::remove_all(directory, ignored);
    if (missingClientCertificate != 0 || mtlsResult != 0) {
        std::fprintf(stderr, "HTTP client mutual TLS exchange failed (missing=%d, mtls=%d)\n",
            missingClientCertificate, mtlsResult);
        return 4;
    }
    if (runTimeoutReconnect() != 0) {
        std::fputs("HTTP client timeout did not discard and reconnect its socket\n", stderr);
        return 5;
    }
    if (runBoundedBuffer() != 0) {
        std::fputs("HTTP client request buffer did not enforce its configured bound\n", stderr);
        return 6;
    }
    if (const auto goawayResult = runHttp2GoawayRetry(); goawayResult != 0) {
        std::fprintf(stderr, "HTTP/2 GOAWAY retry/trailer exchange failed (%d)\n", goawayResult);
        return 7;
    }
    return 0;
}
