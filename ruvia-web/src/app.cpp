#include "app.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/ssl.h>

#include "app_state.h"
#include "callback_ref.h"
#include "connection_lifecycle.h"
#include "controller.h"
#include "document_root_publisher.h"
#include "http_client_pool.h"
#include "http_server.h"
#include "middleware.h"
#include "request_stream.h"
#include "route_table.h"
#include "runtime_control.h"
#include "server_connection.h"
#include "server_options.h"
#include "startup_config.h"
#include "static_root_index.h"
#include "web_worker_runtime.h"
#include "worker_ref.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis_pool.h"
#endif
#if defined(_WIN32)
#include "windows_acceptor.h"
#endif

namespace ruvia::detail {

app_state::app_state() {
    // listen() keeps loaded TLS credentials here; initializing OpenSSL first registers its atexit cleanup before the app singleton completes, so the app is destroyed first.
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        throw std::runtime_error("failed to initialize OpenSSL");
    }
}

app_state::~app_state() = default;

}  // namespace ruvia::detail

namespace ruvia {

application& app() {
    static application instance;
    return instance;
}

application::application() :
    application(std::make_shared<detail::app_state>()) {}

// app_state precedes the backend: its constructor initializes OpenSSL first.
application::application(std::shared_ptr<detail::app_state> state) :
    listener_controls(detail::make_app_listener_backend(state)),
    state_(std::move(state)) {}

application::~application() = default;

const env& application::env() const noexcept {
    return state_->env_;
}

blocking_pool_stats application::blocking_pool_stats() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    if (!state_value.runtime_ || !state_value.runtime_->blocking_pool_) {
        return {};
    }
    return state_value.runtime_->blocking_pool_->stats();
}

std::vector<web_worker_ref> application::workers() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    std::vector<web_worker_ref> result;
    if (!state_value.runtime_ || !state_value.runtime_->workers_ready_) {
        return result;
    }
    result.reserve(state_value.runtime_->workers_.size());
    for (const auto& worker : state_value.runtime_->workers_) {
        result.push_back(worker.runtime_->web_worker());
    }
    return result;
}

web_worker_ref application::worker_for(std::uint64_t key) const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    if (!state_value.runtime_ || !state_value.runtime_->workers_ready_ || state_value.runtime_->workers_.empty()) {
        return {};
    }
    return state_value.runtime_->workers_[key % state_value.runtime_->workers_.size()].runtime_->web_worker();
}

web_worker_ref application::worker_for(std::string_view key) const {
    return worker_for(ruvia::worker_selection_hash(key));
}

void application::run() & {
    detail::run_app(*this, *state_);
}

void application::stop() & {
    auto& state_value = *state_;
    detail::app_runtime_graph* rollback = nullptr;
    {
        std::lock_guard lock(state_value.mutex_);
        const auto request = state_value.lifecycle_.request_stop();
        if (request == detail::app_stop_request::ignored) {
            return;
        }
        // A serving app's run() owner wakes below and stops admission, then
        // the runtime, which closes every connection at once. A startup stop
        // rolls back a published runtime here instead; the control count pins
        // it against retirement while its stop callbacks run outside the lock,
        // where they may reenter the application.
        if (request == detail::app_stop_request::startup && state_value.runtime_ != nullptr) {
            rollback = state_value.runtime_.get();
            ++state_value.runtime_controls_;
        }
    }
    if (rollback != nullptr) {
        detail::request_runtime_stop(*rollback);
        // Notify before unlocking: the woken run() owner retires the runtime
        // and may return, ending the application, as soon as the lock frees.
        std::lock_guard lock(state_value.mutex_);
        --state_value.runtime_controls_;
        state_value.lifecycle_changed_.notify_all();
        return;
    }
    state_value.lifecycle_changed_.notify_all();
}

}  // namespace ruvia

