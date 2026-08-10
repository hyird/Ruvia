#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

namespace {

using namespace std::chrono_literals;

class CountingResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept { return allocations_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
};

class OneShotServer final {
public:
    template <typename Handler>
    explicit OneShotServer(Handler handler)
        : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
          thread_([this, handler = std::move(handler)]() mutable {
              std::error_code error;
              auto socket = acceptor_.accept(error);
              if (!error) handler(socket);
          }) {}

    ~OneShotServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

class TwoShotServer final {
public:
    template <typename Handler>
    explicit TwoShotServer(Handler handler)
        : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
          thread_([this, handler = std::move(handler)]() mutable {
              for (unsigned exchange = 0; exchange < 2; ++exchange) {
                  std::error_code error;
                  auto socket = acceptor_.accept(error);
                  if (error) return;
                  handler(socket, exchange);
              }
          }) {}

    ~TwoShotServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    TwoShotServer(const TwoShotServer&) = delete;
    TwoShotServer& operator=(const TwoShotServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

private:
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

std::string readHead(asio::ip::tcp::socket& socket, std::error_code& error) {
    asio::streambuf input;
    asio::read_until(socket, input, "\r\n\r\n", error);
    if (error) return {};
    const auto bytes = input.data();
    return {asio::buffers_begin(bytes), asio::buffers_end(bytes)};
}

void writeResponse(asio::ip::tcp::socket& socket, std::string_view body, std::string_view extraHeaders = {}) {
    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n";
    response.append(extraHeaders);
    response.append("\r\n");
    response.append(body);
    std::error_code ignored;
    asio::write(socket, asio::buffer(response), ignored);
}

std::string gzipContent(std::string_view body) {
    auto encoded = ruvia::detail::encodeHttpContent(
        ruvia::detail::HttpContentCoding::kGzip, body,
        body.size() + 1024, std::pmr::get_default_resource());
    if (!encoded.encoded()) throw std::runtime_error("failed to encode test gzip body");
    const auto bytes = encoded.encoded()->bytes();
    return {bytes.data(), bytes.size()};
}

template <typename Exercise>
int runClient(ruvia::HttpClientConfig config, CountingResource& operationResource, Exercise exercise) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::detail::HttpClientConfigStorage stored(config, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(stored)};
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));

    auto task = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto client = registry.get(&operationResource, scope);
        const auto result = co_await exercise(client, worker, &operationResource);
        scope.close();
        registry.closeNow();
        co_await registry.join();
        co_return result;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(task()), asio::use_future);
    io.run();
    const auto result = future.get();
    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

ruvia::HttpClientConfig plainConfig(std::uint16_t port) {
    ruvia::HttpClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.scheme = ruvia::HttpScheme::kHttp;
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    return config;
}

int testOperationArena() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (!error) writeResponse(socket, std::string(4096, 'r'));
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource* resource) -> ruvia::Task<int> {
            const auto before = resource->allocations();
            auto request = client.newRequest();
            request.addHeader("x-arena", std::string(4096, 'h'));
            if (resource->allocations() == before) co_return 1;
            const auto afterRequest = resource->allocations();
            auto response = co_await client.sendRequest(std::move(request));
            if (response.body().size() != 4096) co_return 2;
            co_return resource->allocations() > afterRequest ? 0 : 3;
        });
}

int testResponseLimit() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (!error) writeResponse(socket, std::string(128, 'x'));
    });
    auto config = plainConfig(server.port());
    config.maxResponseBytes = 16;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = client.newRequest();
                (void)co_await client.sendRequest(std::move(request));
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kResponseTooLarge ? 0 : 2;
            }
            co_return 1;
        });
}

int testClosingInformationalResponse() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) return;
        constexpr std::string_view response =
            "HTTP/1.1 103 Early Hints\r\n"
            "Connection: close\r\n"
            "\r\n";
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = client.newRequest();
                (void)co_await client.sendRequest(std::move(request));
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kProtocolError ? 0 : 2;
            }
            co_return 1;
        });
}

