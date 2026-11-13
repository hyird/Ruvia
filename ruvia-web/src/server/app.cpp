#include "ruvia/web/app.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/signal_set.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/router.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/testing.h"
#include "ruvia/web/web_worker.h"

#include "http/context_access.h"
#include "http/inline_http_header_views.h"
#include "http/static_root_index.h"
#include "server/app_state.h"
#include "server/connection_lifecycle.h"
#include "server/http_server_options.h"
#include "server/multiplexed_stream_signal.h"
#include "server/route_table.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_session.h"
#endif

#include <fstream>
#include <istream>
#include <system_error>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/url_encoding.h"

#include "client/http_client_pool.h"
#include "http/operation_interrupt.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_session.h"

#include "db/db_pool.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_session.h"

#include "redis/redis_pool.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
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

application::application()
    : state_(std::make_shared<detail::app_state>()) {}

application::~application() = default;

const env& application::env() const noexcept {
    return state_->env_;
}

http_server_stats application::stats() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    http_server_stats total;
    if (!state_value.runtime_) {
        return total;
    }
    for (const auto& worker : state_value.runtime_->workers_) {
        const auto stats = worker.runtime_->stats();
        total.active_connections_ += stats.active_connections_;
        total.connections_refused_ += stats.connections_refused_;
        total.connection_failures_ += stats.connection_failures_;
        total.accept_failures_ += stats.accept_failures_;
        total.worker_failures_ += stats.worker_failures_;
        detail::append_listener_stats(total, stats);
    }
#if defined(_WIN32)
    // The acceptor refuses and fails accepts before any worker sees them.
    total.connections_refused_ += state_value.runtime_->acceptor_->refused();
    total.accept_failures_ += state_value.runtime_->acceptor_->accept_failures();
#endif
    // One application-wide refresher owns the count; workers only adopt.
    if (state_value.runtime_->document_root_) {
        total.document_root_refresh_failures_ = state_value.runtime_->document_root_->failures();
    }
    return total;
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
    if (!state_value.runtime_ || !state_value.runtime_->workers_ready_ ||
        state_value.runtime_->workers_.empty()) {
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
    {
        std::lock_guard lock(state_value.mutex_);
        const auto request = state_value.lifecycle_.request_stop();
        if (request == detail::app_stop_request::ignored) {
            return;
        }
        // A serving app's run() owner wakes below and stops admission, then
        // the runtime, which closes every connection at once.
        if (request == detail::app_stop_request::startup) {
            detail::request_runtime_stop(state_value);
        }
    }
    state_value.lifecycle_changed_.notify_all();
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

template <typename handlers_type, typename handler_type>
void append_prefix_handler(handlers_type& handlers, std::string_view prefix, handler_type&& handler) {
    if (!handler) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    const auto normalized = normalize_fallback_prefix(prefix);
    for (const auto& existing : handlers) {
        if (fallback_prefixes_equivalent(existing.first, normalized)) {
            throw std::invalid_argument("duplicate fallback prefix");
        }
    }
    handlers.emplace_back(std::string(normalized), std::move(handler));
}

}  // namespace

app_configuration::app_configuration() = default;

void app_configuration::add_middleware(owned_middleware_descriptor&& descriptor) {
    const auto& value = descriptor.descriptor();
    if (value.validated_model_type_key() != nullptr) {
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    // Route admission reads body-limit declarations from route and controller
    // lists only; app-wide they would silently bound nothing. The app-wide
    // ceiling is server_config::max_buffered_body_bytes_.
    if (value.request_body_limit() != 0) {
        throw std::invalid_argument("body_limit binds to a route or controller and cannot be app-wide");
    }
    if (value.stream_request_body()) {
        throw std::invalid_argument("stream_body binds to a route or controller and cannot be app-wide");
    }
    middlewares_.push_back(std::move(descriptor));
}

void app_configuration::add_worker_state(worker_state_definition&& definition) {
    append_worker_state_definition(worker_states_, std::move(definition));
}

// A second app-wide registration throws like a duplicate scoped prefix rather
// than silently replacing the first by call order.
void app_configuration::on_error(callback_registration<http_error_handler>&& handler) {
    if (!handler) {
        throw std::invalid_argument("error handler must not be null");
    }
    if (error_handler_) {
        throw std::invalid_argument("duplicate app-wide error handler");
    }
    error_handler_ = std::move(handler);
}

callback_registration<http_error_handler> app_configuration::on_error(std::nullptr_t) noexcept {
    return std::exchange(error_handler_, nullptr);
}

void app_configuration::on_error(borrowed_text prefix, callback_registration<http_error_handler>&& handler) {
    append_prefix_handler(prefix_error_handlers_, prefix.view(), std::move(handler));
}

void app_configuration::on_not_found(callback_registration<http_not_found_handler>&& handler) {
    if (!handler) {
        throw std::invalid_argument("not found handler must not be null");
    }
    if (not_found_handler_) {
        throw std::invalid_argument("duplicate app-wide not found handler");
    }
    not_found_handler_ = std::move(handler);
}

callback_registration<http_not_found_handler> app_configuration::on_not_found(std::nullptr_t) noexcept {
    return std::exchange(not_found_handler_, nullptr);
}

void app_configuration::on_not_found(borrowed_text prefix, callback_registration<http_not_found_handler>&& handler) {
    append_prefix_handler(prefix_not_found_handlers_, prefix.view(), std::move(handler));
}

void app_configuration::apply(router& routes_value) const {
    routes_value.set_error_handler(callback_access::ref(error_handler_));
    routes_value.set_not_found_handler(callback_access::ref(not_found_handler_));
    routes_value.set_prefix_error_handlers(prefix_error_handlers_);
    routes_value.set_prefix_not_found_handlers(prefix_not_found_handlers_);
    if (!middlewares_.empty()) {
        std::vector<controller_middleware_descriptor> descriptors;
        descriptors.reserve(middlewares_.size());
        for (const auto& middleware_value : middlewares_) {
            descriptors.push_back(middleware_value.descriptor());
        }
        routes_value.set_global_middlewares(descriptors);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] detail::static_root_precompression_options make_static_root_precompression_options(
    const document_root_config& config) {
    detail::ensure_positive_size(config.precompress_min_bytes_,
        "document root precompression minimum size must be greater than zero");
    if (config.precompress_max_bytes_ < config.precompress_min_bytes_) {
        throw std::invalid_argument(
            "document root precompression maximum size must not be smaller than the minimum size");
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
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change document root while app is running", [&config, &retired](detail::app_state& state_value) {
            if (config.root_.empty()) {
                throw std::invalid_argument("document root must not be empty");
            }
            detail::ensure_positive_duration(config.refresh_interval_,
                "document root refresh interval must be greater than zero");
            detail::validate_static_root_options(config.static_options_);
            const auto precompression = make_static_root_precompression_options(config);

            detail::app_document_root_config replacement(
                detail::store_validated_static_root_config(config.static_options_));
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
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change document root while app is running",
        [&retired](detail::app_state& state_value) {
            retired = std::exchange(state_value.document_root_config_, std::nullopt);
        });
}

application& application::use_middleware(detail::owned_middleware_descriptor descriptor) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot add app middleware while app is running",
        [&descriptor](detail::app_state& state_value) {
            state_value.configuration_.add_middleware(std::move(descriptor));
        });
}

