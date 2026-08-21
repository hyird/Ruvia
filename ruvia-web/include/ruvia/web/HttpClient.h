#pragma once

#include <memory>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/HttpClientHandle.h"

namespace ruvia {

namespace detail {
class HttpClientState;
}

// One outbound HTTP origin bound to one Ruvia event loop. Construction does
// not create a thread, and connections are established lazily by send().
class HttpClient final {
public:
    HttpClient(EventLoop loop, HttpClientConfig config);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    [[nodiscard]] ScopedOperation<HttpClientResponse> send(const HttpClientRequestView& request, OperationOptions options = {}) const;

    // Idempotent and callable from any thread. Shutdown runs on the bound loop;
    // draining that loop is the lifecycle barrier.
    void close() noexcept;

    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] HttpScheme scheme() const;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    std::shared_ptr<detail::HttpClientState> state_;
};

}  // namespace ruvia
