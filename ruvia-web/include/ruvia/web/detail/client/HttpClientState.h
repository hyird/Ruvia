#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <vector>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/HttpClient.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

namespace ruvia::detail {

class HttpClientState final : public std::enable_shared_from_this<HttpClientState> {
public:
    HttpClientState(EventLoop loop, HttpClientConfig config);
    ~HttpClientState();

    HttpClientState(const HttpClientState&) = delete;
    HttpClientState& operator=(const HttpClientState&) = delete;

    void bindStop();
    void requestClose() noexcept;

    [[nodiscard]] HttpClientHandle handle(OperationOptions options);
    [[nodiscard]] HttpClientStats stats();
    [[nodiscard]] std::string_view host();
    [[nodiscard]] std::uint16_t port();
    [[nodiscard]] HttpScheme scheme();

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

private:
    enum class Phase : unsigned char {
        kOpen,
        kClosing,
        kClosed,
    };

    [[nodiscard]] static EventLoop requireLoop(EventLoop loop);
    [[nodiscard]] static std::pmr::vector<HttpClientDefinition> makeDefinitions(
        const HttpClientConfig& config, std::pmr::memory_resource* resource);

    void requireOpenOnWorker() const;
    void startCloseOnWorker() noexcept;
    [[nodiscard]] Task<void> closeOnWorker();
    void finishClose(TaskCompletionResult<void> result);

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    std::pmr::vector<HttpClientDefinition> definitions_;
    HttpClientRegistry clients_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    std::atomic<Phase> phase_{Phase::kOpen};
    bool closeTaskStarted_{false};
    // Declared last so operations expire before their pools and worker memory.
    ScopedOperationScope operationScope_;
};

}  // namespace ruvia::detail
