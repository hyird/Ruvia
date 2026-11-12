#include "client/http_client_registry.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client.h"

#include "client/client_lifecycle.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_pool.h"
#include "client/http_client_result_budget.h"

namespace ruvia::detail {
namespace {

// This runs in the first member initializer so invalid input is rejected before
// even implementation-specific PMR container bookkeeping can touch owner memory.
template <typename config_type>
[[nodiscard]] std::pmr::memory_resource* validated_http_client_resource(
    std::span<const config_type> definitions) {
    validate_capability_aliases(definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
    return process_resource();
}

[[nodiscard]] const http_client_config& client_config(const http_client_config& config) noexcept {
    return config;
}

[[nodiscard]] const http_client_config_storage& client_config(const http_client_definition_type& definition) noexcept {
    return definition.config_;
}
}  // namespace

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::span<const http_client_definition_type> definitions,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain)
    : pools_(validated_http_client_resource(definitions)),
      alias_index_(process_resource()) {
    initialize(io_context, worker_value, definitions, result_budget_domain);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::span<const http_client_config> configs,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain)
    : pools_(validated_http_client_resource(configs)),
      alias_index_(process_resource()) {
    initialize(io_context, worker_value, configs, result_budget_domain);
}

template <typename config_type>
void http_client_registry::initialize(asio::io_context& io_context, const worker_handle& worker_value,
    std::span<const config_type> configs,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    alias_index_.build(configs);
    pools_.reserve(configs.size());
    for (const auto& config : configs) {
        pools_.push_back(make_pmr_object<http_client_pool>(process_resource(), io_context, worker_value,
            http_client_config_storage(client_config(config), process_resource()), result_budget_domain));
    }
}

http_client_registry::~http_client_registry() = default;

void http_client_registry::close_now() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    for (auto& pool : pools_) {
        pool->close_now();
    }
}

task<void> http_client_registry::join() {
    close_now();
    std::exception_ptr failure;
    for (std::size_t i = 0; i < pools_.size(); ++i) {
        try {
            co_await pools_[i]->join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_pool& http_client_registry::default_pool() const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "http client registry is closing");
    }
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw http_client_error(http_client_error::code_type::not_configured, "fixed HTTP client is not configured");
    }
    return *pools_[*default_pool_index];
}

http_client_handle http_client_registry::get(::ruvia::operation_scope& scope, stop_token host_stop) const {
    http_client_handle result(default_pool(), process_resource(), scope);
    result.registry_ = this;
    result.host_stop_token_ = std::move(host_stop);
    return result;
}

http_client_handle http_client_registry::get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop) const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "http client registry is closing");
    }
    const auto found = alias_index_.find(alias);
    if (!found.has_value()) {
        throw http_client_error(http_client_error::code_type::not_configured, "named HTTP client is not configured");
    }
    http_client_handle result(*pools_[*found], process_resource(), scope);
    result.registry_ = this;
    result.host_stop_token_ = std::move(host_stop);
    return result;
}

http_client_pool* http_client_registry::find_origin(http_scheme scheme, std::string_view authority) const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client registry is closing");
    }
    for (const auto& pool : pools_) {
        if (pool->scheme() == scheme &&
            http_authorities_equal(pool->config().authority_, authority, scheme == http_scheme::https ? 443 : 80)) {
            return pool.get();
        }
    }
    return nullptr;
}

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, std::span<const http_client_config> configs,
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
    [[nodiscard]] http_client_handle alias_handle(std::string_view alias);

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<http_client_state>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] http_client_registry& backend() noexcept {
        return registry_;
    }

    event_loop loop_;
    worker_handle worker_;
    std::pmr::string default_alias_;
    http_client_registry registry_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

http_client_state::http_client_state(event_loop loop, std::span<const http_client_config> configs,
    http_client_result_budget_config result_budget)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      default_alias_(configs.size() == 1 ? configs.front().alias_.view() : default_capability_alias, process_resource()),
      // Responses may outlive this client: the process resource backs them.
      registry_(loop_.io_context(), worker_, configs,
          std::allocate_shared<http_client_result_budget_domain>(
              std::pmr::polymorphic_allocator<http_client_result_budget_domain>(process_resource()), result_budget)),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(http_client_error::code_type::closing, "HTTP client is closing");
}

http_client_handle http_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    validate_operation_options(options);
    return registry_.get(default_alias_, lifecycle_.operation_scope()).with_options(std::move(options));
}

http_client_handle http_client_state::alias_handle(std::string_view alias) {
    lifecycle_.require_ready();
    return registry_.get(alias, lifecycle_.operation_scope());
}
}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    http_client_result_budget_config result_budget)
    : http_client(std::move(loop), std::span<const http_client_config>(&config, 1), result_budget) {}

http_client::http_client(event_loop loop, std::span<const http_client_config> configs,
    http_client_result_budget_config result_budget)
    : state_(std::allocate_shared<detail::http_client_state>(
          std::pmr::polymorphic_allocator<detail::http_client_state>(process_resource()),
          std::move(loop), configs, result_budget)) {
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

http_client_handle http_client::client(std::string_view alias) const& {
    return state_->alias_handle(alias);
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
