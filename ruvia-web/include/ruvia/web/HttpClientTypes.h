#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/TlsPeerVerification.h"

namespace ruvia {

enum class HttpClientProtocol : std::uint8_t {
    kNegotiate,
    kHttp1Only,
    kHttp2Only,
};

enum class HttpClientReceivedCookiePolicy : std::uint8_t {
    kIgnore,
    kRetainAndSend,
};

// Configuration for one HttpClient bound to one EventLoop. App registration
// creates one such client per Web worker, so these limits remain per client
// without exposing the App deployment model in the standalone API.
struct HttpClientConfig final {
    HttpScheme scheme{HttpScheme::kHttps};
    std::string host{};
    std::optional<std::uint16_t> port{};
    std::size_t connectionCount{1};
    std::size_t maxConcurrentHttp2StreamsPerConnection{100};
    std::size_t maxBufferedRequests{1024};
    std::size_t maxCookies{256};
    std::size_t maxCookieBytes{std::size_t{32} * 1024};
    std::chrono::milliseconds connectTimeout{5000};
    std::optional<std::chrono::milliseconds> writeTimeout{30000};
    std::optional<std::chrono::milliseconds> requestTimeout{30000};
    std::optional<std::chrono::milliseconds> acquireTimeout{5000};
    std::size_t maxResponseBytes{kDefaultMaxBufferedBodyBytes};
    HttpClientProtocol protocol{HttpClientProtocol::kNegotiate};
    TlsPeerVerificationPolicy tlsPeerVerification{TlsPeerVerificationPolicy::kVerify};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kEnable};
    HttpClientReceivedCookiePolicy receivedCookies{HttpClientReceivedCookiePolicy::kIgnore};
    std::string caFile{};
    std::string certificateChainFile{};
    std::string privateKeyFile{};
    std::string privateKeyPassword{};
    std::string userAgent{"Ruvia"};
    std::vector<std::pair<std::string, std::string>> cookies{};
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

    HttpClientError(Code code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] Code code() const noexcept {
        return code_;
    }

private:
    Code code_;
};

struct HttpClientStats final {
    std::size_t bufferedRequests{0};
    std::size_t inFlightRequests{0};
    std::size_t completedRequests{0};
    std::size_t failedRequests{0};
    std::size_t bytesSent{0};
    std::size_t bytesReceived{0};
};

}  // namespace ruvia
