#include "ruvia/web/http_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "client/http_client_state.h"

namespace ruvia::detail {

http_client_state::http_client_state(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      clients_(loop_.io_context(), worker_, memory_.resource(), config, result_budget),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(http_client_error::code_type::closing, "HTTP client is closing");
}

http_client_handle http_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope(), lifecycle_.options(std::move(options)));
}

http_client_stats http_client_state::stats() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).stats();
}

std::string_view http_client_state::host() {
    lifecycle_.require_ready();
    const auto client = clients_.get(lifecycle_.operation_scope());
    return client.host();
}

std::uint16_t http_client_state::port() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).port();
}

http_scheme http_client_state::scheme() {
    lifecycle_.require_ready();
    return clients_.get(lifecycle_.operation_scope()).scheme();
}

}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : state_(std::make_shared<detail::http_client_state>(std::move(loop), config, result_budget)) {
    state_->bind_stop();
}

http_client::~http_client() {
    state_->request_close();
}

http_client_handle http_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

scoped_operation<http_client_exchange> http_client::open_request(const http_client_request_view& head, http_client_upload_config upload) const& {
    return with_options({}).open_request(head, upload);
}

scoped_operation<http_client_response> http_client::send(const http_client_request_view& request) const& {
    return with_options({}).send(request);
}

scoped_operation<http_client_tunnel_result> http_client::open_udp_tunnel(const http_client_udp_tunnel_request_view& request, http_client_tunnel_config config) const& {
    return with_options({}).open_udp_tunnel(request, config);
}

scoped_operation<http_client_tunnel_result> http_client::open_tunnel(const http_client_tunnel_request_view& request, http_client_tunnel_config config) const& {
    return with_options({}).open_tunnel(request, config);
}

void http_client::close() noexcept {
    state_->request_close();
}

task<void> http_client::shutdown() & {
    return state_->shutdown();
}

http_client_stats http_client::stats() const {
    return state_->stats();
}

quic_path_migration http_client::start_quic_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) const {
    return with_options({}).start_quic_path_migration(local_endpoint);
}

std::optional<quic_path_migration> http_client::path_migration(std::uint64_t id) const {
    return with_options({}).path_migration(id);
}

quic_operation_status http_client::cancel_quic_path_migration(std::uint64_t id) const {
    return with_options({}).cancel_quic_path_migration(id);
}

std::optional<http_client_push> http_client::next_push() const& {
    return with_options({}).next_push();
}

std::optional<http_client_advertisement> http_client::next_advertisement() const& {
    return with_options({}).next_advertisement();
}

std::string_view http_client::host() const& {
    return state_->host();
}

std::uint16_t http_client::port() const {
    return state_->port();
}

http_scheme http_client::scheme() const {
    return state_->scheme();
}

const worker_handle& http_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