int testTransferCodedResponse() {
    const auto encoded = gzipContent("decoded transfer body");
    OneShotServer server([encoded](asio::ip::tcp::socket& socket) {
        std::error_code error;
        const auto request = readHead(socket, error);
        if (error || request.find("TE: gzip") == std::string::npos) return;
        std::array<char, 32> sizeBytes{};
        const auto [sizeEnd, sizeError] = std::to_chars(
            sizeBytes.data(), sizeBytes.data() + sizeBytes.size(), encoded.size(), 16);
        if (sizeError != std::errc{}) return;
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: gzip, chunked\r\n"
            "Connection: close\r\n"
            "\r\n";
        response.append(sizeBytes.data(), sizeEnd);
        response.append("\r\n");
        response.append(encoded);
        response.append("\r\n0\r\n\r\n");
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            auto request = client.newRequest();
            request.addHeader("Connection", "TE").addHeader("TE", "gzip");
            auto response = co_await client.sendRequest(std::move(request));
            co_return response.body() == "decoded transfer body" ? 0 : 1;
        });
}

int testWriteTimeout() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code ignored;
        socket.set_option(asio::socket_base::receive_buffer_size(1024), ignored);
        std::this_thread::sleep_for(300ms);
        socket.close(ignored);
    });
    auto config = plainConfig(server.port());
    config.writeTimeout = 20ms;
    config.requestTimeout = 2s;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = client.newRequest();
                request.setMethod(ruvia::HttpKnownMethod::kPost).setBody(std::string(16 * 1024 * 1024, 'w'));
                (void)co_await client.sendRequest(std::move(request));
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kTimeout ? 0 : 2;
            }
            co_return 1;
        });
}

ruvia::Task<void> completeSlowRequest(const ruvia::HttpClient& client, int& result) {
    try {
        auto request = client.newRequest();
        auto response = co_await client.sendRequest(std::move(request));
        result = response.body() == "ok" ? 1 : -1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "slow negotiated request failed: %s\n", error.what());
        result = -1;
    }
}

ruvia::Task<void> timeOutQueuedRequest(const ruvia::HttpClient& client, int& result) {
    try {
        auto request = client.newRequest();
        (void)co_await client.sendRequest(std::move(request));
        result = -1;
    } catch (const ruvia::HttpClientError& error) {
        result = error.code() == ruvia::HttpClientError::Code::kTimeout ? 1 : -1;
    }
}

int testNegotiatedHttp1AcquireTimeout() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        std::this_thread::sleep_for(150ms);
        if (!error) writeResponse(socket, "ok");
    });
    auto config = plainConfig(server.port());
    config.protocol = ruvia::HttpClientProtocol::kNegotiate;
    config.acquireTimeout = 30ms;
    config.requestTimeout = 1s;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle& worker, CountingResource* resource) -> ruvia::Task<int> {
            int slow = 0;
            int queued = 0;
            ruvia::TaskScope requests(worker, resource);
            requests.spawn(completeSlowRequest(client, slow));
            (void)co_await ruvia::sleepFor(worker, 10ms);
            requests.spawn(timeOutQueuedRequest(client, queued));
            co_await requests.join();
            if (slow != 1 || queued != 1) {
                std::fprintf(stderr, "negotiated acquire results: slow=%d queued=%d\n", slow, queued);
            }
            co_return slow == 1 && queued == 1 ? 0 : 1;
        });
}

ruvia::Task<void> requestStopSoon(const ruvia::WorkerHandle& worker, ruvia::detail::StopSource& source) {
    (void)co_await ruvia::sleepFor(worker, 20ms);
    source.requestStop();
}

