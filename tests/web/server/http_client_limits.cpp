#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

namespace {

using namespace std::chrono_literals;

class CountingResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

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

class FailSelectedLargeAllocationResource final : public std::pmr::memory_resource {
public:
    void failAllocationSizeRange(std::size_t minimum, std::size_t maximum) noexcept {
        failMinimumBytes_ = minimum;
        failMaximumBytes_ = maximum;
    }
    void disableFailures() noexcept {
        failMinimumBytes_ = std::nullopt;
        failMaximumBytes_ = std::nullopt;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (failMinimumBytes_.has_value() && bytes >= *failMinimumBytes_ &&
            bytes <= failMaximumBytes_.value_or(std::numeric_limits<std::size_t>::max())) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> failMinimumBytes_;
    std::optional<std::size_t> failMaximumBytes_;
};

class OneShotServer final {
public:
    template <typename Handler>
    explicit OneShotServer(Handler handler)
        : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
          thread_([this, handler = std::move(handler)]() mutable {
              std::error_code error;
              auto socket = acceptor_.accept(error);
              if (!error) {
                  handler(socket);
              }
          }) {}

    ~OneShotServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

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
                  if (error) {
                      return;
                  }
                  handler(socket, exchange);
              }
          }) {}

    ~TwoShotServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    TwoShotServer(const TwoShotServer&) = delete;
    TwoShotServer& operator=(const TwoShotServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

std::string readHead(asio::ip::tcp::socket& socket, std::error_code& error) {
    asio::streambuf input;
    const auto headBytes = asio::read_until(socket, input, "\r\n\r\n", error);
    if (error) {
        return {};
    }
    const auto bytes = input.data();
    auto begin = asio::buffers_begin(bytes);
    std::string head(begin, begin + static_cast<std::ptrdiff_t>(headBytes));
    input.consume(headBytes);

    constexpr std::string_view kContentLengthPrefix = "Content-Length: ";
    const auto contentLengthPosition = head.find(kContentLengthPrefix);
    if (contentLengthPosition == std::string::npos) {
        return head;
    }
    const auto valueBegin = contentLengthPosition + kContentLengthPrefix.size();
    const auto valueEnd = head.find("\r\n", valueBegin);
    if (valueEnd == std::string::npos) {
        return head;
    }
    std::size_t contentLength = 0;
    const auto value = std::string_view(head).substr(valueBegin, valueEnd - valueBegin);
    const auto [parsedEnd, parseError] =
        std::from_chars(value.data(), value.data() + value.size(), contentLength);
    if (parseError != std::errc{} || parsedEnd != value.data() + value.size()) {
        return head;
    }

    const auto bufferedBodyBytes = input.size() < contentLength ? input.size() : contentLength;
    input.consume(bufferedBodyBytes);
    auto remaining = contentLength - bufferedBodyBytes;
    std::array<char, 4096> discard{};
    while (remaining != 0 && !error) {
        const auto chunk = remaining < discard.size() ? remaining : discard.size();
        const auto read = asio::read(socket, asio::buffer(discard.data(), chunk), error);
        remaining -= read;
    }
    return head;
}

void writeResponse(
    asio::ip::tcp::socket& socket, std::string_view body, std::string_view extraHeaders = {}) {
    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nConnection: close\r\n";
    response.append(extraHeaders);
    response.append("\r\n");
    response.append(body);
    std::error_code ignored;
    asio::write(socket, asio::buffer(response), ignored);
}

std::string gzipContent(std::string_view body) {
    auto encoded = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, body,
        {.maxEncodedBytes = body.size() + 1024, .resource = std::pmr::get_default_resource()});
    if (!encoded.encoded()) {
        throw std::runtime_error("failed to encode test gzip body");
    }
    const auto bytes = encoded.encoded()->bytes();
    return {bytes.data(), bytes.size()};
}

template <typename Exercise>
int runClient(
    const ruvia::HttpClientConfig& config, CountingResource& operationResource, Exercise exercise) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::detail::HttpClientConfigStorage stored(config, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(stored)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(),
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
    auto config = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    config.port = port;
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    return config;
}

