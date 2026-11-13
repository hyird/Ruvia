#include "client/http_client_registry.h"

#include <cstdint>
#include <exception>
#include <limits>
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

// Reject invalid aliases before any pool or alias-index storage is allocated.
template <typename config_type>
[[nodiscard]] std::pmr::memory_resource* validated_http_client_resource(
    std::span<const config_type> definitions) {
    validate_capability_aliases(definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
    return process_resource();
}

}  // namespace

struct http_client_registry::origin_preparation final {
    origin_preparation(const http_client_config& source, const worker_handle& worker,
        stop_token owner_stop, stop_token user_stop, const std::optional<tls_trust_store>& system_trust)
        : alias_(source.alias_.view(), process_resource()),
          config_(source, process_resource(), client_tls_files::keep_paths),
          cancellation_(owner_stop, user_stop) {
        if (config_.scheme_ == http_scheme::https) {
            if (!config_.tls_.ca_file_.has_value()) {
                config_.tls_.trust_store_ = system_trust;
            }
            const auto password = source.tls_.identity_
                                      ? std::string_view(source.tls_.identity_->private_key_password_)
                                      : std::string_view{};
            tls_.emplace(prepare_client_tls_config(worker, std::move(config_.tls_), password, cancellation_.token()));
        }
    }

    std::pmr::string alias_;
    http_client_config_storage config_;
    stop_group cancellation_;
    std::optional<task<client_tls_config_storage>> tls_;
};

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::span<const http_client_definition_type> definitions,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      aliases_(validated_http_client_resource(definitions)),
      retirements_(worker_, {.resource_ = process_resource()}) {
    initialize(definitions);
    preparations_.bind_worker(worker_);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::span<const http_client_config> configs,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      aliases_(validated_http_client_resource(configs)),
      retirements_(worker_, {.resource_ = process_resource()}) {
    initialize(configs);
    preparations_.bind_worker(worker_);
}

template <typename config_type>
void http_client_registry::initialize(std::span<const config_type> configs) {
    for (const auto& config : configs) {
        auto pool = std::allocate_shared<http_client_pool>(
            std::pmr::polymorphic_allocator<http_client_pool>(process_resource()), io_context_, worker_,
            startup_config(config), services_);
        aliases_.emplace(std::pmr::string(std::string_view(config.alias_), process_resource()),
            alias_binding{std::move(pool), next_origin_order_++});
    }
}

http_client_registry::~http_client_registry() = default;

http_client_config_storage http_client_registry::startup_config(const http_client_definition_type& definition) {
    // Application definitions were parsed once for every worker.
    return http_client_config_storage(definition.config_, process_resource());
}

http_client_config_storage http_client_registry::startup_config(const http_client_config& config) {
    http_client_config_storage storage(config, process_resource(), client_tls_files::keep_paths);
    if (storage.scheme_ == http_scheme::https) {
        if (!storage.tls_.ca_file_.has_value()) {
            if (!system_trust_.has_value()) {
                system_trust_ = tls_trust_store::load_default();
            }
            storage.tls_.trust_store_ = system_trust_;
        }
        storage.tls_.parse_files(config.tls_.identity_ ? std::string_view(config.tls_.identity_->private_key_password_)
                                                       : std::string_view{});
    }
    return storage;
}

void http_client_registry::require_mutable() const {
    if (!worker_.is_current()) {
        throw std::logic_error("HTTP client origins must be changed on their bound worker");
    }
    if (closing_ || !worker_.accepting()) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::upsert(const http_client_config& config, operation_options options) {
    require_mutable();
    validate_capability_alias(config.alias_.view(), "HTTP client alias must not be empty");
    auto input = make_pmr_object<origin_preparation>(
        process_resource(), config, worker_, preparation_stop_.token(), options.stop_token_, system_trust_);
    return make_scoped_operation(preparations_, prepare_origin(std::move(input), std::move(options)));
}

void http_client_registry::require_prepared(stop_token stop, const operation_deadline& timeout) const {
    require_mutable();
    if (stop.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP origin update cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP origin update deadline expired", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::prepare_origin(preparation_owner_type input, operation_options options) {
    const operation_deadline timeout(options.timeout_);
    const auto stop = input->cancellation_.token();
    require_prepared(stop, timeout);
    if (input->tls_) {
        std::exception_ptr failure;
        try {
            input->config_.tls_ = co_await std::move(*input->tls_);
        } catch (...) {
            failure = std::current_exception();
        }
        input->tls_.reset();
        // Cancellation cannot interrupt an OpenSSL file read. Join it first,
        // then decide whether the completed candidate may still be published.
        require_prepared(stop, timeout);
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!input->config_.tls_.ca_file_.has_value() && !system_trust_.has_value()) {
            system_trust_ = input->config_.tls_.trust_store_;
        }
    }
    auto replacement = std::allocate_shared<http_client_pool>(
        std::pmr::polymorphic_allocator<http_client_pool>(process_resource()), io_context_, worker_,
        std::move(input->config_), services_);
    require_prepared(stop, timeout);
    publish(std::move(input->alias_), std::move(replacement));
}

void http_client_registry::publish(std::pmr::string alias, pool_owner_type replacement) {
    const auto position = aliases_.find(alias);
    if (position != aliases_.end()) {
        auto joining = position->second.pool_->join();
        retirements_.spawn(retire_origin(position, std::move(replacement), std::move(joining)));
        return;
    }
    if (next_origin_order_ == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("HTTP client origin registration order is exhausted");
    }
    aliases_.emplace(std::move(alias), alias_binding{std::move(replacement), next_origin_order_});
    ++next_origin_order_;
}

bool http_client_registry::remove(std::string_view alias) {
    require_mutable();
    validate_capability_alias(alias, "HTTP client alias must not be empty");
    const auto position = aliases_.find(alias);
    if (position == aliases_.end()) {
        return false;
    }
    auto joining = position->second.pool_->join();
    retirements_.spawn(retire_origin(position, {}, std::move(joining)));
    return true;
}

task<void> http_client_registry::retire_origin(alias_map_type::iterator position,
    pool_owner_type replacement, task<void> joining) {
    // task_scope allocates its child before starting this coroutine. Everything
    // through publication is then noexcept: a failed admission changes nothing.
    auto retired = std::exchange(position->second.pool_, std::move(replacement));
    if (!position->second.pool_) {
        aliases_.erase(position);
    }
    retired->close_now();
    co_await std::move(joining);
}

void http_client_registry::close_now() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    preparation_stop_.request_stop();
    for (auto& [alias, binding] : aliases_) {
        binding.pool_->close_now();
    }
}

task<void> http_client_registry::join() {
    close_now();
    std::exception_ptr failure;
    co_await preparations_.close_and_join();
    // Removed origins are no longer in aliases_; their retirement children own
    // the old pools until all transport drivers finish.
    try {
        co_await retirements_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& [alias, binding] : aliases_) {
        try {
            co_await binding.pool_->join();
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

http_client_handle http_client_registry::get_single_or_default(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return aliases_.size() == 1 ? get(aliases_.begin()->first, scope, std::move(host_stop)) : get(scope, std::move(host_stop));
}

http_client_handle http_client_registry::get(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return get(default_capability_alias, scope, std::move(host_stop));
}

http_client_handle http_client_registry::get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "http client registry is closing", std::nullopt, nullptr, false);
    }
    const auto found = aliases_.find(alias);
    if (found == aliases_.end()) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "named HTTP client is not configured", std::nullopt, nullptr, false);
    }
    http_client_handle result(*found->second.pool_, process_resource(), scope);
    result.pool_owner_ = found->second.pool_;
    result.registry_ = this;
    result.host_stop_token_ = std::move(host_stop);
    return result;
}