int testStopTokenCancellation() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        std::this_thread::sleep_for(200ms);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle& worker, CountingResource* resource) -> ruvia::Task<int> {
            ruvia::detail::StopSource source;
            ruvia::TaskScope cancellation(worker, resource);
            cancellation.spawn(requestStopSoon(worker, source));
            int result = 1;
            try {
                auto request = client.newRequest();
                (void)co_await client.sendRequest(std::move(request), {.stopToken = source.token()});
            } catch (const ruvia::HttpClientError& error) {
                result = error.code() == ruvia::HttpClientError::Code::kCancelled ? 0 : 2;
            }
            co_await cancellation.join();
            co_return result;
        });
}

int testConnectStopTokenCancellation() {
    OneShotServer server([](asio::ip::tcp::socket&) {
        std::this_thread::sleep_for(200ms);
    });
    auto config = plainConfig(server.port());
    config.scheme = ruvia::HttpScheme::kHttps;
    config.verifyCertificate = false;
    config.connectTimeout = 2s;
    config.requestTimeout = 2s;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle& worker, CountingResource* resource) -> ruvia::Task<int> {
            ruvia::detail::StopSource source;
            ruvia::TaskScope cancellation(worker, resource);
            cancellation.spawn(requestStopSoon(worker, source));
            int result = 1;
            try {
                auto request = client.newRequest();
                (void)co_await client.sendRequest(std::move(request), {.stopToken = source.token()});
            } catch (const ruvia::HttpClientError& error) {
                result = error.code() == ruvia::HttpClientError::Code::kCancelled ? 0 : 2;
            }
            co_await cancellation.join();
            co_return result;
        });
}

int testRegistryRejectsDynamicPoolsAfterClose() {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::detail::HttpClientRegistry registry(
        io, worker, memory.resource(), std::span<const ruvia::detail::HttpClientDefinition>{});
    auto existing = ruvia::HttpClient::newHttpClient("http://127.0.0.1:1");
    auto late = ruvia::HttpClient::newHttpClient("http://127.0.0.1:2");
    auto exercise = [&]() -> ruvia::Task<int> {
        {
            auto request = existing->newRequest();
            auto coldOperation = existing->sendRequest(std::move(request));
            static_cast<void>(coldOperation);
        }
        registry.closeNow();
        const auto rejected = [](const ruvia::HttpClientPtr& client) {
            try {
                auto request = client->newRequest();
                auto coldOperation = client->sendRequest(std::move(request));
                static_cast<void>(coldOperation);
            } catch (const ruvia::HttpClientError& error) {
                return error.code() == ruvia::HttpClientError::Code::kClosing;
            }
            return false;
        };
        const bool existingRejected = rejected(existing);
        const bool lateRejected = rejected(late);
        co_await registry.join();
        co_return existingRejected && lateRejected ? 0 : 1;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    registry.bindCurrent();
    io.run();
    registry.unbindCurrent();
    const auto result = future.get();
    dispatcher->detachContext();
    return result;
}

int testCookieCapacity() {
    ruvia::HttpClientConfig config;
    config.host = "127.0.0.1";
    config.scheme = ruvia::HttpScheme::kHttp;
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    config.maxCookiesPerWorker = 1;
    config.maxCookieBytesPerWorker = 8;
    CountingResource operationResource;
    const auto scopedResult = runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            client.addCookie("a", "1");
            try {
                client.addCookie("b", "2");
            } catch (const std::length_error&) {
                co_return 0;
            }
            co_return 1;
        });
    if (scopedResult != 0) return 1;

    auto dynamic = ruvia::HttpClient::newHttpClient("http://127.0.0.1", config);
    dynamic->addCookie("a", "1");
    try {
        dynamic->addCookie("b", "2");
    } catch (const std::length_error&) {
        return 0;
    }
    return 2;
}