template <typename Exercise>
int runRequestOnly(Exercise exercise) {
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    auto config = plainConfig(1);
    config.userAgent.clear();
    ruvia::detail::HttpClientConfigStorage stored(config, memory.resource());
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", memory.resource()), std::move(stored)};
    ruvia::detail::HttpClientRegistry registry(io, worker, memory.resource(),
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));

    FailSelectedLargeAllocationResource requestResource;
    int result = 0;
    {
        ruvia::detail::ScopedOperationScope scope;
        const auto client = registry.get(&requestResource, scope);
        result = exercise(client, requestResource);
        scope.close();
    }

    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int testOperationArena() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (!error) {
            writeResponse(socket, std::string(4096, 'r'));
        }
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource* resource) -> ruvia::Task<int> {
            const std::string headerValue(4096, 'h');
            const std::array headers{ruvia::HttpHeaderView{"x-arena", headerValue}};
            const auto before = resource->allocations();
            auto operation = client.send({
                .target = "/",
                .headers = headers,
            });
            if (resource->allocations() == before) {
                co_return 1;
            }
            auto response = co_await std::move(operation);
            if ((co_await response.body().readAll()).size() != 4096) {
                co_return 2;
            }
            co_return 0;
        });
}

int testResponseLimit() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (!error) {
            writeResponse(socket, std::string(128, 'x'));
        }
    });
    auto config = plainConfig(server.port());
    config.maxResponseBytes = 16;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                auto response = co_await client.send(request);
                (void)co_await response.body().readAll();
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
        if (error) {
            return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 103 Early Hints\r\n"
            "Connection: close\r\n"
            "\r\n";
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                (void)co_await client.send(request);
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
        if (error || request.find("te: gzip") == std::string::npos) {
            return;
        }
        std::array<char, 32> sizeBytes{};
        const auto [sizeEnd, sizeError] = std::to_chars(
            sizeBytes.data(), sizeBytes.data() + sizeBytes.size(), encoded.size(), 16);
        if (sizeError != std::errc{}) {
            return;
        }
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
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            const std::array headers{
                ruvia::HttpHeaderView{"Connection", "TE"},
                ruvia::HttpHeaderView{"TE", "gzip"},
            };
            auto response = co_await client.send({.headers = headers});
            co_return co_await response.body().readAll() == "decoded transfer body" ? 0 : 1;
        });
}

int testContentEncodedResponse() {
    const auto encoded = gzipContent("decoded content body");
    OneShotServer server([encoded](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) {
            return;
        }
        writeResponse(socket, encoded, "Content-Encoding: gzip\r\n");
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto response = co_await client.send(request);
            co_return co_await response.body().readAll() == "decoded content body" ? 0 : 1;
        });
}

int testContentEncodedResponseLimitAppliesAfterDecode() {
    const std::string decoded(4096, 'z');
    const auto encoded = gzipContent(decoded);
    OneShotServer server([encoded](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) {
            return;
        }
        writeResponse(socket, encoded, "Content-Encoding: gzip\r\n");
    });
    auto config = plainConfig(server.port());
    config.maxResponseBytes = encoded.size() + 8;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                (void)co_await client.send(request);
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kResponseTooLarge ? 0 : 2;
            }
            co_return 1;
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
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                const std::string requestBody(16 * 1024 * 1024, 'w');
                auto request = ruvia::HttpClientRequestView{
                    .method = "POST",
                    .target = "/",
                    .content = ruvia::HttpClientRequestContentView::bytes(requestBody),
                };
                (void)co_await client.send(request);
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kTimeout ? 0 : 2;
            }
            co_return 1;
        });
}

ruvia::Task<void> completeSlowRequest(const ruvia::HttpClientHandle& client, int& result) {
    try {
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        auto response = co_await client.send(request);
        result = co_await response.body().readAll() == "ok" ? 1 : -1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "slow negotiated request failed: %s\n", error.what());
        result = -1;
    }
}

ruvia::Task<void> timeOutQueuedRequest(const ruvia::HttpClientHandle& client, int& result) {
    try {
        auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        (void)co_await client.send(request);
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
        if (!error) {
            writeResponse(socket, "ok");
        }
    });
    auto config = plainConfig(server.port());
    config.protocol = ruvia::HttpClientProtocol::kNegotiate;
    config.acquireTimeout = 30ms;
    config.requestTimeout = 1s;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle& worker,
            CountingResource* resource) -> ruvia::Task<int> {
            int slow = 0;
            int queued = 0;
            ruvia::TaskScope requests(worker, {.resource = resource});
            requests.spawn(completeSlowRequest(client, slow));
            (void)co_await ruvia::sleepFor(worker, 10ms);
            requests.spawn(timeOutQueuedRequest(client, queued));
            co_await requests.join();
            if (slow != 1 || queued != 1) {
                std::fprintf(
                    stderr, "negotiated acquire results: slow=%d queued=%d\n", slow, queued);
            }
            co_return slow == 1 && queued == 1 ? 0 : 1;
        });
}

