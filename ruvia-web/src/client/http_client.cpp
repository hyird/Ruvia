#include "ruvia/web/http_client.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"

#include "client/client_lifecycle.h"
#include "client/http_client_pool.h"
#include "client/http_client_result_budget.h"

namespace ruvia::detail {

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, const http_client_config& config,
        http_client_result_budget_config result_budget);

    http_client_state(const http_client_state&) = delete;
    http_client_state& operator=(const http_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }

    [[nodiscard]] http_client_handle handle(operation_options options);

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<http_client_state>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] http_client_pool& backend() noexcept {
        return pool_;
    }

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    http_client_pool pool_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

http_client_state::http_client_state(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      // Responses may outlive this client and its memory_; they own theirs.
      pool_(loop_.io_context(), worker_, http_client_config_storage(config, memory_.resource()),
          std::allocate_shared<http_client_result_budget_domain>(
              std::pmr::polymorphic_allocator<http_client_result_budget_domain>(process_resource()), result_budget),
          memory_.resource(), http_client_response_storage::response_owned),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(http_client_error::code_type::closing, "HTTP client is closing");
}

http_client_handle http_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    validate_operation_options(options);
    return http_client_handle(pool_, memory_.resource(), lifecycle_.operation_scope(),
        std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : state_(std::allocate_shared<detail::http_client_state>(
          std::pmr::polymorphic_allocator<detail::http_client_state>(process_resource()),
          std::move(loop), config, result_budget)) {
    state_->bind_stop();
}

http_client::~http_client() {
    state_->request_close();
}

http_client_handle http_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

http_client_handle http_client::client() const& {
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