int testAutomaticCookieCapacity() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) return;
        if (exchange == 0) {
            writeResponse(socket, "seeded", "Set-Cookie: a=1; Path=/\r\nSet-Cookie: b=2; Path=/\r\n");
            return;
        }
        const auto retainedFirst = head.find("cookie: a=1") != std::string::npos;
        const auto retainedSecond = head.find("b=2") != std::string::npos;
        writeResponse(socket, retainedFirst && !retainedSecond ? "bounded" : "leaked");
    });
    auto config = plainConfig(server.port());
    config.cookiesEnabled = true;
    config.maxCookiesPerWorker = 1;
    config.maxCookieBytesPerWorker = 64;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            auto first = client.newRequest();
            auto firstResponse = co_await client.sendRequest(std::move(first));
            if (firstResponse.body() != "seeded") co_return 1;
            auto second = client.newRequest();
            auto secondResponse = co_await client.sendRequest(std::move(second));
            co_return secondResponse.body() == "bounded" ? 0 : 2;
        });
}

int testCookieIdentityCanonicalization() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) return;
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: sid=host-only; Path=/\r\n"
                "Set-Cookie: sid=domain; Domain=LOCALHOST; Path=/\r\n");
            return;
        }
        const auto oneCookie = head.find("cookie: sid=domain") != std::string::npos &&
            head.find("sid=host-only") == std::string::npos &&
            head.find("sid=domain; sid=domain") == std::string::npos;
        writeResponse(socket, oneCookie ? "canonical" : "duplicate");
    });
    auto config = plainConfig(server.port());
    config.host = "localhost";
    config.cookiesEnabled = true;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            auto first = client.newRequest();
            auto firstResponse = co_await client.sendRequest(std::move(first));
            if (firstResponse.body() != "seeded") co_return 1;
            auto second = client.newRequest();
            auto secondResponse = co_await client.sendRequest(std::move(second));
            co_return secondResponse.body() == "canonical" ? 0 : 2;
        });
}

int testLargeCookieMaxAge() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) return;
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: long_lived=yes; Path=/; Max-Age=9223372036854775807\r\n");
            return;
        }
        writeResponse(socket,
            head.find("cookie: long_lived=yes") != std::string::npos ? "retained" : "expired");
    });
    auto config = plainConfig(server.port());
    config.cookiesEnabled = true;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClient& client, const ruvia::WorkerHandle&, CountingResource*) -> ruvia::Task<int> {
            auto first = client.newRequest();
            auto firstResponse = co_await client.sendRequest(std::move(first));
            if (firstResponse.body() != "seeded") co_return 1;
            auto second = client.newRequest();
            auto secondResponse = co_await client.sendRequest(std::move(second));
            co_return secondResponse.body() == "retained" ? 0 : 2;
        });
}

}  // namespace

int main() {
    try {
        const std::array<std::pair<int (*)(), std::string_view>, 13> checks{{
            {&testOperationArena, "operation arena"},
            {&testResponseLimit, "response limit"},
            {&testClosingInformationalResponse, "closing informational response"},
            {&testTransferCodedResponse, "transfer-coded response"},
            {&testWriteTimeout, "HTTP/1 write timeout"},
            {&testNegotiatedHttp1AcquireTimeout, "negotiated HTTP/1 acquire timeout"},
            {&testStopTokenCancellation, "stop-token cancellation"},
            {&testConnectStopTokenCancellation, "connect stop-token cancellation"},
            {&testRegistryRejectsDynamicPoolsAfterClose, "registry close gate"},
            {&testCookieCapacity, "cookie capacity"},
            {&testAutomaticCookieCapacity, "automatic cookie capacity"},
            {&testCookieIdentityCanonicalization, "cookie identity canonicalization"},
            {&testLargeCookieMaxAge, "large cookie Max-Age"},
        }};
        for (const auto& [check, name] : checks) {
            if (const auto result = check(); result != 0) {
                std::fprintf(stderr, "HTTP client %.*s check failed (%d)\n",
                    static_cast<int>(name.size()), name.data(), result);
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HTTP client limit checks failed: %s\n", error.what());
        return 1;
    }
}
