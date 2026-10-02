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
#include "ruvia/web/Http3QpackConfig.h"
#include "ruvia/web/HttpClientAdvertisementConfig.h"
#include "ruvia/web/HttpClientPushConfig.h"
#include "ruvia/web/TlsPeerVerification.h"

namespace ruvia {

enum class HttpClientProtocol : std::uint8_t {
    kNegotiate,
    kHttp1Only,
    kHttp2Only,
    // HTTP/3 uses QUIC over UDP and never falls back to the TCP pool.
    kHttp3Only,
};

enum class HttpClientReceivedCookiePolicy : std::uint8_t {
    kIgnore,
    kRetainAndSend,
};

// Bounds bytes retained by readAll() results: per standalone client or,
// in an App, across all registered aliases of one business worker. This is
// independent of per-response limits and worker-owned memory.
struct HttpClientResultBudgetConfig final {
    std::size_t maxRetainedBytes{std::size_t{64} * 1024 * 1024};
    // Worker-local live response allocations, including compressed input,
    // decoder output, metadata and simultaneous buffer growth.
    std::size_t max_in_flight_bytes{std::size_t{64} * 1024 * 1024};
};

// Configuration for one HttpClient bound to one EventLoop. App registration
// creates one such client per Web worker, so these limits remain per client
// without exposing the App deployment model in the standalone API.
struct HttpClientConfig final {
    HttpScheme scheme{HttpScheme::kHttps};
    // Validated unbracketed transport host; DNS names may retain one trailing dot.
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
    Http3QpackConfig qpack{};
    HttpClientAdvertisementConfig advertisements{};
    HttpClientPushConfig push{};
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
        kResultBudgetExceeded,
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
    std::size_t droppedAdvertisements{0};
    std::size_t receivedPushes{0};
    std::size_t rejectedPushes{0};
};

}  // namespace ruvia