application& application::blocking_pool(blocking_pool_options config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the blocking pool while app is running",
        [&config](detail::app_state& state_value) { state_value.blocking_pool_ = config; });
}

application& application::blocking_pool(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the blocking pool while app is running",
        [](detail::app_state& state_value) { state_value.blocking_pool_.reset(); });
}

application& application::use_worker_state_definition(detail::worker_state_definition definition) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register worker state while app is running",
        [&definition](detail::app_state& state_value) {
            state_value.configuration_.add_worker_state(std::move(definition));
        });
}

application& application::on_error(http_error_handler handler) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change error handler while app is running",
        [handler = detail::own_callback(std::move(handler))](detail::app_state& state_value) mutable {
            state_value.configuration_.on_error(std::move(handler));
        });
}

application& application::on_not_found(http_not_found_handler handler) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change not found handler while app is running",
        [handler = detail::own_callback(std::move(handler))](detail::app_state& state_value) mutable {
            state_value.configuration_.on_not_found(std::move(handler));
        });
}

application& application::on_error(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change error handler while app is running",
        [retired = detail::callback_registration<http_error_handler>()](detail::app_state& state_value) mutable {
            // The cleared target retires with this lambda, after the app lock.
            retired = state_value.configuration_.on_error(nullptr);
        });
}

application& application::on_not_found(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change not found handler while app is running",
        [retired = detail::callback_registration<http_not_found_handler>()](detail::app_state& state_value) mutable {
            retired = state_value.configuration_.on_not_found(nullptr);
        });
}

application& application::on_error(scoped_error_handler_options options) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change error handler while app is running",
        [prefix = options.prefix_, handler = detail::own_callback(std::move(options.handler_))](detail::app_state& state_value) mutable {
            state_value.configuration_.on_error(prefix, std::move(handler));
        });
}

application& application::on_not_found(scoped_not_found_handler_options options) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change not found handler while app is running",
        [prefix = options.prefix_, handler = detail::own_callback(std::move(options.handler_))](detail::app_state& state_value) mutable {
            state_value.configuration_.on_not_found(prefix, std::move(handler));
        });
}

}  // namespace ruvia

