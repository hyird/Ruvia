#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/HttpClient.h"

#include "client/HttpClientRegistry.h"
#include "client/client_lifecycle.h"

namespace ruvia::detail {

class HttpClientState final : public std::enable_shared_from_this<HttpClientState> {
public:
    HttpClientState(EventLoop loop, const HttpClientConfig& config,
        HttpClientResultBudgetConfig resultBudget);

    HttpClientState(const HttpClientState&) = delete;
    HttpClientState& operator=(const HttpClientState&) = delete;

    void bindStop() {
        lifecycle_.bind_stop();
    }
    void requestClose() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] Task<void> shutdown() {
        return lifecycle_.shutdown();
    }

    [[nodiscard]] HttpClientHandle handle(OperationOptions options);
    [[nodiscard]] HttpClientStats stats();
    [[nodiscard]] std::string_view host();
    [[nodiscard]] std::uint16_t port();
    [[nodiscard]] HttpScheme scheme();

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<HttpClientState>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] HttpClientRegistry& backend() noexcept {
        return clients_;
    }

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    HttpClientRegistry clients_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<HttpClientState> lifecycle_;
};

}  // namespace ruvia::detail
