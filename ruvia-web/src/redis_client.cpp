#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <asio/ssl/context.hpp>

#include "client_lifecycle.h"
#include "connection_lifecycle.h"
#include "event_loop.h"
#include "http_client_pool.h"
#include "redis_pool.h"
#include "startup_config.h"

namespace ruvia::detail {

namespace {

void validate_redis_registry(const worker_ref& worker_value, std::span<const redis_definition_type> redis) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis registry requires a valid worker");
    }
    validate_capability_aliases(redis, "redis alias must not be empty", "duplicate redis alias");
}

}  // namespace

redis_registry::redis_registry(asio::io_context& io_context, std::span<const redis_definition_type> redis, worker_ref worker_value) :
    runtimes_(),
    alias_index_() {
    validate_redis_registry(worker_value, redis);
    alias_index_.build(redis);
    runtimes_.reserve(redis.size());
    for (const auto& definition : redis) {
        runtimes_.push_back(std::make_unique<redis_client_runtime>(io_context, worker_value, redis_config_storage(definition.config_)));
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

redis_executor redis_registry::get(operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw redis_error(backend_error_code::not_configured, "default redis is not configured");
    }
    return runtimes_[*default_pool_index]->executor(operation_scope, io_metrics);
}

redis_executor redis_registry::get(std::string_view alias, operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return runtimes_[*match]->executor(operation_scope, io_metrics);
    }
    throw redis_error(backend_error_code::not_configured, "redis is not configured");
}

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_ref& worker_value, redis_config_storage config) :
    config_(std::move(config)),
    worker_(worker_value),
    tls_context_(asio::ssl::context::tls_client),
    general_(io_context, config_, tls_context_, config_.command_timeout_, config_.pool_size_per_worker_, worker_),
    // Blocking waits have their own deadlines; ordinary command timeouts
    // must not interrupt a finite long wait or an explicitly cancelled wait.
    blocking_(io_context, config_, tls_context_, std::nullopt, config_.blocking_pool_size_per_worker_, worker_),
    subscriptions_(io_context, tls_context_, config_, worker_) {}

redis_client_runtime::~redis_client_runtime() = default;

task<void> redis_client_runtime::connect(operation_options options) {
    const auto timeout = operation_deadline(options.timeout_).constrained_by(config_.connect_timeout_);
    const linked_operation_options linked({.timeout_ = timeout.timeout(), .stop_token_ = options.stop_token_}, preparation_stop_.token());
    if (config_.tls_.mode_ != client_tls_mode::disabled) {
        std::optional<client_tls_config_storage> prepared;
        std::exception_ptr failure;
        try {
            prepared.emplace(co_await prepare_client_tls_config(worker_, config_.tls_, linked.options()));
        } catch (...) {
            failure = std::current_exception();
        }
        if (preparation_stop_.stop_requested()) {
            throw redis_error(backend_error_code::closing, "redis client is closing");
        }
        if (options.stop_token_.stop_requested()) {
            throw redis_error(backend_error_code::cancelled, "redis connection cancelled");
        }
        if (timeout.expired()) {
            throw redis_error(backend_error_code::timeout, "redis connection timed out");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        configure_client_tls_context(*tls_context_.native_handle(), *prepared);
        config_.tls_ = std::move(*prepared);
    }
    // Every pool slot uses the same absolute startup budget.
    co_await general_.connect({.timeout_ = timeout.timeout(), .stop_token_ = options.stop_token_});
}

void redis_client_runtime::close_now() noexcept {
    preparation_stop_.request_stop();
    general_.close_now();
    blocking_.close_now();
    subscriptions_.close_now();
}

redis_executor redis_client_runtime::executor(operation_scope& scope, request_io_metrics* io_metrics) {
    return redis_executor(*this, scope, io_metrics);
}

redis_executor redis_client_runtime::executor(operation_scope& scope, operation_options options) {
    return redis_executor(*this, scope, std::move(options));
}

class redis_client_state final : public std::enable_shared_from_this<redis_client_state> {
public:
    redis_client_state(event_loop loop, const redis_config& config);

    redis_client_state(const redis_client_state&) = delete;
    redis_client_state& operator=(const redis_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect(operation_options options) {
        return lifecycle_.connect(std::move(options));
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] redis_executor redis();

    [[nodiscard]] const worker_ref& worker() const noexcept {
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
    worker_ref worker_;
    redis_client_runtime runtime_;
    // Retires scopes before backend storage is destroyed.
    client_lifecycle<redis_client_state> lifecycle_;
};
redis_client_state::redis_client_state(event_loop loop, const redis_config& config) :
    loop_(client_lifecycle<redis_client_state>::require_loop(std::move(loop))),
    worker_(loop_.worker()),
    runtime_(loop_.io_context(), worker_, redis_config_storage(config)),
    lifecycle_(*this, loop_, worker_) {}

void redis_client_state::throw_not_ready() {
    throw std::logic_error("redis client is not connected");
}

redis_executor redis_client_state::redis() {
    lifecycle_.require_ready();
    // Unbounded blocking commands/subscriptions require an explicit lifetime.
    // Use the client's lifetime as the default, not as an extra user source.
    // A with_options() token replaces it; runtime_.close_now() independently
    // cancels all command pools and subscriptions during client shutdown.
    return runtime_.executor(lifecycle_.operation_scope(), {.stop_token_ = lifecycle_.token()});
}

}  // namespace ruvia::detail

namespace ruvia {

redis_client::redis_client(event_loop loop, const redis_config& config) :
    state_(std::make_shared<detail::redis_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

redis_client::~redis_client() {
    state_->request_close();
}

task<void> redis_client::connect(operation_options options) & {
    return state_->connect(std::move(options));
}

redis_executor redis_client::redis() const& {
    return state_->redis();
}

void redis_client::request_stop() noexcept {
    state_->request_close();
}

task<void> redis_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& redis_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