namespace ruvia {

web_worker_context::web_worker_context(const worker_ref& worker_value,
    const detail::worker_client_registry_view& client_registries,
    const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value) noexcept
    : capabilities_(worker_value, stop_token_value, worker_states),
      client_registries_(client_registries) {
    operation_scope_.bind_worker(worker_value);
}
const worker_ref& web_worker_context::worker() const& noexcept {
    return capabilities_.worker();
}

::ruvia::stop_token web_worker_context::stop_token() const noexcept {
    return capabilities_.stop_token();
}

#ifdef RUVIA_ENABLE_DATABASE
db_session web_worker_context::db() const {
    return client_registries_.db(operation_scope_, nullptr, capabilities_.stop_token());
}

db_session web_worker_context::db(std::string_view alias) const {
    return client_registries_.db(alias, operation_scope_, nullptr, capabilities_.stop_token());
}
#endif

http_client_ref web_worker_context::client() const {
    return client_registries_.client(operation_scope_, capabilities_.stop_token());
}

http_client_ref web_worker_context::client(std::string_view alias) const {
    return client_registries_.client(alias, operation_scope_, capabilities_.stop_token());
}

#ifdef RUVIA_ENABLE_REDIS
redis_session web_worker_context::redis() const {
    return client_registries_.redis(operation_scope_, nullptr, capabilities_.stop_token());
}

redis_session web_worker_context::redis(std::string_view alias) const {
    return client_registries_.redis(alias, operation_scope_, nullptr, capabilities_.stop_token());
}
#endif

web_worker_ref::web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept
    : dispatch_(std::move(dispatch)) {}

bool web_worker_ref::valid() const noexcept {
    return dispatch_ && dispatch_->valid();
}

bool web_worker_ref::accepting() const noexcept {
    return dispatch_ && dispatch_->accepting();
}

bool web_worker_ref::is_current() const noexcept {
    return dispatch_ && dispatch_->is_current();
}

worker_id_type web_worker_ref::id() const noexcept {
    return dispatch_ ? dispatch_->id() : 0;
}

web_worker_stats web_worker_ref::stats() const noexcept {
    return dispatch_ ? dispatch_->stats() : web_worker_stats{};
}

web_worker_post_result web_worker_ref::post_task(
    move_only_function<task<void>(web_worker_context&)> task_value) const {
    if (dispatch_) {
        return dispatch_->post(std::move(task_value));
    }
    // A handle without a worker refuses with worker_stopping and returns the task.
    return worker_ref().post_payload(std::move(task_value),
        [](move_only_function<task<void>(web_worker_context&)>&&) -> move_only_function<void()> {
            std::terminate();  // An empty handle never admits.
        });
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

// A move-safe reservation for the outstanding_ count post() takes before core
// admission. It rides inside the accepted job; unique_ptr move semantics keep
// exactly one owner as move_only_function relocates the closure. A started job
// release()s it and run() owns the decrement; a job core destroys unrun
// (rejected post, or a stopping worker abandoning queued work) reconciles here.
struct abandon_reservation_deleter {
    void operator()(web_worker_dispatch* dispatch) const noexcept {
        dispatch->abandon();
    }
};
using abandon_reservation_type = std::unique_ptr<web_worker_dispatch, abandon_reservation_deleter>;

}  // namespace

web_worker_dispatch::web_worker_dispatch(ruvia::worker_runtime& runtime,
    worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value)
    : worker_(runtime.worker()),
      runtime_(runtime),
      client_registries_(capabilities.client_registries()),
      worker_states_(capabilities.worker_states()),
      jobs_(jobs),
      stop_token_(stop_token_value) {}

web_worker_ref web_worker_dispatch::make_ref(std::shared_ptr<web_worker_dispatch> dispatch) noexcept {
    return web_worker_ref(std::move(dispatch));
}

bool web_worker_dispatch::valid() const noexcept {
    return worker_.valid();
}

bool web_worker_dispatch::accepting() const noexcept {
    return worker_.accepting();
}

bool web_worker_dispatch::is_current() const noexcept {
    return worker_.is_current();
}

worker_id_type web_worker_dispatch::id() const noexcept {
    return worker_.id();
}

web_worker_post_result web_worker_dispatch::post(job_type job) {
    // Reserve before entering the core queue; core invokes the factory only
    // for an accepted post and destroys an unrun job on abandonment.
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    abandon_reservation_type reservation(this);
    auto result = worker_.post_payload(std::move(job), [&reservation](job_type&& admitted) -> move_only_function<void()> {
        return [job = std::move(admitted), reservation = std::move(reservation)]() mutable {
            reservation.release()->start(std::move(job));
        };
    });
    switch (result.status()) {
        case post_status::accepted:
            accepted_.fetch_add(1, std::memory_order_relaxed);
            break;
        case post_status::queue_full:
            queue_full_.fetch_add(1, std::memory_order_relaxed);
            break;
        case post_status::worker_stopping:
            worker_stopping_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
    return result;
}

web_worker_stats web_worker_dispatch::stats() const noexcept {
    return web_worker_stats{
        .accepted_ = accepted_.load(std::memory_order_relaxed),
        .queue_full_ = queue_full_.load(std::memory_order_relaxed),
        .worker_stopping_ = worker_stopping_.load(std::memory_order_relaxed),
        .completed_ = completed_.load(std::memory_order_relaxed),
        .failed_ = failed_count_.load(std::memory_order_relaxed),
        .outstanding_ = outstanding_.load(std::memory_order_acquire),
    };
}

void web_worker_dispatch::start(job_type job) {
    // Stopping abandons queued jobs: retirement drains reserved queue
    // publication, which must not become new work against retiring state.
    if (!worker_.accepting() || stop_token_.stop_requested()) {
        abandon();
        return;
    }
    try {
        jobs_.spawn(run(std::move(job)));
    } catch (...) {
        abandon();
        throw;
    }
}

ruvia::task<void> web_worker_dispatch::run(job_type job) {
    std::exception_ptr failure;
    try {
        web_worker_context context(worker_, client_registries_, &worker_states_, stop_token_);
        co_await job(context);
    } catch (...) {
        failure = std::current_exception();
    }
    completed_.fetch_add(1, std::memory_order_relaxed);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    if (failure != nullptr) {
        failed_count_.fetch_add(1, std::memory_order_relaxed);
        // A failed job fails its worker, like any other worker-owned work.
        (void)runtime_.fail(std::move(failure));
    }
}

void web_worker_dispatch::abandon() noexcept {
    // Reconciles only post()'s reservation: this is not a completion, and a
    // rejected post is already counted by post().
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace ruvia::detail

namespace ruvia {

// test_app drives the production worker bring-up (worker_capabilities
// start/stop) and the production per-request chain (admission, dispatch, the
// response tail) on one core event loop. Only the transport is absent.
struct test_app::impl_type final {
    enum class lifecycle_type { configuring,
        initializing,
        ready,
        failed };

    detail::app_configuration configuration_;
    detail::controller_store controllers_;
    detail::compiled_route_plan_ptr_type route_plan_;
    std::optional<detail::route_table> routes_;
    env env_;
    detail::worker_codecs codecs_;
    // The defaults a server starts from: no listener-specific policy applies.
    detail::http_server_options options_{};
    // The pool the in-memory worker's event loop borrows, created with the loop.
    blocking_pool blocking_pool_{{.thread_count_ = 1}};
    event_loop_pool event_loop_pool_{{.loop_count_ = 1, .blocking_pool_ = &blocking_pool_}};
    event_loop event_loop_{event_loop_pool_.loop(0)};
    worker_ref worker_{event_loop_.worker()};
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    std::optional<detail::worker_capabilities> capabilities_;
    lifecycle_type lifecycle_{lifecycle_type::configuring};
    std::exception_ptr startup_failure_{};

    impl_type() {
        options_.codecs_ = &codecs_;
    }

    ~impl_type() {
        if (lifecycle_ == lifecycle_type::ready) {
            stop_source_.request_stop();
            try {
                event_loop_.start(capabilities_->stop()).get();
            } catch (...) {
                std::terminate();
            }
        }
        blocking_pool_.stop();
        event_loop_pool_.stop();
        event_loop_pool_.join();
        blocking_pool_.join();
    }

    void require_configurable() const {
        if (lifecycle_ != lifecycle_type::configuring) {
            throw std::logic_error("test_app must be configured before its first request()");
        }
    }

    void finalize() {
        if (lifecycle_ == lifecycle_type::ready) {
            return;
        }
        if (lifecycle_ == lifecycle_type::initializing) {
            throw std::logic_error("test_app request() cannot be reentered during startup");
        }
        if (lifecycle_ == lifecycle_type::failed) {
            std::rethrow_exception(startup_failure_);
        }
        lifecycle_ = lifecycle_type::initializing;

        try {
            const auto controller_registrars = detail::seal_controller_registrars();
            controllers_ = detail::controller_store(controller_registrars);
            route_plan_ = detail::compile_route_plan(
                controller_registrars, controllers_, configuration_);
            routes_.emplace(*route_plan_, controllers_);

            capabilities_.emplace(event_loop_.io_context(), worker_,
                detail::worker_capability_definitions{.worker_states_ = configuration_.worker_states()},
                detail::worker_capability_options{
                    .default_rate_limit_ = options_.default_rate_limit_per_worker_,
                    .route_rate_limits_ = routes_->has_route_rate_limit()
                                              ? detail::route_rate_limit_presence::present
                                              : detail::route_rate_limit_presence::absent,
                    .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
                    .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                    .http_client_worker_ = options_.server_.http_client_worker_,
                    .env_ = &env_,
                    .trusted_proxies_ =
                        options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
                    .precompressed_static_files_ = options_.compression_.has_value(),
                    .file_io_timeout_ = options_.server_.write_timeout_,
                    .connection_failure_ = &options_.connection_failure_,
                });
            event_loop_pool_.start();
            event_loop_.start(capabilities_->start()).get();
            lifecycle_ = lifecycle_type::ready;
        } catch (...) {
            startup_failure_ = std::current_exception();
            lifecycle_ = lifecycle_type::failed;
            throw;
        }
    }

    [[nodiscard]] task<dispatch_response> execute_request(const test_request& request) {
        const auto request_start = std::chrono::steady_clock::now();
        detail::inline_http_header_views<16> active_headers(
            request.headers_.size() + (!request.cookies_.empty() ? 1U : 0U));
        for (const auto& [name, value] : request.headers_) {
            active_headers.push_back(http_header_view(name, value));
        }
        if (!request.cookies_.empty()) {
            active_headers.push_back(http_header_view("Cookie", request.cookies_));
        }
        std::optional<http_protocol_error> parse_error;
        auto parsed_value = [&]() {
            try {
                return make_parsed_http_request(request.method_, request.target_, active_headers.view(),
                    std::as_bytes(std::span(request.body_.data(), request.body_.size())));
            } catch (const http_protocol_error& error) {
                parse_error = error;
                return make_parsed_http_request("GET", "/", {}, {});
            }
        }();

        const auto& routes_value = *routes_;
        const auto resolution = routes_value.resolve(parsed_value);
        // A test request has no transport connection, hence connection id zero.
        const auto services = capabilities_->make_context_services(stop_token_)
                                  .with_plain_transport(0, request.remote_address_, request.remote_port_);

        // A malformed request is rejected before admission, with no coding
        // promise, exactly as a server answers a malformed head.
        std::optional<detail::http_request_admission> admission;
        http_response response;
        if (parse_error.has_value()) {
            const auto rejection = detail::http_request_rejection::error(
                detail::copy_http_protocol_error_info(*parse_error));
            response = co_await detail::respond_http_request_rejection(
                routes_value, parsed_value, rejection, services);
        } else {
            // These borrowed arguments must remain in the coroutine frame across
            // suspension. MSVC cannot tail-transfer this call with braced temporaries.
            const detail::http_request_admission_input input{
                .request_ = parsed_value,
                .resolution_ = resolution,
                .coding_ = http_response_coding_selection_for(parsed_value),
                .start_ = request_start,
                .pinned_arrival_ = request.arrival_,
            };
            const auto policy = detail::http_request_admission_policy::server(options_);
            auto admitted = co_await detail::admit_and_dispatch_buffered_request(
                routes_value, input, policy, services, response);
            admission.emplace(std::move(admitted));
        }
        const auto& request_services = admission ? admission->services() : services;
        const auto remote_ip =
            ip_address::parse(request.remote_address_).value_or(ip_address{});
        const auto tail = admission ? admission->tail()
                                    : detail::http_response_tail(parsed_value,
                                          detail::http_response_coding_policy::disabled(),
                                          remote_ip, remote_ip, request_start);
        const auto prepared = co_await tail.prepare_or_finish(
            response, options_, worker_, routes_value, request_services);

        dispatch_response result_value;
        result_value.copy_head_from(response);
        // On the wire, HEAD responses and content-forbidden statuses carry no
        // body, so the facade must not surface one
        // either. Writer-synthesized fields (Content-Length, Date, Connection)
        // are framing concerns and stay absent here.
        if (!prepared.write_plan_.body_plan().body_suppressed()) {
            if (response.file_body() || response.has_multipart_file_body()) {
                detail::response_file_source source(response,
                    {.worker_ = worker_, .stop_ = stop_token_, .timeout_ = options_.server_.write_timeout_});
                for (;;) {
                    const auto chunk = co_await source.next();
                    if (chunk.kind() == detail::response_body_chunk::kind_type::end) {
                        break;
                    }
                    if (chunk.kind() == detail::response_body_chunk::kind_type::failed) {
                        throw std::runtime_error("failed to read file response in test_app");
                    }
                    result_value.body_.append(chunk.bytes());
                }
            } else {
                const auto body = response.body_bytes();
                result_value.body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
            }
        }
        co_return result_value;
    }
};

test_app::test_app()
    : impl_(std::make_unique<impl_type>()) {}

test_app::~test_app() = default;

test_app& test_app::server(server_config config) {
    impl_->require_configurable();
    impl_->options_.set_server(config);
    return *this;
}

test_app& test_app::rate_limit(rate_limit_config config) {
    impl_->require_configurable();
    impl_->options_.set_rate_limit(config);
    return *this;
}

test_app& test_app::rate_limit(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.clear_rate_limit();
    return *this;
}

test_app& test_app::trusted_proxies(trusted_proxy_config config) {
    impl_->require_configurable();
    impl_->options_.set_trusted_proxies(config);
    return *this;
}

test_app& test_app::trusted_proxies(std::nullptr_t) {
    impl_->require_configurable();
    impl_->options_.clear_trusted_proxies();
    return *this;
}

test_app& test_app::on_error(http_error_handler handler) {
    impl_->require_configurable();
    impl_->configuration_.on_error(detail::own_callback(std::move(handler)));
    return *this;
}

test_app& test_app::on_not_found(http_not_found_handler handler) {
    impl_->require_configurable();
    impl_->configuration_.on_not_found(detail::own_callback(std::move(handler)));
    return *this;
}

test_app& test_app::on_error(std::nullptr_t) {
    impl_->require_configurable();
    // test_app holds no configuration lock, so the old handler may die here.
    (void)impl_->configuration_.on_error(nullptr);
    return *this;
}

test_app& test_app::on_not_found(std::nullptr_t) {
    impl_->require_configurable();
    (void)impl_->configuration_.on_not_found(nullptr);
    return *this;
}

test_app& test_app::on_error(scoped_error_handler_options options) {
    impl_->require_configurable();
    impl_->configuration_.on_error(options.prefix_, detail::own_callback(std::move(options.handler_)));
    return *this;
}

test_app& test_app::on_not_found(scoped_not_found_handler_options options) {
    impl_->require_configurable();
    impl_->configuration_.on_not_found(options.prefix_, detail::own_callback(std::move(options.handler_)));
    return *this;
}

test_app& test_app::use_middleware(detail::owned_middleware_descriptor descriptor) {
    impl_->require_configurable();
    impl_->configuration_.add_middleware(std::move(descriptor));
    return *this;
}

test_app& test_app::use_worker_state_definition(detail::worker_state_definition definition) {
    impl_->require_configurable();
    impl_->configuration_.add_worker_state(std::move(definition));
    return *this;
}

dispatch_response test_app::request(const test_request& request) {
    impl_->finalize();

    return impl_->event_loop_.start(impl_->execute_request(request)).get();
}

}  // namespace ruvia

namespace ruvia {
namespace {

void add_shutdown_signals(asio::signal_set& signals) {
    signals.add(SIGINT);
    signals.add(SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
}

void invoke_stop_hooks(detail::app_state& state_value) noexcept {
    for (auto& hook : state_value.on_stop_hooks_) {
        try {
            (*hook)();
        } catch (...) {
            ruvia::report_unhandled_failure("app stop hook", std::current_exception());
        }
    }
}

// Process shutdown signals for one run, on their own core runtime. The first
// signal restores the default disposition and requests the application's
// stop, so a repeated signal is not swallowed while the stop drains -- or
// while a slow startup hook still runs, before the stop can take effect.
class process_signal_handling final {
public:
    explicit process_signal_handling(application& owner)
        : runtime_({.queue_capacity_ = 1, .io_policy_ = worker_io_policy::single_owner}),
          signals_(runtime_.io_context()) {
        runtime_.configure({
            .stop_admission_ = [this] {
                std::error_code ignored;
                signals_.clear(ignored);
                signals_.cancel(ignored);
                runtime_.finalize(); },
            .failure_ = [&owner](std::exception_ptr) noexcept { owner.stop(); },
        });
        // A failed registration or thread start must join the runtime while
        // signals_ is alive: its stop hook still clears and cancels the set,
        // and member unwinding would destroy signals_ before runtime_.
        try {
            add_shutdown_signals(signals_);
            signals_.async_wait([this, &owner](const std::error_code& ec, int) {
                if (!ec) {
                    std::error_code ignored;
                    signals_.clear(ignored);
                    owner.stop();
                }
            });
            // Nothing else can reach this freshly built runtime, so no stop
            // can precede its launch.
            (void)runtime_.start();
        } catch (...) {
            stop();
            throw;
        }
    }

    ~process_signal_handling() {
        stop();
    }

    process_signal_handling(const process_signal_handling&) = delete;
    process_signal_handling& operator=(const process_signal_handling&) = delete;

    void stop() noexcept {
        runtime_.request_stop();
        runtime_.join();
    }

    [[nodiscard]] std::exception_ptr failure() noexcept {
        return runtime_.take_failure();
    }

private:
    worker_runtime runtime_;
    asio::signal_set signals_;
};

class app_run_coordinator final {
public:
    app_run_coordinator(application& owner_value, detail::app_state& state_value)
        : owner_(owner_value),
          state_(state_value) {}

    void run() {
        begin_run();

        try {
            build_and_publish_runtime();
        } catch (...) {
            // Rethrown to the run() caller, who owns reporting it.
            complete_unpublished_run();
            throw;
        }

        auto& group = state_.runtime_->group_;
        std::exception_ptr primary_failure;
        try {
            if (!stop_requested()) {
                start_signal_handling();
                // Two-phase start: every worker is ready (and on Windows the
                // acceptor holds every listener) before hooks; nothing
                // accepts until every hook has succeeded.
                bool started = group.start();
#if defined(_WIN32)
                started = started && state_.runtime_->acceptor_->start();
#endif
                if (started) {
                    publish_ready_workers();
                    run_start_hooks();
                    bool serving = !stop_requested() && group.serve();
#if defined(_WIN32)
                    serving = serving && state_.runtime_->acceptor_->serve();
#endif
                    if (serving) {
                        wait_for_stop();
                    }
                }
            }
        } catch (...) {
            primary_failure = std::current_exception();
            owner_.stop();
        }

        stop_signal_handling();
        // Direct disconnect, with no drain phase: admission stops, then every
        // worker's stop closes all of its connections at once. Stop hooks run
        // after admission closes and before any worker is finalized.
#if defined(_WIN32)
        auto& acceptor = *state_.runtime_->acceptor_;
        acceptor.stop_admission();
#endif
        group.request_stop();
        invoke_stop_hooks(state_);
        // The rethrown failure is chosen before the group reports the member
        // failures it supersedes, so the library reports no failure twice and
        // never the rethrown one, whose reporting belongs to the run() caller.
        const auto signal_failure = signals_.has_value() ? signals_->failure() : nullptr;
        auto rethrown = group.join(primary_failure != nullptr ? primary_failure : signal_failure);
#if defined(_WIN32)
        // The workers sent their last datagrams (QUIC CONNECTION_CLOSE)
        // through the acceptor until they joined; only now do its sockets close.
        acceptor.request_stop();
        acceptor.join();
        const auto acceptor_failure = acceptor.failure();
        if (rethrown == nullptr) {
            rethrown = acceptor_failure;
        }
#endif
        retire_runtime();
        if (signal_failure != nullptr && !ruvia::equivalent_failures(signal_failure, rethrown)) {
            ruvia::report_unhandled_failure("app signal handling", signal_failure);
        }
#if defined(_WIN32)
        if (acceptor_failure != nullptr && !ruvia::equivalent_failures(acceptor_failure, rethrown)) {
            ruvia::report_unhandled_failure("app acceptor", acceptor_failure);
        }
#endif
        if (rethrown != nullptr) {
            std::rethrow_exception(rethrown);
        }
    }

private:
    void begin_run() {
        std::lock_guard lock(state_.mutex_);
        detail::ensure_app_not_running(state_.lifecycle_.active(), "app is already running");
        if (!state_.lifecycle_.begin_run()) {
            std::terminate();
        }
    }

    void build_and_publish_runtime() {
        const bool routes_enabled = std::ranges::any_of(state_.listeners_, [](const auto& listener) {
            return !listener.connection_.has_value() && !listener.request_stream_ &&
                   !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(listener.transport_);
        });
        std::vector<detail::controller_registrar> controller_registrars;
        if (routes_enabled) {
            controller_registrars = detail::seal_controller_registrars();
        }
        auto runtime = std::make_unique<detail::app_runtime_graph>();
        runtime->routes_enabled_ = routes_enabled;
        auto prepared_options = state_.options_;
        prepared_options.env_ = &state_.env_;
        prepared_options.failure_ = detail::worker_failure_sink{
            .target_ = &owner_,
            .invoke_ =
                [](void* target, const std::exception_ptr&) noexcept {
                    static_cast<application*>(target)->stop();
                },
        };

        if (state_.document_root_config_.has_value()) {
            // Refreshing the index reads the filesystem on the blocking pool.
            if (!state_.blocking_pool_.has_value()) {
                throw std::invalid_argument("document root refresh cannot run while the blocking pool is disabled");
            }
            const auto& document_root = *state_.document_root_config_;
            // Precompressed variants are negotiated by the compression policy,
            // so without it the refresher never builds them.
            const auto precompression = prepared_options.compression_.has_value()
                                            ? document_root.precompression_
                                            : detail::static_root_precompression_options{};
            runtime->document_root_ = std::make_unique<detail::document_root_publisher>(
                http_make_path_from_native_path(document_root.root_), document_root.static_options_,
                document_root.refresh_interval_, precompression, document_root.resident_,
                state_.options_.server_.worker_count_);
            prepared_options.document_root_ =
                detail::http_server_options::document_root_state::refreshing(*runtime->document_root_);
        }
        if (state_.blocking_pool_.has_value()) {
            runtime->blocking_pool_ =
                std::make_unique<blocking_pool>(*state_.blocking_pool_);
        }

        const auto validated_configuration =
            detail::validate_http_server_configuration(state_.listeners_, std::move(prepared_options));
        // Workers number the listeners, and their HTTP/3 services, in this order.
        runtime->listener_slots_.reserve(validated_configuration.listeners().size());
        for (const auto& listener : validated_configuration.listeners()) {
            runtime->listener_slots_.emplace_back(listener.endpoint_);
            runtime->http3_slot_count_ += listener.http3_.has_value() ? 1 : 0;
        }
        runtime->tls_key_origin_ = validated_configuration.tls_key_origin();

        // Non-routed listeners borrow an explicitly empty plan: unused global
        // controllers, routes and middleware must neither be sealed nor built.
        detail::controller_store first_controllers(controller_registrars);
        if (routes_enabled) {
            runtime->route_plan_ = detail::compile_route_plan(
                controller_registrars, first_controllers, state_.configuration_);
        } else {
            runtime->route_plan_ = std::make_unique<detail::compiled_route_plan>();
        }
#if defined(RUVIA_ENABLE_HTTP3)
        if (std::ranges::any_of(validated_configuration.listeners(),
                [](const auto& listener) { return listener.http3_.has_value(); })) {
            runtime->http3_directory_ = std::make_unique<detail::http3_worker_directory>(
                state_.options_.server_.worker_count_);
        }
#endif
#if defined(_WIN32)
        runtime->acceptor_ = std::make_unique<detail::windows_acceptor>(
            validated_configuration.listeners(), state_.options_.server_.worker_count_,
#if defined(RUVIA_ENABLE_HTTP3)
            runtime->http3_directory_.get(),
#endif
            validated_configuration.options().failure_);
#endif
        runtime->workers_.reserve(state_.options_.server_.worker_count_);
        for (std::size_t i = 0; i < state_.options_.server_.worker_count_; ++i) {
            auto controllers =
                i == 0 ? std::move(first_controllers) : detail::controller_store(controller_registrars);
            auto routes_value = std::make_unique<detail::route_table>(
                *runtime->route_plan_, controllers);
            const detail::worker_capability_definitions capabilities{
#ifdef RUVIA_ENABLE_DATABASE
                .databases_ = std::span<const detail::db_definition>(state_.databases_),
#endif
#ifdef RUVIA_ENABLE_REDIS
                .redis_ = std::span<const detail::redis_definition_type>(state_.redis_),
#endif
                .worker_states_ = state_.configuration_.worker_states(),
                .http_clients_ = std::span<const detail::http_client_definition_type>(state_.http_clients_),
            };
#if defined(RUVIA_ENABLE_HTTP3)
            auto worker_value = std::make_unique<detail::web_worker_runtime>(
                validated_configuration, *routes_value, capabilities, runtime->blocking_pool_.get(), i,
                detail::http3_worker_placement{
                    .directory_ = runtime->http3_directory_.get(),
                    .worker_index_ = i,
#if defined(_WIN32)
                    .acceptor_ = runtime->acceptor_.get(),
#endif
                });
            if (runtime->http3_directory_ != nullptr) {
                runtime->http3_directory_->bind_worker(i, worker_value->runtime().worker());
            }
#else
            auto worker_value = std::make_unique<detail::web_worker_runtime>(
                validated_configuration, *routes_value, capabilities, runtime->blocking_pool_.get(), i);
#endif
#if defined(_WIN32)
            runtime->acceptor_->bind_worker(i, worker_value->acceptor_target());
#endif
            runtime->workers_.emplace_back(
                std::move(controllers), std::move(routes_value), std::move(worker_value));
        }

        for (auto& slot : runtime->workers_) {
            runtime->group_.add(slot.runtime_->runtime());
        }

        std::lock_guard lock(state_.mutex_);
        state_.runtime_ = std::move(runtime);
        if (!state_.lifecycle_.publish_runtime() && !state_.lifecycle_.stop_requested()) {
            std::terminate();
        }
    }

    void complete_unpublished_run() noexcept {
        release_inherited_listener_sockets();
        std::lock_guard lock(state_.mutex_);
        if (state_.runtime_ != nullptr) {
            std::terminate();
        }
        state_.lifecycle_.complete_run();
    }

    // The run consumed every inherited listener socket, whatever its outcome,
    // so later runs bind; unclaimed ones close with their last owner. Only
    // the run() owner touches the stored listeners while the app is neither
    // stopped nor running.
    void release_inherited_listener_sockets() noexcept {
        for (auto& listener : state_.listeners_) {
            listener.inherited_sockets_.reset();
        }
    }

    [[nodiscard]] bool stop_requested() const {
        std::lock_guard lock(state_.mutex_);
        return state_.lifecycle_.stop_requested();
    }

    void start_signal_handling() {
        if (state_.options_.server_.process_signal_handlers_ == process_signal_handler_policy::install) {
            signals_.emplace(owner_);
        }
    }

    void publish_ready_workers() {
        std::lock_guard lock(state_.mutex_);
        state_.runtime_->workers_ready_ = true;
    }

    void run_start_hooks() {
        for (auto& hook : state_.on_start_hooks_) {
            if (stop_requested()) {
                return;
            }
            (*hook)();
        }
    }

    void wait_for_stop() {
        std::unique_lock lock(state_.mutex_);
        if (!state_.lifecycle_.mark_running()) {
            return;
        }
        state_.lifecycle_changed_.wait(lock, [this] { return state_.lifecycle_.stop_requested(); });
    }

    void stop_signal_handling() noexcept {
        if (signals_.has_value()) {
            signals_->stop();
        }
    }

    void retire_runtime() noexcept {
        std::unique_ptr<detail::app_runtime_graph> retired;
        {
            // run()/stop() remain synchronous main-thread lifecycle drivers.
            // Controls suspend their callers; only this owner waits to retire
            // their borrowed runtime and frozen configuration after joining.
            std::unique_lock lock(state_.mutex_);
            state_.lifecycle_changed_.wait(lock, [this] { return state_.runtime_controls_ == 0; });
            retired = std::move(state_.runtime_);
        }
        retired.reset();
        release_inherited_listener_sockets();
        {
            std::lock_guard lock(state_.mutex_);
            state_.lifecycle_.complete_run();
        }
    }

    application& owner_;
    detail::app_state& state_;
    // Constructed only for process_signal_handler_policy::install.
    std::optional<process_signal_handling> signals_;
};

}  // namespace

void detail::run_app(application& app, app_state& state_value) {
    app_run_coordinator(app, state_value).run();
}

void detail::request_runtime_stop(app_state& state_value) noexcept {
    if (state_value.runtime_ != nullptr) {
        state_value.runtime_->group_.request_stop();
#if defined(_WIN32)
        state_value.runtime_->acceptor_->request_stop();
#endif
    }
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_ref& require_worker_capabilities_worker(const worker_ref& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("worker capabilities require a valid worker");
    }
    return worker_value;
}

[[nodiscard]] worker_capability_options require_worker_capability_options(worker_capability_options options) {
    if (options.connection_failure_ == nullptr) {
        throw std::invalid_argument("worker capabilities require a connection failure outlet");
    }
    return options;
}

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
[[nodiscard]] operation_options context_operation_options(const stop_token& stop_token_value) {
    return operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value};
}
#endif
}  // namespace

worker_capabilities::worker_capabilities(asio::io_context& io_context, const worker_ref& worker_value,
    worker_capability_definitions definitions,
    worker_capability_options options)
    : worker_(require_worker_capabilities_worker(worker_value)),
#ifdef RUVIA_ENABLE_REDIS
      redis_(io_context, definitions.redis_, worker_),
#endif
#ifdef RUVIA_ENABLE_DATABASE
      databases_(io_context, worker_, definitions.databases_
#ifdef RUVIA_ENABLE_REDIS
          ,
          &redis_
#else
          ,
          nullptr
#endif
          ),
#endif
      http_client_services_(make_http_client_worker_services(options.http_client_worker_)),
      http_clients_(io_context, worker_,
          definitions.http_clients_, http_client_services_),
      worker_states_(definitions.worker_states_),
      rate_limiter_(
          options.default_rate_limit_, options.route_rate_limits_, options.rate_limit_capacity_),
      options_(require_worker_capability_options(std::move(options))),
      worker_services_{
          .worker_ = worker_,
          .clients_ = client_registries(),
          .rate_limiter_ = &rate_limiter_,
          .env_ = options_.env_,
          .max_decoded_body_bytes_ = options_.max_decoded_body_bytes_,
          .states_ = &worker_states_,
          .precompressed_static_files_ = options_.precompressed_static_files_,
          .file_io_timeout_ = options_.file_io_timeout_,
          .connection_failure_ = options_.connection_failure_,
          .trusted_proxies_ = options_.trusted_proxies_,
      } {
}

task<void> worker_capabilities::start() {
    worker_states_.initialize();
    try {
#ifdef RUVIA_ENABLE_REDIS
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
#endif
#ifdef RUVIA_ENABLE_DATABASE
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
#endif
    } catch (...) {
        close_now();
        worker_states_.shutdown();
        throw;
    }
}

void worker_capabilities::close_now() noexcept {
    http_clients_.close_now();
#ifdef RUVIA_ENABLE_DATABASE
    databases_.close_now();
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_.close_now();
#endif
}

task<void> worker_capabilities::stop() {
    close_now();
    std::exception_ptr failure;
    try {
        co_await http_clients_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    worker_states_.shutdown();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

context_services worker_capabilities::make_context_services(const stop_token& stop_token_value) {
    return context_services(worker_services_, stop_token_value);
}

worker_client_registry_view worker_capabilities::client_registries() noexcept {
    return worker_client_registry_view(http_clients_
#ifdef RUVIA_ENABLE_DATABASE
        ,
        databases_
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_
#endif
    );
}

const worker_state_registry& worker_capabilities::worker_states() const noexcept {
    return worker_states_;
}

namespace {

void require_borrowed_client_worker(const worker_ref& router_worker, const worker_ref& client_worker) {
    if (client_worker.id() != router_worker.id()) {
        throw std::invalid_argument("a router borrows only clients bound to its own worker");
    }
}

template <typename entry_type, typename client_type>
void bind_borrowed_alias(std::vector<entry_type>& entries, std::string_view alias, const client_type& client) {
    validate_capability_alias(alias, "borrowed client alias must not be empty");
    for (const auto& entry : entries) {
        if (std::string_view(entry.alias_) == alias) {
            throw std::invalid_argument("borrowed client alias is already bound");
        }
    }
    entries.push_back(entry_type{std::string(alias), &client});
}

template <typename entry_type>
[[nodiscard]] auto find_borrowed_alias(const std::vector<entry_type>& entries, std::string_view alias) noexcept
    -> decltype(entries.front().client_) {
    for (const auto& entry : entries) {
        if (std::string_view(entry.alias_) == alias) {
            return entry.client_;
        }
    }
    return nullptr;
}

}  // namespace

borrowed_clients::borrowed_clients() = default;
bool borrowed_clients::empty() const noexcept {
    return http_clients_.empty()
#ifdef RUVIA_ENABLE_DATABASE
           && databases_.empty()
#endif
#ifdef RUVIA_ENABLE_REDIS
           && redis_.empty()
#endif
        ;
}

void borrowed_clients::bind_http_client(const worker_ref& worker, std::string_view alias, const http_client& client) {
    require_borrowed_client_worker(worker, client.worker());
    bind_borrowed_alias(http_clients_, alias, client);
}

const http_client* borrowed_clients::http_clients(std::string_view alias) const noexcept {
    return find_borrowed_alias(http_clients_, alias);
}

#ifdef RUVIA_ENABLE_DATABASE
void borrowed_clients::bind_database(const worker_ref& worker, std::string_view alias, const db_client& client) {
    require_borrowed_client_worker(worker, client.worker());
    bind_borrowed_alias(databases_, alias, client);
}

const db_client* borrowed_clients::database(std::string_view alias) const noexcept {
    return find_borrowed_alias(databases_, alias);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
void borrowed_clients::bind_redis(const worker_ref& worker, std::string_view alias, const redis_client& client) {
    require_borrowed_client_worker(worker, client.worker());
    bind_borrowed_alias(redis_, alias, client);
}

const redis_client* borrowed_clients::redis(std::string_view alias) const noexcept {
    return find_borrowed_alias(redis_, alias);
}
#endif

void* worker_context_capabilities::worker_state_instance(const void* type_key) const {
    auto* instance = worker_states_ == nullptr ? nullptr : worker_states_->instance(type_key);
    if (instance == nullptr) {
        throw std::logic_error(
            "worker state type is not registered: call application::use_worker_state<state_type>() or router::bind_worker_state()");
    }
    return instance;
}

#ifdef RUVIA_ENABLE_DATABASE
db_session worker_client_registry_view::db(::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return db(default_capability_alias, operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}

db_session worker_client_registry_view::db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        const auto* client = borrowed_->database(alias);
        if (client == nullptr) {
            throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
        }
        return client->db().bind_caller_scope(operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(alias, operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session worker_client_registry_view::redis(::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return redis(default_capability_alias, operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}

redis_session worker_client_registry_view::redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        const auto* client = borrowed_->redis(alias);
        if (client == nullptr) {
            throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
        }
        return client->redis().bind_caller_scope(operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(alias, operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}
#endif

http_client_ref worker_client_registry_view::client(
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return client(default_capability_alias, operation_scope, stop_token_value);
    }
    if (!attached()) {
        throw http_client_error(
            make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return http_clients_->get(operation_scope, stop_token_value);
}

http_client_ref worker_client_registry_view::client(std::string_view alias,
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return borrowed_http_clients(alias).client().bind_caller_scope(operation_scope, stop_token_value);
    }
    if (!attached()) {
        throw http_client_error(
            make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return http_clients_->get(alias, operation_scope, stop_token_value);
}
const http_client& worker_client_registry_view::borrowed_http_clients(std::string_view alias) const {
    const auto* clients = borrowed_->http_clients(alias);
    if (clients == nullptr) {
        throw http_client_error(
            make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return *clients;
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

template <typename variables_type>
[[nodiscard]] auto find_variable_slot(variables_type& variables, std::string_view name) noexcept {
    return std::ranges::lower_bound(
        variables, name, std::ranges::less{}, [](const detail::env_variable& variable) noexcept {
            return std::string_view(variable.name_);
        });
}

}  // namespace

env::env()
    : state_(std::make_unique<detail::env_state>()) {}

env::~env() = default;

std::optional<std::string_view> env::get(std::string_view name) const& noexcept {
    const auto& variables = state_->variables_;
    const auto it = find_variable_slot(variables, name);
    if (it == variables.end() || std::string_view(it->name_) != name) {
        return std::nullopt;
    }

    return std::string_view(it->value_);
}

std::optional<bool> env::parse_bool_value(std::string_view value) noexcept {
    if (value == "1") {
        return true;
    }
    if (value == "0") {
        return false;
    }

    // ASCII-only case fold via the shared owner: the boolean tokens are ASCII, and
    // std::tolower is locale-dependent (a non-"C" LC_CTYPE set by the host app could
    // fold bytes unexpectedly).
    if (http_ascii_equals_ignore_case(value, "true") ||
        http_ascii_equals_ignore_case(value, "yes") ||
        http_ascii_equals_ignore_case(value, "on")) {
        return true;
    }
    if (http_ascii_equals_ignore_case(value, "false") ||
        http_ascii_equals_ignore_case(value, "no") ||
        http_ascii_equals_ignore_case(value, "off")) {
        return false;
    }

    return std::nullopt;
}

bool env::loaded() const noexcept {
    return state_->loaded_;
}

std::size_t env::size() const noexcept {
    return state_->variables_.size();
}

std::filesystem::path detail::dotenv_executable_directory() {
#ifdef _WIN32
    std::wstring buffer(260, L'\0');
    for (;;) {
        const auto length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(1024);
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(
                std::string_view(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::string detail::dotenv_path_message(std::string_view prefix, const std::filesystem::path& path) {
    const auto name = path.u8string();
    std::string result_value(prefix);
    result_value.append(reinterpret_cast<const char*>(name.data()), name.size());
    return result_value;
}

env& env::load_dotenv(dotenv_options options) & {
    return load_dotenv(detail::dotenv_executable_directory() / ".env", options);
}

env& env::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
    if (options.existing_variables_ != dotenv_existing_variable_policy::preserve &&
        options.existing_variables_ != dotenv_existing_variable_policy::override) {
        throw std::invalid_argument("dotenv existing variable policy is invalid");
    }
    if (options.missing_file_ != dotenv_missing_file_policy::ignore &&
        options.missing_file_ != dotenv_missing_file_policy::require) {
        throw std::invalid_argument("dotenv missing file policy is invalid");
    }
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            throw std::runtime_error(detail::dotenv_path_message("failed to read dotenv file: ", path).c_str());
        }
        if (options.missing_file_ == dotenv_missing_file_policy::require) {
            throw std::runtime_error(detail::dotenv_path_message("dotenv file not found: ", path).c_str());
        }
        return *this;
    }

    auto entries = detail::read_dotenv_entries(input, path);
    auto& variables = state_->variables_;
    if (!entries.empty()) {
        variables.reserve(variables.size() + entries.size());
    }

    for (auto& entry : entries) {
        auto it = find_variable_slot(variables, entry.name_);
        if (it != variables.end() && std::string_view(it->name_) == std::string_view(entry.name_)) {
            if (options.existing_variables_ == dotenv_existing_variable_policy::override) {
                it->value_ = std::move(entry.value_);
            }
            continue;
        }

        variables.insert(it, std::move(entry));
    }

    state_->loaded_ = true;
    return *this;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r';
}

[[nodiscard]] std::string_view trim_left(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim_right(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    return trim_right(trim_left(value));
}

[[nodiscard]] bool is_valid_key(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }

    // Explicit ASCII checks rather than std::isalpha/isalnum: an environment
    // variable name is ASCII by definition, whereas the <cctype> predicates are
    // locale-dependent (a non-"C" LC_CTYPE set by the host app could admit high
    // bytes into a key). This also matches how the rest of the codebase validates
    // identifiers (is_valid_session_id, detail::ensure_config_host, ...).
    const auto is_ascii_alpha = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!(is_ascii_alpha(key.front()) || key.front() == '_')) {
        return false;
    }

    return std::ranges::all_of(key.substr(1), [&is_ascii_alpha](char value) {
        return is_ascii_alpha(value) || (value >= '0' && value <= '9') || value == '_';
    });
}

[[nodiscard]] std::string location_message(
    const std::filesystem::path& path, std::size_t line_number, std::string_view message) {
    auto result_value = dotenv_path_message("invalid dotenv entry in ", path);
    result_value += ':';
    result_value += std::to_string(line_number);
    result_value += ": ";
    result_value += message;
    return result_value;
}

[[nodiscard]] std::size_t dotenv_utf8_sequence_length(std::string_view value, std::size_t index,
    const std::filesystem::path& path, std::size_t line_number) {
    const auto sequence = ::ruvia::scan_utf8_sequence(value, index);
    if (!sequence.well_formed_) {
        throw std::invalid_argument(
            location_message(path, line_number, "value is not valid UTF-8").c_str());
    }
    return sequence.length_;
}

// The complete set of escapes inside double quotes. As in POSIX sh double
// quotes, a backslash before any other character is not an escape: the
// backslash and that character are both kept literally.
[[nodiscard]] std::optional<char> decode_double_quoted_escape(char escaped) noexcept {
    switch (escaped) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case '"':
            return '"';
        case '\\':
            return '\\';
        default:
            return std::nullopt;
    }
}

[[nodiscard]] std::string parse_double_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    std::string result;
    std::size_t literal_start = 1;
    bool has_escape = false;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char current = value[index];
        if (static_cast<unsigned char>(current) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
            continue;
        }
        if (current == '"') {
            consumed = index + 1;
            if (!has_escape) {
                return std::string(value.substr(1, index - 1));
            }
            result.append(value.substr(literal_start, index - literal_start));
            return result;
        }
        if (current == '\\') {
            if (index + 1 == value.size()) {
                throw std::invalid_argument(
                    location_message(path, line_number, "unfinished escape sequence").c_str());
            }
            has_escape = true;
            result.append(value.substr(literal_start, index - literal_start));
            const char escaped = value[++index];
            if (const auto decoded = decode_double_quoted_escape(escaped); decoded.has_value()) {
                result.push_back(*decoded);
            } else {
                result.push_back('\\');
                const auto length = static_cast<unsigned char>(escaped) >= 0x80
                                        ? dotenv_utf8_sequence_length(value, index, path, line_number)
                                        : 1;
                result.append(value.substr(index, length));
                index += length - 1;
            }
            literal_start = index + 1;
        }
    }
    throw std::invalid_argument(
        location_message(path, line_number, "unterminated double-quoted value").c_str());
}

[[nodiscard]] std::string parse_single_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    for (std::size_t index = 1; index < value.size(); ++index) {
        if (value[index] == '\'') {
            consumed = index + 1;
            return std::string(value.substr(1, index - 1));
        }
        if (static_cast<unsigned char>(value[index]) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
        }
    }
    throw std::invalid_argument(
        location_message(path, line_number, "unterminated single-quoted value").c_str());
}

void validate_quoted_remainder(
    std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (!value.empty() && value.front() != '#') {
        throw std::invalid_argument(
            location_message(path, line_number, "unexpected characters after quoted value").c_str());
    }
}

[[nodiscard]] std::string parse_unquoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number) {
    std::size_t end = value.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '#' && (index == 0 || is_space(value[index - 1]))) {
            end = index;
            break;
        }
        if (static_cast<unsigned char>(value[index]) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
        }
    }

    return std::string(trim(value.substr(0, end)));
}

[[nodiscard]] std::string parse_value(
    std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (value.empty()) {
        return {};
    }

    if (value.front() == '"') {
        std::size_t consumed = 0;
        auto parsed_value = parse_double_quoted_value(value, path, line_number, consumed);
        validate_quoted_remainder(value.substr(consumed), path, line_number);
        return parsed_value;
    }

    if (value.front() == '\'') {
        std::size_t consumed = 0;
        auto parsed_value = parse_single_quoted_value(value, path, line_number, consumed);
        validate_quoted_remainder(value.substr(consumed), path, line_number);
        return parsed_value;
    }

    return parse_unquoted_value(value, path, line_number);
}

void strip_utf8_bom(std::string_view& line) noexcept {
    constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == bom[0] &&
        static_cast<unsigned char>(line[1]) == bom[1] &&
        static_cast<unsigned char>(line[2]) == bom[2]) {
        line.remove_prefix(3);
    }
}

[[nodiscard]] env_variable parse_entry(
    std::string_view line, const std::filesystem::path& path, std::size_t line_number) {
    line = trim(line);
    if (line.starts_with("export") && line.size() > 6 && is_space(line[6])) {
        line = trim(line.substr(6));
    }

    const auto separator = line.find('=');
    if (separator == std::string_view::npos) {
        throw std::invalid_argument(location_message(path, line_number, "missing '='").c_str());
    }

    const auto key = trim(line.substr(0, separator));
    if (!is_valid_key(key)) {
        throw std::invalid_argument(
            location_message(path, line_number, "invalid variable name").c_str());
    }

    auto parsed_value = parse_value(line.substr(separator + 1), path, line_number);
    env_variable entry;
    entry.name_.assign(key.data(), key.size());
    entry.value_ = std::move(parsed_value);
    return entry;
}

}  // namespace

std::vector<env_variable> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path) {
    std::vector<env_variable> entries;
    // Indices into entries ordered by name: duplicate detection stays
    // O(log N) per line and never borrows a name that a reallocation of
    // entries could move (small-string storage relocates with the element).
    std::vector<std::size_t> entries_by_name;
    const auto entry_name = [&entries](std::size_t index) noexcept {
        return std::string_view(entries[index].name_);
    };
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;

        std::string_view view(line);
        if (line_number == 1) {
            strip_utf8_bom(view);
        }

        view = trim(view);
        if (view.empty() || view.front() == '#') {
            continue;
        }

        auto entry = parse_entry(view, path, line_number);
        // One file assigning a name twice is ambiguous configuration; the
        // existing-variable policy only arbitrates against other sources.
        const auto slot = std::ranges::lower_bound(
            entries_by_name, std::string_view(entry.name_), std::ranges::less{}, entry_name);
        if (slot != entries_by_name.end() && entry_name(*slot) == std::string_view(entry.name_)) {
            throw std::invalid_argument(
                location_message(path, line_number, "duplicate variable name").c_str());
        }
        entries_by_name.insert(slot, entries.size());
        entries.push_back(std::move(entry));
    }

    if (input.bad()) {
        throw std::runtime_error(dotenv_path_message("failed to read dotenv file: ", path).c_str());
    }

    return entries;
}

}  // namespace ruvia::detail
