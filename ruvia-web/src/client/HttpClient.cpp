#include "ruvia/web/HttpClient.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "client/HttpClientState.h"

namespace ruvia::detail {

HttpClientState::HttpClientState(EventLoop loop, const HttpClientConfig& config,
    HttpClientResultBudgetConfig resultBudget)
    : loop_(client_lifecycle<HttpClientState>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      clients_(loop_.ioContext(), worker_, memory_.resource(), config, resultBudget),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void HttpClientState::throw_not_ready() {
    throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client is closing");
}

HttpClientHandle HttpClientState::handle(OperationOptions options) {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope(), lifecycle_.options(std::move(options)));
}

HttpClientStats HttpClientState::stats() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).stats();
}

std::string_view HttpClientState::host() {
    lifecycle_.require_ready();
    const auto client = clients_.get(lifecycle_.operation_scope());
    return client.host();
}

std::uint16_t HttpClientState::port() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).port();
}

HttpScheme HttpClientState::scheme() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).scheme();
}

}  // namespace ruvia::detail

namespace ruvia {

HttpClient::HttpClient(EventLoop loop, const HttpClientConfig& config,
    HttpClientResultBudgetConfig resultBudget)
    : state_(std::make_shared<detail::HttpClientState>(std::move(loop), config, resultBudget)) {
    state_->bindStop();
}

HttpClient::~HttpClient() {
    state_->requestClose();
}

HttpClientHandle HttpClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

ScopedOperation<HttpClientExchange> HttpClient::openRequest(const HttpClientRequestView& head, HttpClientUploadConfig upload) const& {
    return withOptions({}).openRequest(head, upload);
}

ScopedOperation<HttpClientResponse> HttpClient::send(const HttpClientRequestView& request) const& {
    return withOptions({}).send(request);
}

ScopedOperation<HttpClientTunnelResult> HttpClient::openUdpTunnel(const HttpClientUdpTunnelRequestView& request, HttpClientTunnelConfig config) const& {
    return withOptions({}).openUdpTunnel(request, config);
}

ScopedOperation<HttpClientTunnelResult> HttpClient::openTunnel(const HttpClientTunnelRequestView& request, HttpClientTunnelConfig config) const& {
    return withOptions({}).openTunnel(request, config);
}

void HttpClient::close() noexcept {
    state_->requestClose();
}

Task<void> HttpClient::shutdown() & {
    return state_->shutdown();
}

HttpClientStats HttpClient::stats() const {
    return state_->stats();
}

quic_path_migration HttpClient::start_quic_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) const {
    return withOptions({}).start_quic_path_migration(local_endpoint);
}

std::optional<quic_path_migration> HttpClient::path_migration(std::uint64_t id) const {
    return withOptions({}).path_migration(id);
}

quic_operation_status HttpClient::cancel_quic_path_migration(std::uint64_t id) const {
    return withOptions({}).cancel_quic_path_migration(id);
}

std::optional<HttpClientPush> HttpClient::nextPush() const& {
    return withOptions({}).nextPush();
}

std::optional<HttpClientAdvertisement> HttpClient::nextAdvertisement() const& {
    return withOptions({}).nextAdvertisement();
}

std::string_view HttpClient::host() const& {
    return state_->host();
}

std::uint16_t HttpClient::port() const {
    return state_->port();
}

HttpScheme HttpClient::scheme() const {
    return state_->scheme();
}

const WorkerHandle& HttpClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