ruvia::Task<void> requestStopSoon(const ruvia::WorkerHandle& worker, ruvia::StopSource& source) {
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
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle& worker,
            CountingResource* resource) -> ruvia::Task<int> {
            ruvia::StopSource source;
            ruvia::TaskScope cancellation(worker, {.resource = resource});
            cancellation.spawn(requestStopSoon(worker, source));
            int result = 1;
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                (void)co_await client.withOptions({.stopToken = source.token()}).send(request);
            } catch (const ruvia::HttpClientError& error) {
                result = error.code() == ruvia::HttpClientError::Code::kCancelled ? 0 : 2;
            }
            co_await cancellation.join();
            co_return result;
        });
}

int testConnectStopTokenCancellation() {
    OneShotServer server([](asio::ip::tcp::socket&) { std::this_thread::sleep_for(200ms); });
    auto config = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1"};
    config.port = server.port();
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
    config.connectTimeout = 2s;
    config.requestTimeout = 2s;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle& worker,
            CountingResource* resource) -> ruvia::Task<int> {
            ruvia::StopSource source;
            ruvia::TaskScope cancellation(worker, {.resource = resource});
            cancellation.spawn(requestStopSoon(worker, source));
            int result = 1;
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                (void)co_await client.withOptions({.stopToken = source.token()}).send(request);
            } catch (const ruvia::HttpClientError& error) {
                result = error.code() == ruvia::HttpClientError::Code::kCancelled ? 0 : 2;
            }
            co_await cancellation.join();
            co_return result;
        });
}

int testCookieCapacity() {
    auto config = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1"};
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    config.maxCookies = 1;
    config.maxCookieBytes = 8;
    config.cookies.emplace_back("a", "1");
    ruvia::detail::validateHttpClientConfig(config);
    config.cookies.emplace_back("b", "2");
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        return 0;
    }
    return 1;
}

int testUserAgentConfigRejectsInvalidHeaderValue() {
    auto config = plainConfig(1);
    config.userAgent = "safe-agent";
    ruvia::detail::validateHttpClientConfig(config);
    config.userAgent = "Ruvia\r\nInjected: yes";
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        return 0;
    }
    return 1;
}

int testTlsPeerVerificationConfigRejectsInvalidPolicy() {
    auto config = plainConfig(1);
    config.tlsPeerVerification = static_cast<ruvia::TlsPeerVerificationPolicy>(0xFF);
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        return 0;
    }
    return 1;
}

int testReceivedCookieConfigRejectsInvalidPolicy() {
    auto config = plainConfig(1);
    config.receivedCookies = static_cast<ruvia::HttpClientReceivedCookiePolicy>(0xFF);
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        return 0;
    }
    return 1;
}

int testTcpSocketConfigRejectsInvalidPolicies() {
    auto config = plainConfig(1);
    config.tcpNoDelay = static_cast<ruvia::TcpNoDelayPolicy>(0xFF);
    bool rejectedNoDelay = false;
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        rejectedNoDelay = true;
    }
    if (!rejectedNoDelay) {
        return 1;
    }
    config.tcpNoDelay = ruvia::TcpNoDelayPolicy::kEnable;
    config.tcpKeepAlive = static_cast<ruvia::TcpKeepAlivePolicy>(0xFF);
    bool rejectedKeepAlive = false;
    try {
        ruvia::detail::validateHttpClientConfig(config);
    } catch (const std::invalid_argument&) {
        rejectedKeepAlive = true;
    }
    return rejectedKeepAlive ? 0 : 2;
}

int testOperationOptionsRejectNonpositiveTimeout() {
    auto rejectsInvalidArgument = [](auto action) {
        try {
            action();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto config = plainConfig(1);
    CountingResource operationResource;
    const auto scopedResult = runClient(config, operationResource,
        [rejectsInvalidArgument](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            if (!rejectsInvalidArgument(
                    [&client] { (void)client.withOptions({.timeout = 0ms}).send({}); })) {
                co_return 1;
            }
            if (!rejectsInvalidArgument(
                    [&client] { (void)client.withOptions({.timeout = -1ms}).send({}); })) {
                co_return 2;
            }
            co_return 0;
        });
    if (scopedResult != 0) {
        return scopedResult;
    }

    return 0;
}

int testAutomaticCookieCapacity() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(
                socket, "seeded", "Set-Cookie: a=1; Path=/\r\nSet-Cookie: b=2; Path=/\r\n");
            return;
        }
        const auto retainedFirst = head.find("cookie: a=1") != std::string::npos;
        const auto retainedSecond = head.find("b=2") != std::string::npos;
        writeResponse(socket, retainedFirst && !retainedSecond ? "bounded" : "leaked");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    config.maxCookies = 1;
    config.maxCookieBytes = 64;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "bounded" ? 0 : 2;
        });
}

