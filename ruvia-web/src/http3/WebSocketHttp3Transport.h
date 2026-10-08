#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http3Connection.h"

#include "http3/Http3QuicClientEndpointResolver.h"
#include "http3/Http3QuicClientSocketSession.h"

namespace ruvia::detail {
class WebSocketClientState;

// A single worker-affine QUIC pump owns DNS, critical streams and one Extended
// CONNECT stream. Application operations publish bounded owned blocks and wake
// that pump. SSL WANT retries retain the original block until accepted or close.
class WebSocketHttp3Transport final {
public:
    WebSocketHttp3Transport(WebSocketClientState& owner, const WorkerHandle& worker,
        std::pmr::memory_resource* resource);
    [[nodiscard]] Task<void> connect();
    [[nodiscard]] Task<std::size_t> read(std::span<char> output);
    [[nodiscard]] Task<void> write(std::string_view bytes);
    [[nodiscard]] Task<void> finish();
    void stop() noexcept;
    [[nodiscard]] Task<void> join();

private:
    [[nodiscard]] Task<void> drive();
    [[nodiscard]] Task<void> waitForOutput();
    [[nodiscard]] bool receive(std::uint64_t id, bool request);
    [[nodiscard]] bool driveOutput();
    void checkFailure() const;
    void wake() noexcept;
    void prepareFrame(std::uint64_t type, std::span<const char> payload);
    static void onEvent(void* context, const Http3ConnectionEvent& event);

    WebSocketClientState& owner_;
    std::pmr::memory_resource* resource_;
    http3_quic_client_tls_context tls_;
    Http3QuicClientEndpointResolver resolver_;
    ruvia::Http3Connection connection_;
    TaskScope drivers_;
    WorkerSignal progress_;
    std::optional<Http3QuicClientSocketSession> session_{};
    std::pmr::vector<std::uint64_t> peerStreams_;
    std::pmr::string received_;
    std::size_t readOffset_{0};
    std::pmr::string outbound_;
    std::size_t writeOffset_{0};
    std::pmr::string blockedInput_;
    std::array<std::pmr::string, 2> criticalOutput_;
    std::array<std::size_t, 2> criticalOffset_{};
    std::array<WorkerTimerRegistration, 2> criticalTimers_{};
    std::optional<Http3MessageHead> response_{};
    std::exception_ptr failure_{};
    std::optional<std::uint64_t> streamId_{};
    bool openRequested_{false};
    bool finishRequested_{false};
    bool finished_{false};
    bool finPrepared_{false};
    bool qpackBlocked_{false};
    bool blockedFin_{false};
    bool eof_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
