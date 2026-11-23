#include "app.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
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
#include "connection_lifecycle.h"
#include "controller.h"
#include "document_root_publisher.h"
#include "function_ref.h"
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
namespace {

// Validates the whole aggregate before publishing any of it.
void set_worker_config(app_state& state_value, const application_worker_config& config) {
    ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner && config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    ensure_positive_size(config.worker_queue_capacity_, "worker queue capacity must be greater than zero");
    if (config.worker_queue_capacity_ == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
    state_value.options_.worker_count_ = config.worker_count_;
    state_value.options_.worker_queue_capacity_ = config.worker_queue_capacity_;
    state_value.process_signal_handlers_ = config.process_signal_handlers_;
}

}  // namespace

app_state::app_state() {
    // listen() keeps loaded TLS credentials here; initializing OpenSSL first registers its atexit cleanup before the app singleton completes, so the app is destroyed first.
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        throw std::runtime_error("failed to initialize OpenSSL");
    }
    set_worker_config(*this, application_worker_config{});
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

const env& application::env() const& noexcept {
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

std::vector<web_worker_ref> application::worker_refs() const {
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

void application::request_stop() & noexcept {
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

// Every dispatch_configuration member funnels through this guard.
void application::edit_configuration(function_ref<void(detail::app_configuration&)> edit) {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    detail::ensure_app_not_running(state_value.lifecycle_.active(), "cannot change dispatch configuration while app is running");
    edit(state_value.configuration_);
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

application& application::workers(application_worker_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change worker config while app is running", [&config](detail::app_state& state_value) {
        detail::set_worker_config(state_value, config);
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

application& application::trusted_proxies(trusted_proxy_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change trusted proxies while app is running", [config = std::move(config)](detail::app_state& state_value) {
        state_value.options_.set_trusted_proxies(config);
    });
}

application& application::trusted_proxies(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot change trusted proxies while app is running", [](detail::app_state& state_value) {
        state_value.options_.clear_trusted_proxies();
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

application& application::http_client(http_client_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
        detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_.view(), config, "HTTP client alias must not be empty");
    });
}

application& application::http_client(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure HTTP client while app is running", [](detail::app_state& state_value) {
        state_value.http_clients_.clear();
    });
}

}  // namespace ruvia