int testAutomaticCookieInsertionFailureDoesNotRetainPartialCookie() {
    const std::string longPath = "/" + std::string(900, 'p');
    TwoShotServer server([longPath](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded", "Set-Cookie: bad=1; Path=" + longPath + "\r\n");
            return;
        }
        writeResponse(socket, head.find("bad=1") == std::string::npos ? "clean" : "leaked");
    });

    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);

    FailSelectedLargeAllocationResource poolResource;
    CountingResource operationResource;
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    config.maxCookieBytes = 64 * 1024;
    config.userAgent.clear();

    ruvia::detail::HttpClientConfigStorage stored(config, &poolResource);
    ruvia::detail::HttpClientDefinition definition{
        std::pmr::string("default", &poolResource), std::move(stored)};
    ruvia::detail::HttpClientRegistry registry(io, worker, &poolResource,
        std::span<const ruvia::detail::HttpClientDefinition>(&definition, 1));

    auto task = [&]() -> ruvia::Task<int> {
        ruvia::detail::ScopedOperationScope scope;
        auto poolClient = registry.get(&poolResource, scope);
        auto operationClient = registry.get(&operationResource, scope);

        auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        poolResource.failAllocationSizeRange(512, 2048);
        bool storageAllocationFailed = false;
        try {
            (void)co_await operationClient.send(first);
        } catch (const std::bad_alloc&) {
            // Expected: the cookie storage allocation failed after the response
            // was parsed. The jar must remain as if that Set-Cookie was ignored.
            storageAllocationFailed = true;
        }
        poolResource.disableFailures();

        auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
        auto secondResponse = co_await operationClient.send(second);
        scope.close();
        registry.closeNow();
        co_await registry.join();
        if (!storageAllocationFailed) {
            co_return 2;
        }
        co_return co_await secondResponse.body().readAll() == "clean" ? 0 : 1;
    };
    auto future = asio::co_spawn(io, ruvia::detail::taskAsAwaitable(task()), asio::use_future);
    io.run();
    const auto result = future.get();

    registry.closeNow();
    dispatcher->detachContext();
    return result;
}

int testCookieHostOnlyIdentity() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: sid=host-only; Path=/\r\n"
                "Set-Cookie: sid=domain; Domain=LOCALHOST; Path=/\r\n");
            return;
        }
        const auto hostOnly = head.find("sid=host-only");
        const auto domain = head.find("sid=domain");
        const bool distinct =
            hostOnly != std::string::npos && domain != std::string::npos && hostOnly < domain;
        writeResponse(socket, distinct ? "distinct" : "collapsed");
    });
    auto config = ruvia::HttpClientConfig{.scheme = ruvia::HttpScheme::kHttp, .host = "localhost"};
    config.port = server.port();
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "distinct" ? 0 : 2;
        });
}

int testCookiePathOrdering() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: sid=root; Path=/\r\n"
                "Set-Cookie: sid=account; Path=/account\r\n");
            return;
        }
        const auto account = head.find("sid=account");
        const auto root = head.find("sid=root");
        writeResponse(
            socket, account != std::string::npos && root != std::string::npos && account < root
                        ? "ordered"
                        : "misordered");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second =
                ruvia::HttpClientRequestView{.method = "GET", .target = "/account/profile"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "ordered" ? 0 : 2;
        });
}

int testLargeCookieMaxAge() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: long_lived=yes; Path=/; Max-Age=9223372036854775807\r\n");
            return;
        }
        writeResponse(socket,
            head.find("cookie: long_lived=yes") != std::string::npos ? "retained" : "expired");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "retained" ? 0 : 2;
        });
}

int testNamelessResponseCookie() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded", "Set-Cookie: nameless-value; Path=/\r\n");
            return;
        }
        writeResponse(socket, head.find("cookie: nameless-value\r\n") != std::string::npos
                                  ? "serialized"
                                  : "missing");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "serialized" ? 0 : 2;
        });
}

