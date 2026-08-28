#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/web/TlsPeerVerification.h"
#include "ruvia/web/WebSocket.h"

namespace ruvia {

enum class WebSocketScheme : std::uint8_t {
    kWs,
    kWss,
};

struct WebSocketClientConfig final {
    WebSocketScheme scheme{WebSocketScheme::kWss};
    std::string host{};
    std::optional<std::uint16_t> port{};
    std::string target{"/"};
    std::vector<std::pair<std::string, std::string>> headers{};
    std::vector<std::string> subprotocols{};
    std::size_t maxMessageBytes{kDefaultMaxWebSocketMessageBytes};
    std::chrono::milliseconds connectTimeout{5000};
    std::optional<std::chrono::milliseconds> readTimeout{};
    std::optional<std::chrono::milliseconds> writeTimeout{30000};
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout{5000};
    TlsPeerVerificationPolicy tlsPeerVerification{TlsPeerVerificationPolicy::kVerify};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kEnable};
    std::string caFile{};
    std::string certificateChainFile{};
    std::string privateKeyFile{};
    std::string privateKeyPassword{};
    std::string userAgent{"Ruvia"};
};

class WebSocketClientError final : public std::runtime_error {
public:
    enum class Code : std::uint8_t {
        kInvalidConfig,
        kInvalidState,
        kTimeout,
        kCancelled,
        kResolveFailed,
        kConnectFailed,
        kTlsFailed,
        kHandshakeRejected,
        kIoError,
        kProtocolError,
        kMessageTooLarge,
        kClosing,
    };

    WebSocketClientError(Code code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] Code code() const noexcept {
        return code_;
    }

private:
    Code code_;
};

namespace detail {
class WebSocketClientState;
}

class WebSocketClientHandle final : private detail::ScopedCapabilityNode {
public:
    WebSocketClientHandle(const WebSocketClientHandle& other) noexcept;
    WebSocketClientHandle& operator=(const WebSocketClientHandle&) = delete;

    [[nodiscard]] WebSocketClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read() const;
    [[nodiscard]] ScopedOperation<void> text(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> binary(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> ping(std::string_view payload = {}) const;
    [[nodiscard]] ScopedOperation<void> pong(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> close(WebSocketCloseOptions options = {}) const;
    void abort() noexcept;

private:
    friend class detail::WebSocketClientState;
    WebSocketClientHandle(std::shared_ptr<detail::WebSocketClientState> state, detail::ScopedOperationScope& scope, OperationOptions options) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    std::shared_ptr<detail::WebSocketClientState> state_;
    OperationOptions options_;
};

// One WebSocket connection bound to one Ruvia event loop. Construction performs
// no I/O; connect() is lazy and must be started on the bound loop. The connection
// is worker-affine and never migrates between event loops.
class WebSocketClient final {
public:
    WebSocketClient(EventLoop loop, const WebSocketClientConfig& config);
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) = delete;
    WebSocketClient& operator=(WebSocketClient&&) = delete;

    [[nodiscard]] Task<void> connect();
    [[nodiscard]] WebSocketClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read() const;
    [[nodiscard]] ScopedOperation<void> text(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> binary(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> ping(std::string_view payload = {}) const;
    [[nodiscard]] ScopedOperation<void> pong(std::string_view payload) const;
    [[nodiscard]] ScopedOperation<void> close(WebSocketCloseOptions options) const;

    // Immediate lifecycle shutdown, matching HttpClient::close(). Graceful RFC
    // 6455 close uses the typed overload above: co_await client.close({...}).
    void close() noexcept;
    void abort() noexcept {
        close();
    }

    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::string_view subprotocol() const&;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    std::shared_ptr<detail::WebSocketClientState> state_;
};

}  // namespace ruvia