namespace ruvia {

namespace {

[[nodiscard]] detail::static_root_precompression_options make_static_root_precompression_options(const document_root_config& config) {
    detail::ensure_positive_size(config.precompress_min_bytes_, "document root precompression minimum size must be greater than zero");
    if (config.precompress_max_bytes_ < config.precompress_min_bytes_) {
        throw std::invalid_argument("document root precompression maximum size must not be smaller than the minimum size");
    }
    return detail::static_root_precompression_options{
        .gzip_ = config.precompress_gzip_,
        .brotli_ = config.precompress_brotli_,
        .zstd_ = config.precompress_zstd_,
        .min_bytes_ = config.precompress_min_bytes_,
        .max_bytes_ = config.precompress_max_bytes_,
    };
}

}  // namespace

application& application::document_root(document_root_config config) & {
    std::optional<detail::app_document_root_config> retired;
    return detail::mutate_stopped_app(*this, *state_, "cannot change document root while app is running", [&config, &retired](detail::app_state& state_value) {
        if (config.root_.empty()) {
            throw std::invalid_argument("document root must not be empty");
        }
        detail::ensure_positive_duration(config.refresh_interval_, "document root refresh interval must be greater than zero");
        detail::validate_static_root_options(config.static_options_);
        const auto precompression = make_static_root_precompression_options(config);

        detail::app_document_root_config replacement(detail::store_validated_static_root_config(config.static_options_));
        http_assign_native_path(replacement.root_, config.root_);
        replacement.refresh_interval_ = config.refresh_interval_;
        replacement.precompression_ = precompression;
        replacement.resident_ = detail::static_root_resident_options{
            .max_file_bytes_ = config.static_options_.resident_max_bytes_.value_or(std::size_t{64} * 1024),
            .budget_bytes_ = config.static_options_.resident_budget_bytes_.value_or(std::size_t{16} * 1024 * 1024),
        };

        retired = std::exchange(state_value.document_root_config_, std::move(replacement));
    });
}

application& application::document_root(std::nullptr_t) & {
    std::optional<detail::app_document_root_config> retired;
    return detail::mutate_stopped_app(*this, *state_, "cannot change document root while app is running", [&retired](detail::app_state& state_value) {
        retired = std::exchange(state_value.document_root_config_, std::nullopt);
    });
}

application& application::use_middleware(detail::owned_middleware_descriptor descriptor) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot add app middleware while app is running", [&descriptor](detail::app_state& state_value) {
        state_value.configuration_.add_middleware(std::move(descriptor));
    });
}

application& application::blocking_pool(blocking_pool_options config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change the blocking pool while app is running", [&config](detail::app_state& state_value) {
        state_value.blocking_pool_ = config;
    });
}

application& application::blocking_pool(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change the blocking pool while app is running", [](detail::app_state& state_value) {
        state_value.blocking_pool_.reset();
    });
}

application& application::use_worker_state_definition(detail::worker_state_definition definition) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register worker state while app is running", [&definition](detail::app_state& state_value) {
        state_value.configuration_.add_worker_state(std::move(definition));
    });
}

application& application::on_error(http_error_handler handler) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change error handler while app is running", [handler = detail::own_callback(std::move(handler))](detail::app_state& state_value) mutable {
        state_value.configuration_.on_error(std::move(handler));
    });
}

application& application::on_not_found(http_not_found_handler handler) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change not found handler while app is running", [handler = detail::own_callback(std::move(handler))](detail::app_state& state_value) mutable {
        state_value.configuration_.on_not_found(std::move(handler));
    });
}

application& application::on_error(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change error handler while app is running", [retired = detail::callback_registration<http_error_handler>()](detail::app_state& state_value) mutable {
        // The cleared target retires with this lambda, after the app lock.
        retired = state_value.configuration_.on_error(nullptr);
    });
}

application& application::on_not_found(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change not found handler while app is running", [retired = detail::callback_registration<http_not_found_handler>()](detail::app_state& state_value) mutable {
        retired = state_value.configuration_.on_not_found(nullptr);
    });
}