int testAutomaticCookieJarRejectsPairsThatCannotBeSerialized() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: bad name=1; Path=/\r\n"
                "Set-Cookie: spaced=bad value; Path=/\r\n"
                "Set-Cookie: quoted=\"good\"; Path=/\r\n"
                "Set-Cookie: good=ok; Path=/\r\n");
            return;
        }
        const bool keptSerializable = head.find("quoted=\"good\"") != std::string::npos &&
                                      head.find("good=ok") != std::string::npos;
        const bool droppedUnserializable = head.find("bad name=1") == std::string::npos &&
                                           head.find("spaced=bad value") == std::string::npos;
        writeResponse(socket, keptSerializable && droppedUnserializable ? "filtered" : "leaked");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "filtered" ? 0 : 2;
        });
}

int testFarFutureCookieExpires() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: future=yes; Path=/; Expires=Fri, 31 Dec 9999 23:59:59 GMT\r\n");
            return;
        }
        writeResponse(
            socket, head.find("cookie: future=yes") != std::string::npos ? "retained" : "expired");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "retained" ? 0 : 2;
        });
}

int testCookieStorageSecurityConstraints() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: __SeCuRe-named=bad; Path=/\r\n"
                "Set-Cookie: __SeCuRe-nameless; Path=/\r\n"
                "Set-Cookie: same_site=bad; Path=/; SameSite=None\r\n");
            return;
        }
        const bool rejected = head.find("__SeCuRe-named=bad") == std::string::npos &&
                              head.find("__SeCuRe-nameless") == std::string::npos &&
                              head.find("same_site=bad") == std::string::npos;
        writeResponse(socket, rejected ? "rejected" : "accepted");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "rejected" ? 0 : 2;
        });
}

int testIpCookieDomainSuffixRejection() {
    TwoShotServer server([](asio::ip::tcp::socket& socket, unsigned exchange) {
        std::error_code error;
        const auto head = readHead(socket, error);
        if (error) {
            return;
        }
        if (exchange == 0) {
            writeResponse(socket, "seeded",
                "Set-Cookie: suffix=bad; Domain=0.0.1; Path=/\r\n"
                "Set-Cookie: exact=good; Domain=127.0.0.1; Path=/\r\n");
            return;
        }
        const bool correct = head.find("suffix=bad") == std::string::npos &&
                             head.find("exact=good") != std::string::npos;
        writeResponse(socket, correct ? "restricted" : "leaked");
    });
    auto config = plainConfig(server.port());
    config.receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend;
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto first = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto firstResponse = co_await client.send(first);
            if (co_await firstResponse.body().readAll() != "seeded") {
                co_return 1;
            }
            auto second = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto secondResponse = co_await client.send(second);
            co_return co_await secondResponse.body().readAll() == "restricted" ? 0 : 2;
        });
}

int testHttp1ResponseTrailers() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) {
            return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Trailer: Server-Timing, X-Trace\r\n"
            "Connection: close\r\n"
            "\r\n"
            "3\r\nabc\r\n"
            "0\r\n"
            "Server-Timing: db;dur=4\r\n"
            "X-Trace: done\r\n"
            "\r\n";
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
            auto response = co_await client.send(request);
            if (co_await response.body().readAll() != "abc") {
                co_return 1;
            }
            if (response.trailer("server-timing") != std::optional<std::string_view>("db;dur=4")) {
                co_return 2;
            }
            co_return response.trailer("x-trace") == std::optional<std::string_view>("done") ? 0
                                                                                             : 3;
        });
}

int testHttp1ResponseTrailerRules() {
    {
        OneShotServer server([](asio::ip::tcp::socket& socket) {
            std::error_code error;
            (void)readHead(socket, error);
            if (error) {
                return;
            }
            constexpr std::string_view response =
                "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Trailer: Accept-Ranges\r\n"
                "Connection: close\r\n"
                "\r\n"
                "5\r\nhello\r\n"
                "0\r\n"
                "Accept-Ranges: bytes\r\n"
                "\r\n";
            asio::write(socket, asio::buffer(response), error);
        });
        auto config = plainConfig(server.port());
        CountingResource operationResource;
        const auto accepted = runClient(config, operationResource,
            [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
                CountingResource*) -> ruvia::Task<int> {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                auto response = co_await client.send(request);
                if (co_await response.body().readAll() != "hello") {
                    co_return 1;
                }
                co_return response.trailer("accept-ranges") ==
                               std::optional<std::string_view>("bytes")
                              ? 0
                              : 2;
            });
        if (accepted != 0) {
            return accepted;
        }
    }

    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) {
            return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Trailer: Date\r\n"
            "Connection: close\r\n"
            "\r\n"
            "5\r\nhello\r\n"
            "0\r\n"
            "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
            "\r\n";
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                auto request = ruvia::HttpClientRequestView{.method = "GET", .target = "/"};
                auto response = co_await client.send(request);
                (void)co_await response.body().readAll();
            } catch (const ruvia::HttpClientError& error) {
                co_return error.code() == ruvia::HttpClientError::Code::kProtocolError ? 0 : 3;
            }
            co_return 4;
        });
}

