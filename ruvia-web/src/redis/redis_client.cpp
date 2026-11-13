#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <asio/ssl/context.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/redis/redis_handle.h"

#include "client/client_lifecycle.h"
#include "client/client_tls.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_registry.h"
namespace ruvia::detail {
namespace {

[[nodiscard]] asio::ssl::context make_redis_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context_value(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context_value.native_handle(), config);
    }
    return context_value;
}

void validate_redis_registry(
    const worker_handle& worker_value, std::span<const redis_definition_type> redis) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validate_capability_aliases(redis, "redis alias must not be empty", "duplicate redis alias");
}

}  // namespace

redis_registry::redis_registry(asio::io_context& io_context,
    std::span<const redis_definition_type> redis, worker_handle worker_value)
    : runtimes_(process_resource()),
      alias_index_(process_resource()) {
    validate_redis_registry(worker_value, redis);
    alias_index_.build(redis);
    runtimes_.reserve(redis.size());
    for (const auto& definition : redis) {
        runtimes_.push_back(make_pmr_object<redis_client_runtime>(process_resource(), io_context, worker_value,
            redis_config_storage(definition.config_, process_resource())));
    }
}

redis_registry::~redis_registry() = default;

task<void> redis_registry::connect() {
    for (auto& entry : runtimes_) {
        co_await entry->connect();
    }
    co_return;
}

void redis_registry::close_now() noexcept {
    for (auto& entry : runtimes_) {
        entry->close_now();
    }
}

bool redis_registry::empty() const noexcept {
    return runtimes_.empty();
}

redis_handle redis_registry::get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw redis_error(redis_error::code_type::not_configured, "default redis is not configured");
    }
    return runtimes_[*default_pool_index]->handle(operation_scope, io_metrics);
}

redis_handle redis_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return runtimes_[*match]->handle(operation_scope, io_metrics);
    }
    throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
}

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_handle& worker_value,
    redis_config_storage config)
    : config_(std::move(config)),
      worker_(worker_value),
      tls_context_(make_redis_tls_context(config_.tls_)),
      general_(io_context, config_, tls_context_, config_.command_timeout_,
          config_.pool_size_per_worker_, worker_),
      // Blocking waits have their own deadlines; ordinary command timeouts
      // must not interrupt a finite long wait or an explicitly cancelled wait.
      blocking_(io_context, config_, tls_context_, std::nullopt,
          config_.blocking_pool_size_per_worker_, worker_),
      subscriptions_(io_context, tls_context_, config_, worker_) {}

redis_client_runtime::~redis_client_runtime() = default;

task<void> redis_client_runtime::connect() {
    // Validate ordinary connections at startup; blocking slots and
    // subscriptions connect lazily.
    co_await general_.connect();
}

void redis_client_runtime::close_now() noexcept {
    general_.close_now();
    blocking_.close_now();
    subscriptions_.close_now();
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope, request_io_metrics* io_metrics) {
    return redis_handle(*this, process_resource(), scope, io_metrics);
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope, operation_options options) {
    return redis_handle(*this, process_resource(), scope, std::move(options));
}

class redis_client_state final : public std::enable_shared_from_this<redis_client_state> {
public:
    redis_client_state(event_loop loop, const redis_config& config);

    redis_client_state(const redis_client_state&) = delete;
    redis_client_state& operator=(const redis_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] redis_handle redis();

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<redis_client_state>;
    static constexpr std::string_view client_name{"redis"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] redis_client_runtime& backend() noexcept {
        return runtime_;
    }

    event_loop loop_;
    worker_handle worker_;
    redis_client_runtime runtime_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<redis_client_state> lifecycle_;
};
redis_client_state::redis_client_state(event_loop loop, const redis_config& config)
    : loop_(client_lifecycle<redis_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      runtime_(loop_.io_context(), worker_, redis_config_storage(config, process_resource())),
      lifecycle_(*this, loop_, worker_) {}

void redis_client_state::throw_not_ready() {
    throw std::logic_error("redis client is not connected");
}

redis_handle redis_client_state::redis() {
    lifecycle_.require_ready();
    // Unbounded blocking commands/subscriptions require an explicit lifetime.
    // Use the client's lifetime as the default, not as an extra user source.
    // A with_options() token replaces it; runtime_.close_now() independently
    // cancels all command pools and subscriptions during client shutdown.
    return runtime_.handle(lifecycle_.operation_scope(), {.stop_token_ = lifecycle_.token()});
}

}  // namespace ruvia::detail

namespace ruvia {

redis_client::redis_client(event_loop loop, const redis_config& config)
    : state_(std::allocate_shared<detail::redis_client_state>(
          std::pmr::polymorphic_allocator<detail::redis_client_state>(process_resource()),
          std::move(loop), config)) {
    state_->bind_stop();
}

redis_client::~redis_client() {
    state_->request_close();
}

task<void> redis_client::connect() & {
    return state_->connect();
}

redis_handle redis_client::redis() const& {
    return state_->redis();
}

void redis_client::close() noexcept {
    state_->request_close();
}

task<void> redis_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& redis_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