application& application::on_error(scoped_error_handler_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change error handler while app is running", [prefix = options.prefix_, handler = detail::own_callback(std::move(options.handler_))](detail::app_state& state_value) mutable {
        state_value.configuration_.on_error(prefix, std::move(handler));
    });
}

application& application::on_not_found(scoped_not_found_handler_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change not found handler while app is running", [prefix = options.prefix_, handler = detail::own_callback(std::move(options.handler_))](detail::app_state& state_value) mutable {
        state_value.configuration_.on_not_found(prefix, std::move(handler));
    });
}

application& application::load_dotenv(dotenv_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
        state_value.env_.load_dotenv(options);
    });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
        state_value.env_.load_dotenv(path, options);
    });
}

application& application::server(server_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change server config while app is running", [&config](detail::app_state& state_value) {
        state_value.options_.set_server(config);
    });
}

application& application::on_start(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register on_start hook while app is running", [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
        if (!hook) {
            throw std::invalid_argument("on_start hook must not be null");
        }
        state_value.on_start_hooks_.push_back(std::move(hook));
    });
}

application& application::on_start(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register on_start hook while app is running", [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
        // The cleared targets retire with this lambda, after the app lock.
        retired.swap(state_value.on_start_hooks_);
    });
}

application& application::on_stop(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register on_stop hook while app is running", [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
        if (!hook) {
            throw std::invalid_argument("on_stop hook must not be null");
        }
        state_value.on_stop_hooks_.push_back(std::move(hook));
    });
}

application& application::on_stop(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register on_stop hook while app is running", [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
        retired.swap(state_value.on_stop_hooks_);
    });
}

application& application::rate_limit(rate_limit_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change the default rate limit per worker while app is running", [config](detail::app_state& state_value) {
        state_value.options_.set_rate_limit(config);
    });
}

application& application::rate_limit(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change the default rate limit per worker while app is running", [](detail::app_state& state_value) {
        state_value.options_.clear_rate_limit();
    });
}

application& application::trusted_proxies(trusted_proxy_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change trusted proxies while app is running", [config = std::move(config)](detail::app_state& state_value) {
        state_value.options_.set_trusted_proxies(config);
    });
}

application& application::callbacks(http_server_callbacks callbacks) & {
    // Built outside the lock; the swapped-out set retires with this lambda,
    // after the app lock, so a target's destructor may reenter the app.
    return detail::mutate_stopped_app(*this, *state_, "cannot change host callbacks while app is running", [replacement = std::make_unique<http_server_callbacks>(std::move(callbacks))](detail::app_state& state_value) mutable {
        std::swap(state_value.callbacks_, replacement);
        state_value.options_.use_callbacks(*state_value.callbacks_);
    });
}

#ifdef RUVIA_ENABLE_DATABASE
application& application::db(db_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure database while app is running", [&](detail::app_state& state_value) {
        detail::validate_capability_alias(config.alias_.view(), "database alias must not be empty");
        detail::db_definition replacement{std::string(config.alias_.view()), detail::db_config_storage(config.config_)};
        if (config.query_cache_) {
#ifdef RUVIA_ENABLE_REDIS
            replacement.query_cache_.emplace(*config.query_cache_);
#else
            throw std::invalid_argument("database query cache requires Redis support");
#endif
        }
        detail::upsert_named_capability_definition(state_value.databases_, std::move(replacement));
    });
}

application& application::db(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure database while app is running", [](detail::app_state& state_value) {
        state_value.databases_.clear();
    });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
application& application::redis(redis_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running", [&](detail::app_state& state_value) {
        detail::upsert_named_capability_definition(state_value.redis_, config.alias_.view(), config.config_, "redis alias must not be empty");
    });
}

application& application::redis(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running", [](detail::app_state& state_value) {
        state_value.redis_.clear();
    });
}
#endif

application& application::client(http_client_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
        detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_.view(), config, "HTTP client alias must not be empty");
    });
}

application& application::client(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure HTTP client while app is running", [](detail::app_state& state_value) {
        state_value.http_clients_.clear();
    });
}

}  // namespace ruvia