std::shared_ptr<http_client_pool> http_client_registry::find_origin(http_scheme scheme, std::string_view authority) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
    const alias_binding* found = nullptr;
    // Aliases sharing one origin retain registration precedence, independent
    // of lexical lookup order or a replacement pool's storage position.
    for (const auto& [alias, binding] : aliases_) {
        const auto& pool = binding.pool_;
        if ((found == nullptr || binding.order_ < found->order_) && pool->scheme() == scheme &&
            http_authorities_equal(pool->config().authority_, authority, scheme == http_scheme::https ? 443 : 80)) {
            found = &binding;
        }
    }
    return found == nullptr ? pool_owner_type{} : found->pool_;
}

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, std::span<const http_client_config> configs,
        const http_client_worker_config& worker_config);

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

    [[nodiscard]] http_client_handle client();
    [[nodiscard]] http_client_handle client(std::string_view alias);
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options) {
        lifecycle_.require_ready();
        return registry_.upsert(config, std::move(options));
    }
    [[nodiscard]] bool remove(std::string_view alias) {
        lifecycle_.require_ready();
        return registry_.remove(alias);
    }

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
    http_client_registry registry_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

http_client_state::http_client_state(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker_config)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      // Responses may outlive this client: the process resource backs them.
      registry_(loop_.io_context(), worker_, configs, make_http_client_worker_services(worker_config)),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client is closing", std::nullopt, nullptr, false);
}

http_client_handle http_client_state::client() {
    lifecycle_.require_ready();
    return registry_.get_single_or_default(lifecycle_.operation_scope(), lifecycle_.token());
}

http_client_handle http_client_state::client(std::string_view alias) {
    lifecycle_.require_ready();
    return registry_.get(alias, lifecycle_.operation_scope(), lifecycle_.token());
}
}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    const http_client_worker_config& worker)
    : http_client(std::move(loop), std::span<const http_client_config>(&config, 1), worker) {}

http_client::http_client(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker)
    : state_(std::allocate_shared<detail::http_client_state>(
          std::pmr::polymorphic_allocator<detail::http_client_state>(process_resource()),
          std::move(loop), configs, worker)) {
    state_->bind_stop();
}

http_client::~http_client() {
    state_->request_close();
}

http_client_handle http_client::client() const& {
    return state_->client();
}

http_client_handle http_client::client(std::string_view alias) const& {
    return state_->client(alias);
}

task<void> http_client::upsert(const http_client_config& config, operation_options options) & {
    return state_->upsert(config, std::move(options));
}

bool http_client::remove(std::string_view alias) & {
    return state_->remove(alias);
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
