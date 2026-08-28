#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/WebSocketClient.h"
#include "ruvia/web/detail/client/WebSocketClientConfigStorage.h"

namespace ruvia {
class Http1ParsedClientResponseHead;
}

namespace ruvia::detail {

class WebSocketClientState final : public std::enable_shared_from_this<WebSocketClientState> {
public:
    WebSocketClientState(EventLoop loop, const WebSocketClientConfig& config);
    ~WebSocketClientState();

    void bindStop();
    [[nodiscard]] Task<void> connect();
    [[nodiscard]] WebSocketClientHandle handle(OperationOptions options);
    void requestClose() noexcept;
    void requestCancel() noexcept;
    [[nodiscard]] bool connected();
    [[nodiscard]] std::string_view subprotocol();
    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read(OperationOptions options);
    [[nodiscard]] ScopedOperation<void> write(WebSocketOpcode opcode, std::string_view payload, OperationOptions options);
    [[nodiscard]] ScopedOperation<void> close(WebSocketCloseOptions options, OperationOptions operationOptions);

private:
    enum class Phase : std::uint8_t { kFresh, kConnecting, kOpen, kClosing, kClosed };
    enum class AbortReason : std::uint8_t { kNone, kTimeout, kCancelled, kClosing };

    [[nodiscard]] static Task<void> connectOwned(std::shared_ptr<WebSocketClientState> state);
    [[nodiscard]] static Task<std::optional<WebSocketMessage>> readOwned(std::shared_ptr<WebSocketClientState> state, OperationOptions options);
    [[nodiscard]] static Task<void> writeOwned(std::shared_ptr<WebSocketClientState> state, WebSocketOpcode opcode, std::pmr::string payload, OperationOptions options);
    [[nodiscard]] static Task<void> closeOwned(std::shared_ptr<WebSocketClientState> state, WebSocketCloseOptions options, std::pmr::string reason, OperationOptions operationOptions);

    void requireCurrent() const;
    void requireOpen() const;
    [[nodiscard]] WsConnection& requireProtocol() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void closeOnWorker(AbortReason reason) noexcept;
    void requestAbort(AbortReason reason) noexcept;
    [[nodiscard]] Task<void> establishTransport();
    [[nodiscard]] Task<void> performTlsHandshake();
    void validateHandshakeResponse(const Http1ParsedClientResponseHead& response, std::string_view key);
    [[nodiscard]] Task<void> flushOutput(OperationOptions options);
    [[nodiscard]] Task<std::size_t> readTransport(std::span<char> output, OperationOptions options, std::optional<std::chrono::milliseconds> configuredTimeout);
    [[nodiscard]] Task<void> writeTransport(std::string_view bytes, OperationOptions options, std::optional<std::chrono::milliseconds> configuredTimeout);
    [[nodiscard]] Task<void> performHandshake(OperationOptions options);
    [[nodiscard]] std::optional<std::chrono::milliseconds> effectiveTimeout(const OperationOptions& options, std::optional<std::chrono::milliseconds> configured) const;
    void arm(asio::steady_timer& timer, std::optional<std::chrono::milliseconds> timeout, AbortReason reason);
    void disarm(asio::steady_timer& timer) noexcept;
    void throwAbort() const;
    [[nodiscard]] static bool generateMask(void*, WsMaskKey& key) noexcept;

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    WebSocketClientConfigStorage config_;
    asio::ssl::context tlsContext_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    asio::steady_timer connectTimer_;
    asio::steady_timer readTimer_;
    asio::steady_timer writeTimer_;
    std::pmr::string input_;
    std::optional<WsConnection> protocol_;
    std::pmr::string selectedSubprotocol_;
    ScopedOperationScope operationScope_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    std::atomic<Phase> phase_{Phase::kFresh};
    AbortReason abortReason_{AbortReason::kNone};
    bool readActive_{false};
    bool writeActive_{false};
};

}  // namespace ruvia::detail
