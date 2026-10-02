#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"

namespace ruvia::detail {
class WebSocketClientState;

// One worker owns the connection and its sole socket reader/writer. Received
// DATA retains its linear credit until the application consumes the buffer.
class WebSocketHttp2Transport final {
public:
    WebSocketHttp2Transport(WebSocketClientState& owner, const WorkerHandle& worker,
        std::pmr::memory_resource* resource);
    [[nodiscard]] Task<void> connect();
    [[nodiscard]] Task<std::size_t> read(std::span<char> output);
    [[nodiscard]] Task<void> write(std::string_view bytes);
    [[nodiscard]] Task<void> finish();
    void stop() noexcept;
    [[nodiscard]] Task<void> join();

private:
    [[nodiscard]] Task<void> runReader();
    [[nodiscard]] Task<void> runWriter();
    [[nodiscard]] Task<void> flush();
    void drainEvents();
    void checkFailure() const;
    void fail(std::exception_ptr failure) noexcept;

    WebSocketClientState& owner_;
    ruvia::Http2Connection connection_;
    TaskScope drivers_;
    WorkerSignal progress_;
    WorkerSignal writerWake_;
    std::pmr::string received_;
    std::size_t readOffset_{0};
    std::optional<Http2ReceivedDataCredit> receivedCredit_{};
    std::optional<HttpClientResponseHead> response_{};
    std::exception_ptr failure_{};
    std::uint32_t streamId_{0};
    bool writerActive_{false};
    bool eof_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
