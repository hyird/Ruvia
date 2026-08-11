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
#include "ruvia/web/OperationOptions.h"
#include "ruvia/web/ScopedOperation.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientRegistry;
struct HttpClientRequestAccess;
}

class Context;
class HttpClientHandle;

enum class HttpClientProtocol : std::uint8_t {
    kNegotiate,
    kHttp1Only,
    kHttp2Only,
};

class HttpClientConfig final {
public:
    [[nodiscard]] static HttpClientConfig http(std::string_view host) {
        return HttpClientConfig(host, HttpScheme::kHttp, 80);
    }

    [[nodiscard]] static HttpClientConfig https(std::string_view host) {
        return HttpClientConfig(host, HttpScheme::kHttps, 443);
    }

    [[nodiscard]] std::string_view host() const& noexcept {
        return host_;
    }
    std::string_view host() const&& = delete;

    [[nodiscard]] HttpScheme scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    HttpClientConfig& setPort(std::uint16_t port) {
        if (port == 0) {
            throw std::invalid_argument("http client port must be greater than zero");
        }
        port_ = port;
        return *this;
    }

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

private:
    HttpClientConfig(std::string_view host, HttpScheme scheme, std::uint16_t port)
        : host_(host), scheme_(scheme), port_(port) {}

    std::string host_;
    HttpScheme scheme_;
    std::uint16_t port_;
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

    HttpClientRequest& setHeader(std::string_view name, std::string_view value);
    HttpClientRequest& appendHeader(std::string_view name, std::string_view value);
    HttpClientRequest& removeHeader(std::string_view name);
    HttpClientRequest& setContentType(std::string_view contentType);
    HttpClientRequest& addCookie(std::string_view name, std::string_view value);
    HttpClientRequest& setBody(std::string_view body);
    HttpClientRequest& setBody(std::span<const std::byte> body);
    HttpClientRequest& clearBody() noexcept;

    [[nodiscard]] std::string_view method() const& noexcept { return method_; }
    [[nodiscard]] std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view target() const& noexcept { return target_; }
    [[nodiscard]] std::string_view target() const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept { return body_; }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend class HttpClientHandle;
    friend struct detail::HttpClientRequestAccess;

    struct Header final {
        Header(std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource), value(value, resource) {}
        std::pmr::string name;
        std::pmr::string value;
    };

    HttpClientRequest(
        std::string_view method,
        std::string_view target,
        std::pmr::memory_resource* resource);

    std::pmr::string method_;
    std::pmr::string target_;
    std::pmr::vector<Header> headers_;
    std::pmr::string body_;
    bool hasBody_{false};
};

class HttpClientResponse final {
public:
    HttpClientResponse(const HttpClientResponse&) = delete;
    HttpClientResponse& operator=(const HttpClientResponse&) = delete;
    HttpClientResponse(HttpClientResponse&&) noexcept = default;
    HttpClientResponse& operator=(HttpClientResponse&&) noexcept = default;

    [[nodiscard]] HttpStatusCode status() const noexcept { return status_; }
    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept { return protocolVersion_; }
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const& noexcept { return headers_; }
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const&& = delete;
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const& noexcept { return trailers_; }
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] std::string_view body() const& noexcept { return body_; }
    [[nodiscard]] std::string_view body() const&& = delete;

private:
    friend class detail::HttpClientPool;

    explicit HttpClientResponse(std::pmr::memory_resource* resource = nullptr);

    HttpStatusCode status_{http_status::kOk};
    HttpProtocolVersion protocolVersion_{HttpProtocolVersion::kHttp11};
    std::pmr::vector<HttpClientResponseHeader> headers_;
    std::pmr::vector<HttpClientResponseHeader> trailers_;
    std::pmr::string body_;
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

    [[nodiscard]] HttpClientRequest newRequest(HttpKnownMethod method, std::string_view target) const;
    [[nodiscard]] HttpClientRequest newRequest(std::string_view method, std::string_view target) const;
    [[nodiscard]] HttpClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<HttpClientResponse> sendRequest(HttpClientRequest request) const;
    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] HttpScheme scheme() const;
private:
    friend class detail::HttpClientRegistry;
    HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    detail::HttpClientPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    OperationOptions options_;
};

}  // namespace ruvia
