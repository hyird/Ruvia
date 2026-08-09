#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/StopToken.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/ScopedOperation.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientRegistry;
struct HttpClientHeaderAccess;
struct HttpClientRequestAccess;
}

class Context;
class HttpClient;
using HttpClientPtr = std::shared_ptr<HttpClient>;

enum class HttpClientProtocol : std::uint8_t {
    kNegotiate,
    kHttp1Only,
    kHttp2Only,
};

struct HttpClientConfig {
    std::string host;
    HttpScheme scheme{HttpScheme::kHttps};
    std::uint16_t port{0};
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
    bool verifyCertificate{true};
    bool tcpNoDelay{true};
    bool keepAlive{true};
    bool cookiesEnabled{false};
    std::string caFile;
    std::string certificateChainFile;
    std::string privateKeyFile;
    std::string privateKeyPassword;
    std::string userAgent{"Ruvia"};
    std::vector<std::pair<std::string, std::string>> cookies;
};

struct HttpClientOperationOptions {
    std::optional<std::chrono::milliseconds> timeout;
    StopToken stopToken;
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

class HttpClientRequest final {
public:
    HttpClientRequest(const HttpClientRequest&) = delete;
    HttpClientRequest& operator=(const HttpClientRequest&) = delete;
    HttpClientRequest(HttpClientRequest&&) noexcept = default;
    HttpClientRequest& operator=(HttpClientRequest&&) noexcept = default;

    HttpClientRequest& setMethod(HttpKnownMethod method);
    HttpClientRequest& setMethod(std::string_view method);
    HttpClientRequest& setPath(std::string_view path);
    HttpClientRequest& addHeader(std::string_view name, std::string_view value);
    HttpClientRequest& removeHeader(std::string_view name);
    HttpClientRequest& setContentTypeString(std::string_view contentType);
    HttpClientRequest& addCookie(std::string_view name, std::string_view value);
    HttpClientRequest& setBody(std::string_view body);
    HttpClientRequest& setBody(std::span<const std::byte> body);
    HttpClientRequest& clearBody() noexcept;

    [[nodiscard]] std::string_view method() const& noexcept { return method_; }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view path() const& noexcept { return path_; }
    [[nodiscard]] std::string_view path() const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept { return body_; }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend class HttpClient;
    friend struct detail::HttpClientRequestAccess;

    struct Header final {
        Header(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource), value(value, resource) {}
        std::pmr::string name;
        std::pmr::string value;
    };

    explicit HttpClientRequest(std::pmr::memory_resource* resource);

    std::pmr::string method_;
    std::pmr::string path_;
    std::pmr::vector<Header> headers_;
    std::pmr::string body_;
    bool hasBody_{false};
};

class HttpClientHeader final {
public:
    [[nodiscard]] std::string_view name() const& noexcept { return name_; }
    [[nodiscard]] std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept { return value_; }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::HttpClientHeaderAccess;
    HttpClientHeader(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
        : name_(name, resource), value_(value, resource) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

class HttpClientResponse final {
public:
    HttpClientResponse(const HttpClientResponse&) = delete;
    HttpClientResponse& operator=(const HttpClientResponse&) = delete;
    HttpClientResponse(HttpClientResponse&&) noexcept = default;
    HttpClientResponse& operator=(HttpClientResponse&&) noexcept = default;

    [[nodiscard]] HttpStatusCode statusCode() const noexcept { return status_; }
    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept { return protocolVersion_; }
    [[nodiscard]] std::span<const HttpClientHeader> headers() const& noexcept { return headers_; }
    [[nodiscard]] std::span<const HttpClientHeader> headers() const&& = delete;
    [[nodiscard]] std::span<const HttpClientHeader> trailers() const& noexcept { return trailers_; }
    [[nodiscard]] std::span<const HttpClientHeader> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> getHeader(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> getHeader(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> getTrailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> getTrailer(std::string_view) const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept { return body_; }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend class detail::HttpClientPool;

    explicit HttpClientResponse(std::pmr::memory_resource* resource = nullptr);

    HttpStatusCode status_{http_status::kOk};
    HttpProtocolVersion protocolVersion_{HttpProtocolVersion::kHttp11};
    std::pmr::vector<HttpClientHeader> headers_;
    std::pmr::vector<HttpClientHeader> trailers_;
    std::pmr::string body_;
};

struct HttpClientStats {
    std::size_t requestsBuffered{0};
    std::size_t requestsInFlight{0};
    std::size_t completedRequests{0};
    std::size_t failedRequests{0};
    std::size_t bytesSent{0};
    std::size_t bytesReceived{0};
};

class HttpClient final : private detail::ScopedCapabilityNode {
public:
    HttpClient(const HttpClient& other);
    HttpClient& operator=(const HttpClient&) = delete;

    [[nodiscard]] static HttpClientPtr newHttpClient(std::string_view origin, HttpClientConfig config = {});
    [[nodiscard]] HttpClientRequest newRequest() const;
    [[nodiscard]] HttpClient withOptions(HttpClientOperationOptions options) const;
    [[nodiscard]] Task<HttpClientResponse> sendRequest(HttpClientRequest request, HttpClientOperationOptions options = {}) const;
    [[nodiscard]] Task<HttpClientResponse> sendRequest(HttpClientRequest request, std::chrono::milliseconds timeout) const;
    [[nodiscard]] HttpClientStats stats() const noexcept;
    [[nodiscard]] std::size_t requestsBufferSize() const noexcept;
    [[nodiscard]] std::size_t outstandingRequests() const noexcept;
    [[nodiscard]] std::size_t bytesSent() const noexcept;
    [[nodiscard]] std::size_t bytesReceived() const noexcept;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] bool secure() const;
    [[nodiscard]] bool onDefaultPort() const;
    void setUserAgent(std::string_view userAgent) const;
    void enableCookies(bool enabled = true) const;
    void addCookie(std::string_view name, std::string_view value) const;

private:
    friend class detail::HttpClientRegistry;
    HttpClient(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept;
    HttpClient(std::uint64_t dynamicId, HttpClientConfig config);
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
    [[nodiscard]] detail::HttpClientPool& resolvePool() const;

    detail::HttpClientPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    HttpClientOperationOptions options_;
    std::uint64_t dynamicId_{0};
    mutable HttpClientConfig dynamicConfig_;
};

}  // namespace ruvia
