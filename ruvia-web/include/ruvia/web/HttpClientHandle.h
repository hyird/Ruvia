#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/TcpSocketOptions.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientRegistry;
class HttpClientRequestStorage;
class HttpClientResponseState;
struct HttpClientRequestStorageAccess;
}

class Context;
class HttpClientHandle;
class ResponseStreamWriter;
class WorkerHandle;

enum class HttpClientProtocol : std::uint8_t {
    kNegotiate,
    kHttp1Only,
    kHttp2Only,
};

enum class HttpClientTlsPeerVerificationPolicy : std::uint8_t {
    kVerify,
    kSkipVerification,
};

enum class HttpClientReceivedCookiePolicy : std::uint8_t {
    kIgnore,
    kRetainAndSend,
};

struct HttpClientConfig final {
    HttpScheme scheme{HttpScheme::kHttps};
    std::string host;
    std::optional<std::uint16_t> port;
    std::size_t connectionsPerWorker{1};
    std::size_t maxConcurrentHttp2StreamsPerConnection{100};
    std::size_t maxBufferedRequestsPerWorker{1024};
    std::size_t maxCookiesPerWorker{256};
    std::size_t maxCookieBytesPerWorker{32 * 1024};
    std::chrono::milliseconds connectTimeout{5000};
    std::optional<std::chrono::milliseconds> writeTimeout{30000};
    std::optional<std::chrono::milliseconds> requestTimeout{30000};
    std::optional<std::chrono::milliseconds> acquireTimeout{5000};
    std::size_t maxResponseBytes{16 * 1024 * 1024};
    HttpClientProtocol protocol{HttpClientProtocol::kNegotiate};
    HttpClientTlsPeerVerificationPolicy tlsPeerVerification{HttpClientTlsPeerVerificationPolicy::kVerify};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kEnable};
    HttpClientReceivedCookiePolicy receivedCookies{HttpClientReceivedCookiePolicy::kIgnore};
    std::string caFile;
    std::string certificateChainFile;
    std::string privateKeyFile;
    std::string privateKeyPassword;
    std::string userAgent{"Ruvia"};
    std::vector<std::pair<std::string, std::string>> cookies;
};

class HttpClientError final : public std::runtime_error {
public:
    enum class Code : std::uint8_t {
        kNotConfigured,
        kInvalidRequest,
        kTimeout,
        kCancelled,
        kResolveFailed,
        kConnectFailed,
        kTlsFailed,
        kProtocolUnavailable,
        kIoError,
        kProtocolError,
        kResponseTooLarge,
        kQueueFull,
        kClosing,
    };

    HttpClientError(Code code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] Code code() const noexcept { return code_; }

private:
    Code code_;
};

namespace detail {

class HttpClientRequestStorage final {
public:
    HttpClientRequestStorage(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage& operator=(const HttpClientRequestStorage&) = delete;
    HttpClientRequestStorage(HttpClientRequestStorage&&) noexcept = default;
    HttpClientRequestStorage& operator=(HttpClientRequestStorage&&) noexcept = default;

    HttpClientRequestStorage& appendHeader(std::string_view name, std::string_view value);
    HttpClientRequestStorage& setBody(std::string_view body);

    [[nodiscard]] std::string_view method() const& noexcept { return method_; }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view target() const& noexcept { return target_; }
    [[nodiscard]] std::string_view target() const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept { return body_; }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend class ::ruvia::HttpClientHandle;
    friend class HttpClientPool;
    friend struct HttpClientRequestStorageAccess;

    struct Header final {
        Header(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource), value(value, resource) {}
        std::pmr::string name;
        std::pmr::string value;
    };

    HttpClientRequestStorage(
        std::string_view method,
        std::string_view target,
        std::pmr::memory_resource* resource);

    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<Header> headers_;
    std::pmr::string body_;
    bool hasBody_{false};
};

}  // namespace detail

class HttpClientResponseBody final {
public:
    HttpClientResponseBody(const HttpClientResponseBody&) = delete;
    HttpClientResponseBody& operator=(const HttpClientResponseBody&) = delete;
    HttpClientResponseBody(HttpClientResponseBody&& other) noexcept;
    HttpClientResponseBody& operator=(HttpClientResponseBody&& other) noexcept;
    ~HttpClientResponseBody();

    // The returned view remains valid until the next body operation. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] ScopedOperation<std::optional<std::string_view>> read();

    // Collects the unread remainder of this same stream. maxBytes is a caller
    // bound in addition to the origin's transport bound.
    [[nodiscard]] ScopedOperation<std::pmr::string> readAll(
        std::size_t maxBytes = 16 * 1024 * 1024);

    // Copies this same stream into a controller response stream with natural
    // backpressure. This is the common forwarding path for both small and
    // long-lived upstream responses.
    [[nodiscard]] ScopedOperation<void> pipeTo(ResponseStreamWriter& output);

    [[nodiscard]] bool complete() const noexcept;

private:
    friend class HttpClientResponse;
    friend class detail::HttpClientPool;

    [[nodiscard]] Task<std::optional<std::string_view>> readTask();
    [[nodiscard]] Task<std::pmr::string> readAllTask(std::size_t maxBytes);
    [[nodiscard]] Task<void> pipeToTask(ResponseStreamWriter& output);

    explicit HttpClientResponseBody(detail::HttpClientResponseState* state) noexcept
        : state_(state) {}

    detail::HttpClientResponseState* state_{nullptr};
    bool readActive_{false};
    detail::ScopedOperationScope operationScope_;
};

class HttpClientResponse final {
public:
    HttpClientResponse(const HttpClientResponse&) = delete;
    HttpClientResponse& operator=(const HttpClientResponse&) = delete;
    HttpClientResponse(HttpClientResponse&& other) noexcept;
    HttpClientResponse& operator=(HttpClientResponse&& other) noexcept;
    ~HttpClientResponse();

    [[nodiscard]] HttpStatusCode status() const noexcept;
    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const& noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const&& = delete;
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const& noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] HttpClientResponseBody& body() & noexcept { return body_; }
    [[nodiscard]] const HttpClientResponseBody& body() const& = delete;
    [[nodiscard]] HttpClientResponseBody& body() && = delete;

private:
    friend class detail::HttpClientPool;

    HttpClientResponse(
        std::pmr::memory_resource* resource,
        const WorkerHandle& worker,
        detail::HttpClientPool& pool);
    HttpClientResponse(detail::HttpClientResponseState* state, bool retain) noexcept;
    void release() noexcept;

    detail::HttpClientResponseState* state_{nullptr};
    HttpClientResponseBody body_{state_};
    bool consumer_{true};
};

struct HttpClientStats {
    std::size_t bufferedRequests{0};
    std::size_t inFlightRequests{0};
    std::size_t completedRequests{0};
    std::size_t failedRequests{0};
    std::size_t bytesSent{0};
    std::size_t bytesReceived{0};
};

class HttpClientHandle final : private detail::ScopedCapabilityNode {
public:
    HttpClientHandle(const HttpClientHandle& other);
    HttpClientHandle& operator=(const HttpClientHandle&) = delete;

    [[nodiscard]] HttpClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<HttpClientResponse> send(const HttpClientRequestView& request) const;
    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] HttpScheme scheme() const;
private:
    friend class detail::HttpClientRegistry;
    friend class Context;
    friend class WebWorkerContext;
    HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    detail::HttpClientPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    OperationOptions options_;
};

}  // namespace ruvia