int testHttp1ImmediateBodyUpgradeMarksRequestComplete() {
    OneShotServer server([](asio::ip::tcp::socket& socket) {
        std::error_code error;
        (void)readHead(socket, error);
        if (error) {
            return;
        }
        constexpr std::string_view response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "\r\n";
        asio::write(socket, asio::buffer(response), error);
    });
    auto config = plainConfig(server.port());
    CountingResource operationResource;
    return runClient(config, operationResource,
        [](const ruvia::HttpClientHandle& client, const ruvia::WorkerHandle&,
            CountingResource*) -> ruvia::Task<int> {
            try {
                const std::array headers{
                    ruvia::HttpHeaderView{"Connection", "Upgrade"},
                    ruvia::HttpHeaderView{"Upgrade", "websocket"},
                };
                (void)co_await client.send({
                    .method = "POST",
                    .headers = headers,
                    .content = ruvia::HttpClientRequestContentView::bytes("payload"),
                });
            } catch (const ruvia::HttpClientError& error) {
                if (error.code() != ruvia::HttpClientError::Code::kProtocolError) {
                    co_return 1;
                }
                const std::string_view message(error.what());
                if (message ==
                    "HTTP tunnel and protocol upgrade responses require a dedicated API") {
                    co_return 0;
                }
                if (message == "invalid Switching Protocols response") {
                    co_return 2;
                }
                co_return 3;
            }
            co_return 4;
        });
}

}  // namespace

int main() {
    try {
        const std::array<std::pair<int (*)(), std::string_view>, 29> checks{{
            {&testOperationArena, "operation arena"},
            {&testResponseLimit, "response limit"},
            {&testClosingInformationalResponse, "closing informational response"},
            {&testTransferCodedResponse, "transfer-coded response"},
            {&testContentEncodedResponse, "content-encoded response"},
            {&testContentEncodedResponseLimitAppliesAfterDecode,
                "content-encoded response decoded limit"},
            {&testWriteTimeout, "HTTP/1 write timeout"},
            {&testNegotiatedHttp1AcquireTimeout, "negotiated HTTP/1 acquire timeout"},
            {&testStopTokenCancellation, "stop-token cancellation"},
            {&testConnectStopTokenCancellation, "connect stop-token cancellation"},
            {&testCookieCapacity, "cookie capacity"},
            {&testUserAgentConfigRejectsInvalidHeaderValue, "user-agent configuration validation"},
            {&testTlsPeerVerificationConfigRejectsInvalidPolicy,
                "TLS peer verification policy validation"},
            {&testReceivedCookieConfigRejectsInvalidPolicy, "received cookie policy validation"},
            {&testTcpSocketConfigRejectsInvalidPolicies, "TCP socket policy validation"},
            {&testOperationOptionsRejectNonpositiveTimeout, "operation timeout validation"},
            {&testAutomaticCookieCapacity, "automatic cookie capacity"},
            {&testAutomaticCookieInsertionFailureDoesNotRetainPartialCookie,
                "automatic cookie insertion failure rollback"},
            {&testCookieHostOnlyIdentity, "cookie host-only identity"},
            {&testCookiePathOrdering, "cookie path ordering"},
            {&testLargeCookieMaxAge, "large cookie Max-Age"},
            {&testNamelessResponseCookie, "nameless response cookie"},
            {&testAutomaticCookieJarRejectsPairsThatCannotBeSerialized,
                "automatic cookie serialization filter"},
            {&testFarFutureCookieExpires, "far-future cookie Expires"},
            {&testCookieStorageSecurityConstraints, "cookie storage security constraints"},
            {&testIpCookieDomainSuffixRejection, "IP cookie domain suffix rejection"},
            {&testHttp1ResponseTrailers, "HTTP/1 response trailers"},
            {&testHttp1ResponseTrailerRules, "HTTP/1 response trailer rules"},
            {&testHttp1ImmediateBodyUpgradeMarksRequestComplete,
                "HTTP/1 immediate body upgrade completion"},
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
