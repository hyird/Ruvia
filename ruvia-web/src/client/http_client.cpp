#include "ruvia/web/http_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "client/http_client_pool.h"
#include "client/http_client_result_budget.h"
#include "client/http_client_state.h"

namespace ruvia::detail {

http_client_state::http_client_state(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      // Responses may outlive this client and its memory_; they own theirs.
      pool_(loop_.io_context(), worker_, http_client_config_storage(config, memory_.resource()),
          std::make_shared<http_client_result_budget_domain>(result_budget), memory_.resource(),
          http_client_response_storage::response_owned),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(http_client_error::code_type::closing, "HTTP client is closing");
}

http_client_handle http_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    validate_operation_options(options);
    return http_client_handle(pool_, memory_.resource(), lifecycle_.operation_scope(),
        lifecycle_.options(std::move(options)));
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

http_client_handle http_client::handle() const& {
    return state_->handle({});
}

void http_client::close() noexcept {
    state_->request_close();
}

task<void> http_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& http_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
