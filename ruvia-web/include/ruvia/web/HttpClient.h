#pragma once

#include <memory>
#include <optional>

#include "ruvia/core/EventLoop.h"
#include "ruvia/http/HttpClientTunnelRequestView.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/HttpClientTunnel.h"
#include "ruvia/web/HttpClientTunnelConfig.h"

namespace ruvia {

namespace detail {
class HttpClientState;
}

// One outbound HTTP origin bound to one Ruvia event loop. Construction does
// not create a thread, and connections are established lazily by send().
class HttpClient final {
public:
    HttpClient(EventLoop loop, const HttpClientConfig& config,
        HttpClientResultBudgetConfig resultBudget = {});
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    // Handles and operations borrow this client's open lifecycle. Starting one
    // from a temporary would immediately run the client destructor and begin
    // shutdown, so only lvalue clients may create them.
    [[nodiscard]] HttpClientHandle withOptions(OperationOptions options) const&;
    HttpClientHandle withOptions(OperationOptions) const&& = delete;
    [[nodiscard]] ScopedOperation<HttpClientResponse> send(
        const HttpClientRequestView& request) const&;
    ScopedOperation<HttpClientResponse> send(const HttpClientRequestView&) const&& = delete;

    [[nodiscard]] ScopedOperation<HttpClientExchange> openRequest(
        const HttpClientRequestView& head, HttpClientUploadConfig upload = {}) const&;
    ScopedOperation<HttpClientExchange> openRequest(const HttpClientRequestView&, HttpClientUploadConfig = {}) const&& = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the EventLoop retires.
    [[nodiscard]] Task<void> shutdown() &;
    Task<void> shutdown() && = delete;

    [[nodiscard]] ScopedOperation<HttpClientTunnelResult> openTunnel(const HttpClientTunnelRequestView& request, HttpClientTunnelConfig config = {}) const&;
    ScopedOperation<HttpClientTunnelResult> openTunnel(const HttpClientTunnelRequestView&, HttpClientTunnelConfig = {}) const&& = delete;
    [[nodiscard]] ScopedOperation<HttpClientTunnelResult> openUdpTunnel(const HttpClientUdpTunnelRequestView& request, HttpClientTunnelConfig config = {}) const&;
    ScopedOperation<HttpClientTunnelResult> openUdpTunnel(const HttpClientUdpTunnelRequestView&, HttpClientTunnelConfig = {}) const&& = delete;
    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] std::optional<HttpClientAdvertisement> nextAdvertisement() const&;
    [[nodiscard]] std::optional<HttpClientPush> nextPush() const&;
    std::optional<HttpClientPush> nextPush() const&& = delete;
    std::optional<HttpClientAdvertisement> nextAdvertisement() const&& = delete;
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
