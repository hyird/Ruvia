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
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/middleware.h"

#include "context_access.h"
#include "route_table.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

#include <fstream>
#include <istream>
#include <system_error>

#include "ruvia/core/stop_token.h"
#include "ruvia/http/borrowed_text.h"

#include "route_table.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"

#include "route_table.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"

#include "route_table.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <type_traits>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/crypto.h>

#ifdef RUVIA_ENABLE_DATABASE
#include "route_table.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "route_table.h"
#endif

#include <cerrno>
#include <cstring>
#include <limits>

#include <asio/bind_executor.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>

#include "ruvia/core/task_scope.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

namespace ruvia {

application& application::load_dotenv(dotenv_options options) & {
    return detail::mutate_stopped_app(
        *this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
            state_value.env_.load_dotenv(options);
        });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running",
        [&](detail::app_state& state_value) { state_value.env_.load_dotenv(path, options); });
}

namespace {
void cleanse_secret(std::string& secret) noexcept {
    OPENSSL_cleanse(secret.data(), secret.size());
    secret.clear();
}

void cleanse_identity(tls_identity_config& identity) noexcept {
    cleanse_secret(identity.private_key_password_);
    cleanse_secret(identity.private_key_pem_);
    cleanse_secret(identity.certificate_chain_pem_);
}

[[nodiscard]] asio::ip::address normalize_listen_address(std::string_view address) {
    if (address.empty()) {
        throw std::invalid_argument("listen address must not be empty");
    }
    const auto normalized = ruvia::parse_ip_address(address);
    if (!normalized) {
        throw std::invalid_argument("listen address must be a numeric IP address");
    }
    return *normalized;
}

[[nodiscard]] bool has_tls_configuration(const tls_config& config) noexcept {
    const auto& identity = config.identity_;
    return !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() ||
           !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() ||
           !identity.private_key_password_.empty() || config.client_certificates_.ca_file_.has_value() ||
           config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional ||
           !config.sni_.empty() || !config.local_addresses_.empty() ||
           config.certificate_store_ != nullptr || static_cast<bool>(config.select_certificate_) ||
           config.default_certificate_ != 0;
}

// listen() and replace_tls_identities() own their tls_config, the only copy
// of each private-key password and in-memory PEM text the application ever
// holds: credentials are loaded from it and every such secret is cleansed
// when the call returns or throws.
class tls_secret_cleanser final {
public:
    explicit tls_secret_cleanser(tls_config& config) noexcept
        : config_(config) {}
    ~tls_secret_cleanser() {
        cleanse_identity(config_.identity_);
        for (auto& sni : config_.sni_) {
            cleanse_identity(sni.identity_);
        }
        for (auto& local : config_.local_addresses_) {
            cleanse_identity(local.identity_);
        }
    }

    tls_secret_cleanser(const tls_secret_cleanser&) = delete;
    tls_secret_cleanser& operator=(const tls_secret_cleanser&) = delete;
    tls_secret_cleanser(tls_secret_cleanser&&) = delete;
    tls_secret_cleanser& operator=(tls_secret_cleanser&&) = delete;

private:
    tls_config& config_;
};

// Validates listen_config::dispatch_ and tls_.alpn_. For a connection-dispatch
// listener, returns the ALPN offer its HTTPS service makes, in preference
// order, as an RFC 7301 §3.1 ProtocolNameList without its length prefix.
[[nodiscard]] std::optional<std::string> connection_dispatch_alpn(const listen_config& config) {
    const auto& protocols = config.tls_.alpn_;
    switch (config.dispatch_.value_or(listener_dispatch::routes)) {
        case listener_dispatch::routes:
        case listener_dispatch::request_stream:
            if (!protocols.empty()) {
                throw std::invalid_argument("tls_config::alpn_ requires listener_dispatch::connection");
            }
            return std::nullopt;
        case listener_dispatch::connection:
            break;
        default:
            throw std::invalid_argument("listener dispatch is invalid");
    }
    if (config.http3_.has_value()) {
        throw std::invalid_argument("listener_dispatch::connection serves no HTTP/3 port");
    }
    if (config.auto_https_redirect_) {
        throw std::invalid_argument("listener_dispatch::connection cannot redirect HTTP to HTTPS");
    }
    if (!config.https_.has_value() && !protocols.empty()) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (protocols.empty()) {
        return std::string("\x02h2\x08http/1.1", 12);
    }
    std::string encoded;
    for (const auto& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("ALPN protocol names must be 1 to 255 bytes");
        }
        encoded.push_back(static_cast<char>(protocol.size()));
        encoded.append(protocol);
    }
    return encoded;
}

void append_listen_config(
    std::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
    const auto address = normalize_listen_address(config.address_.view());
    if (config.ipv6_only_ && !address.is_v6()) {
        throw std::invalid_argument("IPV6_V6ONLY requires an IPv6 listen address");
    }
    if (!config.http_.has_value() && !config.https_.has_value()) {
        throw std::invalid_argument("listen config must enable HTTP, HTTPS, or both");
    }
    detail::ensure_non_zero_optional_port(config.http_, "HTTP listen port must not be zero");
    detail::ensure_non_zero_optional_port(config.https_, "HTTPS listen port must not be zero");
    const bool mixed = config.http_.has_value() && config.http_ == config.https_;
    if (config.auto_https_redirect_ &&
        (!config.http_.has_value() || !config.https_.has_value())) {
        throw std::invalid_argument(
            "automatic HTTPS redirect requires both HTTP and HTTPS ports");
    }

    if (!config.https_.has_value() && has_tls_configuration(config.tls_)) {
        throw std::invalid_argument("TLS config requires an HTTPS listen port");
    }
    if (config.proxy_protocol_ && config.http3_) {
        throw std::invalid_argument("PROXY protocol is not supported on HTTP/3 listeners");
    }
    const auto connection_alpn = connection_dispatch_alpn(config);
    std::optional<detail::http_server_listener_definition::http3_service> http3;
    if (config.http3_.has_value()) {
#if defined(RUVIA_ENABLE_HTTP3)
        detail::ensure_non_zero_optional_port(config.http3_, "HTTP/3 listen port must not be zero");
        if (!config.https_.has_value()) {
            throw std::invalid_argument("HTTP/3 requires an HTTPS listen port and its TLS config");
        }
        if (config.http3_alt_svc_max_age_.count() <= 0) {
            throw std::invalid_argument("HTTP/3 Alt-Svc max age must be greater than zero");
        }
        // The secret is generated per validated configuration.
        http3.emplace(detail::http_server_listener_definition::http3_service{
            .port_ = config.http3_->value(),
            .alt_svc_max_age_ = config.http3_alt_svc_max_age_,
            .early_data_ = config.http3_early_data_,
            .retry_ = config.http3_retry_,
            .secret_ = {}});
#else
        throw std::invalid_argument("HTTP/3 listen port requires a build with RUVIA_ENABLE_HTTP3");
#endif
    }

    const auto ensure_unique_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        for (const auto& existing : replacement) {
            if (existing.endpoint_ == endpoint) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
        }
    };
    // UDP endpoints are a separate namespace from the TCP ones above.
    const auto ensure_unique_http3_endpoint = [&](std::uint16_t port) {
        for (const auto& existing : replacement) {
            if (existing.http3_.has_value() && existing.http3_->port_ == port &&
                existing.endpoint_.address() == address) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    };

    if (config.http_.has_value() && !mixed) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.http_->value());
        ensure_unique_endpoint(endpoint);
        if (config.auto_https_redirect_) {
            replacement.emplace_back(endpoint,
                detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
        } else {
            replacement.emplace_back(endpoint);
            replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        }
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(std::string_view{});
        }
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
    }
    if (config.https_.has_value()) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.https_->value());
        ensure_unique_endpoint(endpoint);
        if (http3.has_value()) {
            ensure_unique_http3_endpoint(http3->port_);
        }
        // Credentials load once, here; the definition keeps no identity file
        // name, in-memory PEM or password.
        auto tls = detail::load_http_server_tls(config.tls_);
        if (mixed) {
            tls.tcp_mode_ = detail::http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
            if (config.auto_https_redirect_) {
                tls.plaintext_redirect_.emplace(
                    detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
            }
        }
        replacement.emplace_back(endpoint, std::move(tls), std::move(http3));
        replacement.back().ipv6_only_ = config.ipv6_only_;
        replacement.back().proxy_protocol_ = config.proxy_protocol_;
        replacement.back().request_stream_ = config.dispatch_ == listener_dispatch::request_stream;
        if (connection_alpn.has_value()) {
            replacement.back().connection_.emplace(*connection_alpn);
        }
    }
}

class tls_secret_span_cleanser final {
public:
    explicit tls_secret_span_cleanser(std::span<const listen_config> configs) noexcept
        : configs_(configs) {}

    ~tls_secret_span_cleanser() {
        for (const auto& config : configs_) {
            tls_secret_cleanser cleanser(const_cast<tls_config&>(config.tls_));
        }
    }

    tls_secret_span_cleanser(const tls_secret_span_cleanser&) = delete;
    tls_secret_span_cleanser& operator=(const tls_secret_span_cleanser&) = delete;

private:
    std::span<const listen_config> configs_;
};

// Every listen_config::inherited_sockets_ handle of `configs`, in order.
template <typename visitor_type>
void visit_inherited_sockets(std::span<const listen_config> configs, const visitor_type& visit) {
    for (const auto& config : configs) {
        if (!config.inherited_sockets_.has_value()) {
            continue;
        }
        const auto& sockets = *config.inherited_sockets_;
        for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
            for (const auto socket : *service) {
                visit(socket);
            }
        }
    }
}

// A handle given twice would be served, and closed, twice.
void ensure_distinct_inherited_sockets(std::span<const listen_config> configs) {
    visit_inherited_sockets(configs, [configs](native_socket socket) {
        std::size_t occurrences = 0;
        visit_inherited_sockets(configs, [socket, &occurrences](native_socket other) {
            occurrences += other == socket ? 1 : 0;
        });
        if (occurrences != 1) {
            throw std::invalid_argument("a listener socket was handed over twice");
        }
    });
}

void validate_owned_inherited_sockets(std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    for (const auto& config : configs) {
        if (!config.inherited_sockets_) {
            continue;
        }
        const auto address = normalize_listen_address(config.address_.view());
        const auto& sockets = *config.inherited_sockets_;
        const auto tcp = [&](const auto& handles, const auto& port) {
            if (!handles.empty() && !port) {
                throw std::invalid_argument("inherited TCP sockets require their service");
            }
            for (const auto socket : handles) {
                detail::adopt_inherited_tcp_socket(socket, {address, port->value()},
                    "inherited TCP socket does not serve its endpoint", config.ipv6_only_);
            }
        };
        tcp(sockets.http_, config.http_);
        tcp(sockets.https_, config.https_);
        if (!sockets.http3_.empty() && !config.http3_) {
            throw std::invalid_argument("inherited UDP sockets require HTTP/3");
        }
        for (const auto socket : sockets.http3_) {
            detail::adopt_inherited_udp_socket(socket, {address, config.http3_->value()},
                "inherited UDP socket does not serve its endpoint", config.ipv6_only_);
        }
    }
}

[[nodiscard]] bool holds_socket(std::span<const native_socket> sockets, native_socket socket) noexcept {
    return std::ranges::find(sockets, socket) != sockets.end();
}

// A failed listen() or add_listener() closes every handle it was given that
// no definition it built owns -- those close with their definition -- each
// handle once. No run has claimed any of them yet.
void close_unowned_inherited_sockets(std::span<const listen_config> configs,
    std::span<const detail::http_server_listener_definition> built) noexcept {
    std::size_t position = 0;
    visit_inherited_sockets(configs, [&](native_socket socket) {
        const auto current = position++;
        std::size_t earlier = 0;
        bool repeated = false;
        visit_inherited_sockets(configs, [&](native_socket other) {
            repeated = repeated || (earlier++ < current && other == socket);
        });
        const bool owned = std::ranges::any_of(built, [socket](const auto& definition) {
            const auto& inherited = definition.inherited_sockets_;
            return inherited != nullptr && (holds_socket(inherited->tcp(), socket) || holds_socket(inherited->udp(), socket));
        });
        if (!repeated && !owned) {
            detail::close_native_socket(socket);
        }
    });
}

// Hands config.inherited_sockets_ to the definitions append_listen_config()
// built from it, once each socket proved to serve its service.
void adopt_inherited_sockets(std::span<detail::http_server_listener_definition> built, const listen_config& config) {
    if (!config.inherited_sockets_.has_value()) {
        return;
    }
    const auto& sockets = *config.inherited_sockets_;
    if (config.http_.has_value() && config.http_ == config.https_ && !sockets.http_.empty()) {
        throw std::invalid_argument("a mixed HTTP/HTTPS listener inherits its single TCP socket group in https_, not http_");
    }
    if ((!sockets.http_.empty() && !config.http_.has_value()) || (!sockets.https_.empty() && !config.https_.has_value()) ||
        (!sockets.http3_.empty() && !config.http3_.has_value())) {
        throw std::invalid_argument("inherited listener sockets require their service's listen port");
    }
#if defined(_WIN32)
    // The acceptor thread owns one socket per service.
    for (const auto* service : {&sockets.http_, &sockets.https_, &sockets.http3_}) {
        if (service->size() > 1) {
            throw std::invalid_argument("Windows adopts one listener socket per service");
        }
    }
#endif
    for (auto& definition : built) {
        const bool https = std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_);
        const std::span<const native_socket> tcp = https ? sockets.https_ : sockets.http_;
        const auto udp = definition.http3_.has_value() ? std::span<const native_socket>(sockets.http3_)
                                                       : std::span<const native_socket>();
        if (tcp.empty() && udp.empty()) {
            continue;
        }
        for (const auto socket : tcp) {
            detail::adopt_inherited_tcp_socket(socket, definition.endpoint_,
                https ? "inherited HTTPS sockets must listen on the configured address and port"
                      : "inherited HTTP sockets must listen on the configured address and port",
                config.ipv6_only_);
        }
        if (!udp.empty()) {
            const asio::ip::udp::endpoint endpoint(definition.endpoint_.address(), definition.http3_->port_);
            for (const auto socket : udp) {
                detail::adopt_inherited_udp_socket(
                    socket, endpoint, "inherited HTTP/3 sockets must be UDP sockets bound to the configured address and port", config.ipv6_only_);
            }
        }
        definition.inherited_sockets_ = std::make_shared<detail::inherited_listener_sockets>(
            std::vector<native_socket>(tcp.begin(), tcp.end()),
            std::vector<native_socket>(udp.begin(), udp.end()));
    }
}

// The listener definitions `configs` describe, with their inherited sockets.
void append_listen_configs(
    std::vector<detail::http_server_listener_definition>& replacement, std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    std::size_t endpoint_count = 0;
    for (const auto& config : configs) {
        endpoint_count += config.http_.has_value() && config.http_ == config.https_
                              ? 1
                              : static_cast<std::size_t>(config.http_.has_value()) +
                                    static_cast<std::size_t>(config.https_.has_value());
    }
    replacement.reserve(replacement.size() + endpoint_count);
    for (const auto& config : configs) {
        const auto first = replacement.size();
        append_listen_config(replacement, config);
        adopt_inherited_sockets(std::span(replacement).subspan(first), config);
    }
}

// The caller holds the application lock. Awaited controls may originate on
// any worker, including one of this run's own workers.
[[nodiscard]] detail::app_runtime_graph& running_runtime(
    detail::app_state& state_value, const char* not_running_message) {
    if (!state_value.lifecycle_.running() || state_value.runtime_ == nullptr) {
        throw std::logic_error(not_running_message);
    }
    return *state_value.runtime_;
}

class active_app_control final {
public:
    active_app_control(std::shared_ptr<detail::app_state> state, const char* not_running)
        : state_(std::move(state)) {
        std::lock_guard lock(state_->mutex_);
        runtime_ = &running_runtime(*state_, not_running);
        ++state_->runtime_controls_;
    }
    ~active_app_control() {
        std::lock_guard lock(state_->mutex_);
        --state_->runtime_controls_;
        state_->lifecycle_changed_.notify_all();
    }
    active_app_control(const active_app_control&) = delete;
    active_app_control& operator=(const active_app_control&) = delete;
    [[nodiscard]] detail::app_runtime_graph& runtime() const noexcept {
        return *runtime_;
    }

private:
    std::shared_ptr<detail::app_state> state_;
    detail::app_runtime_graph* runtime_{};
};

struct owned_tls_config final {
    explicit owned_tls_config(tls_config config)
        : config_(std::move(config)) {}
    owned_tls_config(owned_tls_config&&) = default;
    ~owned_tls_config() {
        tls_secret_cleanser cleanser(config_);
    }
    tls_config config_;
};

// run_blocking can stop delivering before its callable finishes. A separate
// receipt joins actual execution/destruction, so no accepted preparation or
// exception escapes the runtime control, even when its caller is stopping.
template <typename function_type>
task<std::invoke_result_t<function_type&>> prepare_runtime_control(worker_ref caller, function_type function) {
    using result_type = std::invoke_result_t<function_type&>;
    detail::runtime_control_completion completion(caller);
    auto result = co_await run_blocking(caller, completion.make_command(
                                                    [function = std::move(function)](const auto& receipt) mutable -> result_type {
                                                        try {
                                                            auto prepared = function();
                                                            receipt.complete();
                                                            return prepared;
                                                        } catch (...) {
                                                            receipt.complete(std::current_exception());
                                                            throw;
                                                        }
                                                    }));
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    co_return std::move(result).take_value();
}

// The stored definition of the listener serving `target`. The caller holds
// the application lock.
[[nodiscard]] std::size_t stored_listener_index(const detail::app_state& state_value, const asio::ip::tcp::endpoint& target) {
    for (std::size_t index = 0; index < state_value.listeners_.size(); ++index) {
        if (state_value.listeners_[index].endpoint_ == target) {
            return index;
        }
    }
    throw std::invalid_argument("no listener serves this endpoint");
}

// The slot every worker of the run serves `target` in. The caller holds the
// application lock, and the stored listeners name `target`.
[[nodiscard]] std::size_t runtime_listener_slot(
    const detail::app_runtime_graph& runtime, const asio::ip::tcp::endpoint& target) {
    for (std::size_t slot = 0; slot < runtime.listener_slots_.size(); ++slot) {
        if (runtime.listener_slots_[slot] == target) {
            return slot;
        }
    }
    throw std::logic_error("the running app serves no slot for a stored listener");
}

// The index of the HTTPS listener `target` names in the running app, whose
// identities `config` may replace. The caller holds the application lock.
[[nodiscard]] std::size_t find_replaceable_tls_listener(
    detail::app_state& state_value, const asio::ip::tcp::endpoint& target, const tls_config& config) {
    (void)running_runtime(state_value, "TLS identities can be replaced only while the app is running");
    const auto index = stored_listener_index(state_value, target);
    const auto* tls = std::get_if<detail::http_server_listener_definition::tls>(&state_value.listeners_[index].transport_);
    if (tls == nullptr) {
        throw std::invalid_argument("the listener on this endpoint does not serve HTTPS");
    }
    detail::ensure_same_http_server_tls_policy(*tls, config);
    return index;
}

using control_list = std::vector<move_only_function<void()>>;

// Builds, before anything is posted, one control per worker that runs
// `run(worker, worker_index)` on that worker's thread for `publication`.
template <typename run_type>
void make_worker_controls(control_list& controls, detail::app_runtime_graph& runtime,
    detail::runtime_control_completion& publication, const run_type& run) {
    controls.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        controls.emplace_back(publication.make_command(
            [run, worker = runtime.workers_[index].runtime_.get(), index](const auto& receipt) mutable {
                std::exception_ptr failure;
                try {
                    run(*worker, index);
                } catch (...) {
                    failure = std::current_exception();
                }
                receipt.complete(std::move(failure));
            }));
    }
}

// Posts the controls in worker order and then drops the unposted ones, whose
// receipts wait() would otherwise wait for. False when a worker refused one:
// it refuses only once it stops, and a stopping worker stops the whole app.
[[nodiscard]] bool post_worker_controls(detail::app_runtime_graph& runtime, control_list& controls) noexcept {
    bool posted = true;
    for (std::size_t index = 0; index < controls.size() && posted; ++index) {
        posted = runtime.workers_[index].runtime_->runtime().post_control(std::move(controls[index]));
    }
    controls.clear();
    return posted;
}

// A non-routed run materializes the same route plan and worker bindings as
// startup only when its first routed listener is requested. Every throwing
// step precedes adoption; owner controls replace the empty tables in place.
struct runtime_route_candidate final {
    detail::compiled_route_plan_ptr_type plan_;
    std::vector<detail::controller_store> controllers_;
    std::vector<std::unique_ptr<detail::route_table>> tables_;
    std::vector<std::unique_ptr<detail::rate_limiter>> rate_limiters_;
};

[[nodiscard]] runtime_route_candidate prepare_runtime_routes(
    const detail::app_state& state_value, std::size_t worker_count) {
    runtime_route_candidate result;
    const auto registrars = detail::seal_controller_registrars();
    auto& controllers = result.controllers_;
    auto& tables = result.tables_;
    auto& rate_limiters = result.rate_limiters_;
    controllers.reserve(worker_count);
    controllers.emplace_back(registrars);
    result.plan_ = detail::compile_route_plan(registrars, controllers.front(), state_value.configuration_);
    tables.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        if (index != 0) {
            controllers.emplace_back(registrars);
        }
        tables.push_back(std::make_unique<detail::route_table>(*result.plan_, controllers.back()));
    }
    if (tables.front()->has_route_rate_limit() && !state_value.options_.default_rate_limit_per_worker_.has_value()) {
        rate_limiters.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            rate_limiters.push_back(std::make_unique<detail::rate_limiter>(std::nullopt,
                detail::route_rate_limit_presence::present, state_value.options_.rate_limit_capacity_per_worker_));
        }
    }
    return result;
}

task<void> enable_runtime_routes(worker_ref caller, detail::app_runtime_graph& runtime,
    runtime_route_candidate& candidate) {
    detail::runtime_control_completion publication(caller);
    control_list controls;
    make_worker_controls(controls, runtime, publication,
        [&runtime, &candidate](detail::web_worker_runtime& worker, std::size_t index) noexcept {
            auto& controllers = candidate.controllers_;
            auto& tables = candidate.tables_;
            auto& rate_limiters = candidate.rate_limiters_;
            if (!rate_limiters.empty()) {
                worker.adopt_route_rate_limiter(*rate_limiters[index]);
            }
            std::swap(runtime.workers_[index].controllers_, controllers[index]);
            runtime.workers_[index].routes_->swap(*tables[index]);
        });
    // Retain both plans even if stopping makes some workers refuse adoption.
    // Once published, later additions reuse this run's compiled route ability,
    // including when binding the first routed listener subsequently fails.
    runtime.empty_route_plan_ = std::move(runtime.route_plan_);
    runtime.route_plan_ = std::move(candidate.plan_);
    runtime.routes_enabled_ = true;
    const bool posted = post_worker_controls(runtime, controls);
    co_await publication.wait();
    if (auto failure = publication.failure()) {
        std::rethrow_exception(failure);
    }
    if (!posted || publication.completed() != runtime.workers_.size()) {
        throw std::logic_error("the app began stopping before every worker adopted the routes");
    }
}

#if defined(_WIN32)
// Builds an acceptor control before any owner thread prepares new resources.
template <typename run_type>
[[nodiscard]] move_only_function<void()> make_acceptor_control(
    detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
    return publication.make_command([&acceptor, run = std::move(run)](const auto& receipt) mutable {
        std::exception_ptr failure;
        try {
            run(acceptor);
        } catch (...) {
            failure = std::current_exception();
        }
        receipt.complete(std::move(failure));
    });
}

// Runs `run(acceptor)` on the acceptor thread for `publication`; false when
// the acceptor stops first.
template <typename run_type>
[[nodiscard]] bool post_acceptor_control(
    detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
    return acceptor.post_control(make_acceptor_control(acceptor, publication, std::move(run)));
}
#endif

// The owner threads one control round of the run reaches: every worker, and
// on Windows the acceptor.
[[nodiscard]] std::size_t control_targets(const detail::app_runtime_graph& runtime) noexcept {
#if defined(_WIN32)
    return runtime.workers_.size() + 1;
#else
    return runtime.workers_.size();
#endif
}

}  // namespace

// add_listener() rejects an endpoint a running listener serves, as listen()
// rejects one its call names twice.
void detail::ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing,
    std::span<const http_server_listener_definition> added) {
    for (const auto& candidate : added) {
        for (const auto& listener : existing) {
            if (listener.endpoint_ == candidate.endpoint_) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
            if (candidate.http3_.has_value() && listener.http3_.has_value() &&
                listener.http3_->port_ == candidate.http3_->port_ &&
                listener.endpoint_.address() == candidate.endpoint_.address()) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    }
}

std::vector<detail::http_server_listener_definition> detail::load_server_listeners(
    std::span<const listen_config> configs) {
    const tls_secret_span_cleanser secret_cleanser(configs);
    std::vector<http_server_listener_definition> listeners;
    try {
        if (configs.empty()) {
            throw std::invalid_argument("listen config list must not be empty");
        }
        append_listen_configs(listeners, configs);
    } catch (...) {
        close_unowned_inherited_sockets(configs, listeners);
        throw;
    }
    return listeners;
}

detail::owned_listen_configs::owned_listen_configs(listen_config config) {
    try {
        configs_.reserve(1);
        configs_.push_back(std::move(config));
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs_.empty() ? std::span<const listen_config>(&config, 1) : std::span<const listen_config>(configs_), {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        const tls_secret_cleanser input_secrets(config.tls_);
        throw;
    }
}

detail::owned_listen_configs::owned_listen_configs(std::span<const listen_config> configs) {
    try {
        configs_.reserve(configs.size());
        for (const auto& config : configs) {
            configs_.push_back(config);
        }
        own_addresses();
        validate_owned_inherited_sockets(configs_);
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        const tls_secret_span_cleanser owned_secrets(configs_);
        throw;
    }
}

void detail::owned_listen_configs::own_addresses() {
    addresses_.reserve(configs_.size());
    for (auto& config : configs_) {
        addresses_.emplace_back(config.address_.view());
        config.address_ = addresses_.back();
    }
}

detail::owned_listen_configs::~owned_listen_configs() {
    const tls_secret_span_cleanser secrets(configs_);
    if (inherited_owned_) {
        close_unowned_inherited_sockets(configs_, {});
    }
}

std::vector<detail::http_server_listener_definition> detail::owned_listen_configs::load() {
    inherited_owned_ = false;
    return load_server_listeners(configs_);
}

bool detail::owned_listen_configs::needs_tls_preparation() const noexcept {
    return std::ranges::any_of(configs_, [](const auto& config) { return config.https_.has_value(); });
}

application& application::listen(listen_config config) & {
    return listen(std::span<const listen_config>(&config, 1));
}

application& application::listen(std::span<const listen_config> configs) & {
    const tls_secret_span_cleanser secret_cleanser(configs);
    try {
        std::lock_guard lock(state_->mutex_);
        detail::ensure_app_not_running(state_->lifecycle_.active(), "cannot change listeners while app is running");
    } catch (...) {
        close_unowned_inherited_sockets(configs, {});
        throw;
    }
    auto replacement = detail::load_server_listeners(configs);
    // The replaced definitions retire with `replacement`, after the lock,
    // closing inherited sockets no run consumed.
    detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running",
        [&replacement](detail::app_state& state_value) { state_value.listeners_.swap(replacement); });
    return *this;
}

application& application::listen(std::initializer_list<listen_config> configs) & {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

namespace {
task<void> replace_app_tls_identities(std::shared_ptr<detail::app_state> state, worker_ref caller,
    asio::ip::tcp::endpoint target, owned_tls_config input) {
    std::shared_ptr<const detail::http_server_tls_identities> retired;
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "TLS identities can be replaced only while the app is running");
    {
        std::lock_guard lock(state->mutex_);
        (void)find_replaceable_tls_listener(*state, target, input.config_);
    }
    auto identities = co_await prepare_runtime_control(caller, [input = std::move(input)]() mutable {
        return detail::load_http_server_tls_identities(input.config_);
    });
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto& runtime = active.runtime();
    std::size_t index = 0;
    bool posted = false;
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "TLS identities can be replaced only while the app is running");
        index = stored_listener_index(*state, target);
        const auto slot = runtime_listener_slot(runtime, target);
        make_worker_controls(controls, runtime, completion,
            [slot, identities](detail::web_worker_runtime& worker, std::size_t) {
                worker.adopt_tls_identities(slot, identities);
            });
        posted = post_worker_controls(runtime, controls);
    }
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    {
        std::lock_guard lock(state->mutex_);
        if (!posted || completion.completed() != runtime.workers_.size() || !state->lifecycle_.running()) {
            throw std::logic_error("the app began stopping before every worker adopted the TLS identities");
        }
        auto& stored = std::get<detail::http_server_listener_definition::tls>(state->listeners_[index].transport_).identities_;
        retired = std::exchange(stored, std::move(identities));
    }
}
}  // namespace

task<void> application::replace_tls_identities(
    worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    owned_tls_config input(std::move(config));
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return replace_app_tls_identities(state_, std::move(caller), target, std::move(input));
}

namespace {
task<bool> close_app_connection(
    std::shared_ptr<detail::app_state> state, worker_ref caller, std::uint64_t connection_id) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "connections can be closed only while the app is running");
    auto& runtime = active.runtime();
    std::atomic<bool> closed{false};
    detail::runtime_control_completion completion(caller);
    control_list controls;
    make_worker_controls(controls, runtime, completion,
        [connection_id, &closed](detail::web_worker_runtime& worker, std::size_t) noexcept {
            if (worker.close_connection(connection_id)) {
                closed.store(true, std::memory_order_relaxed);
            }
        });
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    if (auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
    if (!posted || completion.completed() != runtime.workers_.size()) {
        throw std::logic_error("the app began stopping before every worker accepted the connection control");
    }
    co_return closed.load(std::memory_order_relaxed);
}
}  // namespace

task<bool> application::close_connection(worker_ref caller, std::uint64_t connection_id) & {
    return close_app_connection(state_, std::move(caller), connection_id);
}

namespace {
task<void> wait_app_connections(std::shared_ptr<detail::app_state> state, worker_ref caller,
    asio::ip::tcp::endpoint target, operation_options options) {
    detail::operation_steps steps(options);
    active_app_control active(state, "connections can be awaited only while the app is running");
    auto& runtime = active.runtime();
    std::vector<std::shared_ptr<detail::connection_count>> counts;
    std::vector<stop_token> stops;
    {
        const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "connections can be awaited only while the app is running");
        counts.reserve(runtime.workers_.size());
        stops.reserve(runtime.workers_.size());
        for (const auto& worker : runtime.workers_) {
            counts.push_back(worker.runtime_->listener_connections(target));
            stops.push_back(worker.runtime_->cancellation());
        }
    }
    stop_group stopping(stops);
    for (const auto& count : counts) {
        co_await detail::connection_count::wait(count, caller, steps.next("wait_connections_closed"), stopping.token());
    }
}
}  // namespace

task<void> application::wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options) & {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return wait_app_connections(state_, std::move(caller), target, std::move(options));
}

namespace {
struct listener_addition_candidate final {
    std::vector<detail::http_server_listener_definition> added_;
    std::vector<detail::http_server_listener_definition> prepared_;
    std::vector<std::vector<detail::prepared_http_server_session>> sessions_;
    std::optional<runtime_route_candidate> routes_;
};

[[nodiscard]] std::exception_ptr control_round_failure(const detail::runtime_control_completion& completion,
    bool posted, std::size_t targets, const char* message) noexcept {
    if (auto failure = completion.failure()) {
        return failure;
    }
    if (!posted || completion.completed() != targets) {
        try {
            return std::make_exception_ptr(std::logic_error(message));
        } catch (...) {
            return std::current_exception();
        }
    }
    return {};
}

task<void> add_app_listener(
    std::shared_ptr<detail::app_state> state, worker_ref caller, detail::owned_listen_configs input,
    std::shared_ptr<detail::prepared_listener_state> transaction) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be added only while the app is running");
    auto& runtime = active.runtime();
    const auto worker_count = runtime.workers_.size();
    const auto key_origin = runtime.tls_key_origin_;
    const bool needs_routes = !runtime.routes_enabled_;
    const bool inexpensive = std::ranges::none_of(input.configs_, [needs_routes](const auto& config) {
        return config.https_.has_value() || (needs_routes && config.dispatch_ == listener_dispatch::routes);
    });
    auto prepare =
        [state, worker_count, key_origin, needs_routes, input = std::move(input)]() mutable {
            listener_addition_candidate result;
            result.added_ = input.load();
            result.prepared_.reserve(result.added_.size());
            for (const auto& definition : result.added_) {
                result.prepared_.push_back(detail::validate_run_listener(
                    definition, state->options_, key_origin));
            }
            result.sessions_.reserve(worker_count);
            for (std::size_t worker = 0; worker < worker_count; ++worker) {
                auto& sessions = result.sessions_.emplace_back();
                sessions.reserve(result.prepared_.size());
                for (const auto& definition : result.prepared_) {
                    if (std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_)) {
                        sessions.push_back(detail::prepare_http_server_session_config(
                            definition, static_cast<bool>(state->options_.tls_client_hello_)));
                    } else {
                        sessions.emplace_back();
                    }
                }
            }
            if (needs_routes && std::ranges::any_of(result.added_, [](const auto& listener) {
                    return !listener.connection_.has_value() && !listener.request_stream_ &&
                           !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(
                               listener.transport_);
                })) {
                result.routes_.emplace(prepare_runtime_routes(*state, worker_count));
            }
            return result;
        };
    // Pure plaintext additions need no file/PEM/route work and still work on
    // callers whose blocking pool is disabled.
    auto candidate = inexpensive ? prepare() : (co_await prepare_runtime_control(caller, std::move(prepare)));
    auto& added = candidate.added_;
    auto& prepared = candidate.prepared_;
    std::vector<std::optional<std::size_t>> http3_slots;
    std::size_t first_slot = 0;
#if defined(RUVIA_ENABLE_HTTP3)
    detail::http3_worker_directory* directory = nullptr;
#endif
    {
        std::lock_guard lock(state->mutex_);
        (void)running_runtime(*state, "listeners can be added only while the app is running");
        detail::ensure_new_listener_endpoints(state->listeners_, added);
        state->listeners_.reserve(state->listeners_.size() + added.size());
        http3_slots.reserve(added.size());
#if defined(RUVIA_ENABLE_HTTP3)
        if (runtime.http3_directory_ == nullptr &&
            std::ranges::any_of(prepared, [](const auto& definition) { return definition.http3_.has_value(); })) {
            auto created = std::make_unique<detail::http3_worker_directory>(runtime.workers_.size());
            for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
                created->bind_worker(index, runtime.workers_[index].runtime_->runtime().worker());
            }
            runtime.http3_directory_ = std::move(created);
        }
        directory = runtime.http3_directory_.get();
#endif
        first_slot = runtime.listener_slots_.size();
        runtime.listener_slots_.resize(first_slot + added.size());
        for (const auto& definition : prepared) {
            http3_slots.push_back(
                definition.http3_.has_value() ? std::optional<std::size_t>(runtime.http3_slot_count_++) : std::nullopt);
        }
    }
    if (candidate.routes_) {
        co_await enable_runtime_routes(caller, runtime, *candidate.routes_);
    }
    const auto discard = [first_slot, count = prepared.size()](auto& target, const std::shared_ptr<void>& receipt) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.discard_listener(first_slot + index, receipt);
        }
    };
    const auto open = [first_slot, count = prepared.size()](auto& target) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            target.open_listener(first_slot + index);
        }
    };
    detail::runtime_control_completion prepared_completion(caller);
    detail::runtime_control_completion discarded_completion(caller);
    detail::runtime_control_completion opened_completion(caller);
    control_list prepare_controls;
    control_list discard_controls;
    control_list open_controls;
    make_worker_controls(prepare_controls, runtime, prepared_completion,
        [&](detail::web_worker_runtime& worker, std::size_t worker_index) {
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index],
                    std::move(candidate.sessions_[worker_index][index])
#if defined(RUVIA_ENABLE_HTTP3)
                        ,
                    directory
#endif
                );
            }
        });
    make_worker_controls(discard_controls, runtime, discarded_completion,
        [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::web_worker_runtime& worker, std::size_t) { discard(worker, receipt); });
    make_worker_controls(open_controls, runtime, opened_completion,
        [open](detail::web_worker_runtime& worker, std::size_t) { open(worker); });
#if defined(_WIN32)
    auto prepare_acceptor = make_acceptor_control(*runtime.acceptor_, prepared_completion,
        [&](detail::windows_acceptor& acceptor) {
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index]
#if defined(RUVIA_ENABLE_HTTP3)
                    ,
                    directory
#endif
                );
            }
        });
    auto discard_acceptor = make_acceptor_control(*runtime.acceptor_, discarded_completion,
        [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::windows_acceptor& acceptor) { discard(acceptor, receipt); });
    detail::runtime_control_completion accepting_completion(caller);
    auto open_acceptor = make_acceptor_control(*runtime.acceptor_, accepting_completion,
        [open](detail::windows_acceptor& acceptor) { open(acceptor); });
#endif
    bool posted = post_worker_controls(runtime, prepare_controls);
#if defined(_WIN32)
    if (posted) {
        posted = runtime.acceptor_->post_control(std::move(prepare_acceptor));
    }
    prepare_acceptor = nullptr;
#endif
    co_await prepared_completion.wait();
    auto failure = control_round_failure(prepared_completion, posted, control_targets(runtime),
        "the app began stopping before every worker prepared the listeners");
    if (!failure) {
        try {
            co_await transaction->checkpoint(runtime.workers_.front().runtime_->cancellation());
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (!failure) {
        {
            std::lock_guard lock(state->mutex_);
            posted = state->lifecycle_.running() && post_worker_controls(runtime, open_controls);
        }
        open_controls.clear();
        co_await opened_completion.wait();
        failure = control_round_failure(opened_completion, posted, runtime.workers_.size(),
            "the app began stopping before every worker accepted the listeners");
#if defined(_WIN32)
        if (!failure) {
            {
                std::lock_guard lock(state->mutex_);
                posted = state->lifecycle_.running() && runtime.acceptor_->post_control(std::move(open_acceptor));
            }
            open_acceptor = nullptr;
            co_await accepting_completion.wait();
            failure = control_round_failure(accepting_completion, posted, 1,
                "the app began stopping before the acceptor accepted the listeners");
        }
#endif
    }
    if (!failure) {
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            try {
                failure = std::make_exception_ptr(
                    std::logic_error("the app began stopping before every worker accepted the listeners"));
            } catch (...) {
                failure = std::current_exception();
            }
        } else {
            for (std::size_t index = 0; index < added.size(); ++index) {
                runtime.listener_slots_[first_slot + index] = added[index].endpoint_;
                added[index].inherited_sockets_.reset();
                state->listeners_.push_back(std::move(added[index]));
            }
        }
    }
    if (failure) {
        (void)post_worker_controls(runtime, discard_controls);
#if defined(_WIN32)
        (void)runtime.acceptor_->post_control(std::move(discard_acceptor));
#endif
        co_await discarded_completion.wait();
        if (auto rollback_failure = discarded_completion.failure()) {
            report_unhandled_failure("app listener rollback", rollback_failure);
        }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        for (const auto& slot : http3_slots) {
            if (slot) {
                directory->close_unclaimed_listener_sockets(*slot);
            }
        }
#endif
        std::rethrow_exception(failure);
    }
}
}  // namespace

task<prepared_listener> application::prepare_listener(worker_ref caller, listen_config config) & {
    detail::owned_listen_configs input(std::move(config));
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction,
        add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<prepared_listener> application::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    detail::owned_listen_configs input(configs);
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction,
        add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<void> application::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}

namespace {
task<void> remove_app_listener(
    std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, listener_remove_options options) {
    std::optional<detail::http_server_listener_definition> retired;
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be removed only while the app is running");
    auto& runtime = active.runtime();
    std::size_t slot = 0;
    {
        std::lock_guard lock(state->mutex_);
        (void)stored_listener_index(*state, target);
        slot = runtime_listener_slot(runtime, target);
    }
#if defined(_WIN32)
    detail::runtime_control_completion accepting_completion(caller);
    auto accepting_lifetime = std::make_shared<detail::runtime_control_completion::receipt>(
        accepting_completion.make_receipt());
    const bool accepting_posted = post_acceptor_control(*runtime.acceptor_, accepting_completion,
        [slot, receipt = std::move(accepting_lifetime)](detail::windows_acceptor& acceptor) {
            acceptor.remove_listener(slot, receipt);
        });
    co_await accepting_completion.wait();
    if (auto failure = control_round_failure(accepting_completion, accepting_posted, 1,
            "the app began stopping before the acceptor removed the listener")) {
        std::rethrow_exception(failure);
    }
#endif
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto lifetime = std::make_shared<detail::runtime_control_completion::receipt>(completion.make_receipt());
    make_worker_controls(controls, runtime, completion,
        [slot, receipt = std::move(lifetime), options](detail::web_worker_runtime& worker, std::size_t) {
            worker.remove_listener(slot, receipt, options);
        });
    bool posted = false;
    {
        std::lock_guard lock(state->mutex_);
        posted = state->lifecycle_.running() && post_worker_controls(runtime, controls);
    }
    controls.clear();
    co_await completion.wait();
    if (auto failure = control_round_failure(completion, posted, runtime.workers_.size(),
            "the app began stopping before every worker removed the listener")) {
        std::rethrow_exception(failure);
    }
    {
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            throw std::logic_error("the app began stopping before every worker removed the listener");
        }
        const auto index = stored_listener_index(*state, target);
        retired.emplace(std::move(state->listeners_[index]));
        state->listeners_.erase(state->listeners_.begin() + static_cast<std::ptrdiff_t>(index));
        runtime.listener_slots_[slot].reset();
    }
}
}  // namespace

task<void> application::remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options) & {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return remove_app_listener(state_, std::move(caller), target, options);
}

#if !defined(_WIN32)
namespace {
task<listener_sockets> duplicate_app_listener_sockets(
    std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listener sockets can be duplicated only while the app is running");
    auto& runtime = active.runtime();
    std::vector<detail::listener_socket_duplicates> duplicates;
    detail::runtime_control_completion completion(caller);
    control_list controls;
    bool https = false;
    bool http3 = false;
    {
        std::lock_guard lock(state->mutex_);
        const auto& stored = state->listeners_[stored_listener_index(*state, target)];
        https = std::holds_alternative<detail::http_server_listener_definition::tls>(stored.transport_);
        http3 = stored.http3_.has_value();
        const auto slot = runtime_listener_slot(runtime, target);
        duplicates.resize(runtime.workers_.size());
        make_worker_controls(controls, runtime, completion,
            [slot, &duplicates](detail::web_worker_runtime& worker, std::size_t index) {
                duplicates[index] = worker.duplicate_listener(slot);
            });
    }
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    listener_sockets result;
    auto& tcp = https ? result.https_ : result.http_;
    try {
        if (auto failure = control_round_failure(completion, posted, duplicates.size(),
                "the app began stopping before every worker duplicated its sockets")) {
            std::rethrow_exception(failure);
        }
        tcp.reserve(duplicates.size());
        if (http3) {
            result.http3_.reserve(duplicates.size());
        }
    } catch (...) {
        for (const auto& duplicate : duplicates) {
            detail::close_native_socket(duplicate.tcp_);
            detail::close_native_socket(duplicate.udp_);
        }
        throw;
    }
    for (const auto& duplicate : duplicates) {
        tcp.push_back(duplicate.tcp_);
        if (http3) {
            result.http3_.push_back(duplicate.udp_);
        }
    }
    co_return result;
}
}  // namespace

task<listener_sockets> application::duplicate_listener_sockets(
    worker_ref caller, const listener_endpoint& endpoint) const& {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return duplicate_app_listener_sockets(state_, std::move(caller), target);
}
#endif

application& application::server(server_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change server config while app is running",
        [&config](detail::app_state& state_value) { state_value.options_.set_server(config); });
}

application& application::on_start(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
            if (!hook) {
                throw std::invalid_argument("on_start hook must not be null");
            }
            state_value.on_start_hooks_.push_back(std::move(hook));
        });
}

application& application::on_start(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_start hook while app is running",
        [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
            // The cleared targets retire with this lambda, after the app lock.
            retired.swap(state_value.on_start_hooks_);
        });
}

application& application::on_stop(app_hook hook) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [hook = detail::own_callback(std::move(hook))](detail::app_state& state_value) mutable {
            if (!hook) {
                throw std::invalid_argument("on_stop hook must not be null");
            }
            state_value.on_stop_hooks_.push_back(std::move(hook));
        });
}

application& application::on_stop(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register on_stop hook while app is running",
        [retired = std::vector<detail::callback_registration<app_hook>>{}](detail::app_state& state_value) mutable {
            retired.swap(state_value.on_stop_hooks_);
        });
}

application& application::rate_limit(rate_limit_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [config](detail::app_state& state_value) { state_value.options_.set_rate_limit(config); });
}

application& application::rate_limit(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the default rate limit per worker while app is running",
        [](detail::app_state& state_value) { state_value.options_.clear_rate_limit(); });
}

application& application::trusted_proxies(trusted_proxy_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change trusted proxies while app is running",
        [config = std::move(config)](detail::app_state& state_value) {
            state_value.options_.set_trusted_proxies(config);
        });
}

application& application::on_access(access_log_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register access-log hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            // Captured old target retires with this lambda, after the app lock.
            std::swap(state_value.access_log_callback_, callback_value);
            state_value.options_.access_log_.callback_ = detail::callback_access::ref(state_value.access_log_callback_);
        });
}

application& application::on_connection_failure(connection_failure_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection-failure hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_failure_callback_, callback_value);
            state_value.options_.connection_failure_.callback_ =
                detail::callback_access::ref(state_value.connection_failure_callback_);
        });
}

application& application::on_tls_client_hello(tls_client_hello_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register TLS ClientHello hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.tls_client_hello_callback_, callback_value);
            state_value.options_.tls_client_hello_ =
                detail::callback_access::ref(state_value.tls_client_hello_callback_);
        });
}

application& application::on_tls_client_hello(std::nullptr_t) & {
    return on_tls_client_hello(tls_client_hello_callback{});
}

application& application::on_http2_input(http2_input_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 input hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_input_callback_, callback_value);
            state_value.options_.http2_input_ = detail::callback_access::ref(state_value.http2_input_callback_);
        });
}

application& application::on_http2_input(std::nullptr_t) & {
    return on_http2_input(http2_input_callback{});
}

#if defined(RUVIA_ENABLE_HTTP2)
application& application::on_http2_frame(http2_frame_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 frame hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_frame_callback_, callback_value);
            state_value.options_.http2_frame_ = detail::callback_access::ref(state_value.http2_frame_callback_);
        });
}

application& application::on_http2_frame(std::nullptr_t) & {
    return on_http2_frame(http2_frame_callback{});
}

application& application::on_http2_headers(http2_headers_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/2 headers hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http2_headers_callback_, callback_value);
            state_value.options_.http2_headers_ = detail::callback_access::ref(state_value.http2_headers_callback_);
        });
}

application& application::on_http2_headers(std::nullptr_t) & {
    return on_http2_headers(http2_headers_callback{});
}
#endif
#if defined(RUVIA_ENABLE_HTTP3)
application& application::on_http3_frame(http3_frame_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/3 frame hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http3_frame_callback_, callback_value);
            state_value.options_.http3_frame_ = detail::callback_access::ref(state_value.http3_frame_callback_);
        });
}

application& application::on_http3_frame(std::nullptr_t) & {
    return on_http3_frame(http3_frame_callback{});
}

application& application::on_http3_field(http3_field_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register HTTP/3 field hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.http3_field_callback_, callback_value);
            state_value.options_.http3_field_ = detail::callback_access::ref(state_value.http3_field_callback_);
        });
}

application& application::on_http3_field(std::nullptr_t) & {
    return on_http3_field(http3_field_callback{});
}
#endif

application& application::on_connection(connection_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection handler while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_callback_, callback_value);
            state_value.options_.connection_ = detail::callback_access::ref(state_value.connection_callback_);
        });
}

application& application::on_connection(std::nullptr_t) & {
    return on_connection(connection_callback{});
}

application& application::on_connection_admission(connection_admission_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection admission hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_admission_callback_, callback_value);
            state_value.options_.connection_admission_ = detail::callback_access::ref(state_value.connection_admission_callback_);
        });
}

application& application::on_connection_admission(std::nullptr_t) & {
    return on_connection_admission(connection_admission_callback{});
}

application& application::on_connection_open(connection_open_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection open hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_open_callback_, callback_value);
            state_value.options_.connection_open_ = detail::callback_access::ref(state_value.connection_open_callback_);
        });
}

application& application::on_connection_open(std::nullptr_t) & {
    return on_connection_open(connection_open_callback{});
}

application& application::on_connection_close(connection_close_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register connection close hook while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.connection_close_callback_, callback_value);
            state_value.options_.connection_close_ = detail::callback_access::ref(state_value.connection_close_callback_);
        });
}

application& application::on_connection_close(std::nullptr_t) & {
    return on_connection_close(connection_close_callback{});
}

application& application::on_request_stream(request_stream_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register request stream handler while app is running",
        [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
            std::swap(state_value.request_stream_callback_, callback_value);
            state_value.options_.request_stream_ = detail::callback_access::ref(state_value.request_stream_callback_);
        });
}

application& application::on_request_stream(std::nullptr_t) & {
    return on_request_stream(request_stream_callback{});
}

#ifdef RUVIA_ENABLE_DATABASE
application& application::database(db_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running", [&](detail::app_state& state_value) {
            detail::validate_capability_alias(config.alias_.view(), "database alias must not be empty");
            detail::db_definition replacement{std::string(config.alias_.view()),
                detail::db_config_storage(config.config_)};
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

application& application::database(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure database while app is running",
        [](detail::app_state& state_value) { state_value.databases_.clear(); });
}
#endif

#ifdef RUVIA_ENABLE_REDIS
application& application::redis(redis_registration_config config) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(
                state_value.redis_, config.alias_.view(), config.config_, "redis alias must not be empty");
        });
}

application& application::redis(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot configure redis while app is running",
        [](detail::app_state& state_value) { state_value.redis_.clear(); });
}
#endif

application& application::client(http_client_config config) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running", [&](detail::app_state& state_value) {
            detail::upsert_named_capability_definition(state_value.http_clients_, config.alias_.view(), config,
                "HTTP client alias must not be empty");
        });
}

application& application::client(std::nullptr_t) & {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot configure HTTP client while app is running",
        [](detail::app_state& state_value) { state_value.http_clients_.clear(); });
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

http_server_options standalone_options(server_config config, const http_server_callbacks& callbacks, std::size_t count) {
    config.worker_count_ = count;
    config.process_signal_handlers_ = process_signal_handler_policy::external_owner;
    http_server_options options;
    options.set_server(config);
    options.connection_ = callback_access::ref(callbacks.connection_);
    options.request_stream_ = callback_access::ref(callbacks.request_stream_);
    options.access_log_.callback_ = callback_access::ref(callbacks.access_);
    options.connection_failure_.callback_ = callback_access::ref(callbacks.connection_failure_);
    options.tls_client_hello_ = callback_access::ref(callbacks.tls_client_hello_);
    options.http2_input_ = callback_access::ref(callbacks.http2_input_);
#if defined(RUVIA_ENABLE_HTTP2)
    options.http2_frame_ = callback_access::ref(callbacks.http2_frame_);
    options.http2_headers_ = callback_access::ref(callbacks.http2_headers_);
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    options.http3_frame_ = callback_access::ref(callbacks.http3_frame_);
    options.http3_field_ = callback_access::ref(callbacks.http3_field_);
#endif
    options.connection_admission_ = callback_access::ref(callbacks.connection_admission_);
    options.connection_open_ = callback_access::ref(callbacks.connection_open_);
    options.connection_close_ = callback_access::ref(callbacks.connection_close_);
    return options;
}

asio::ip::tcp::endpoint service_endpoint(const listener_endpoint& endpoint) {
    const auto address = parse_ip_address(endpoint.address_.view());
    if (!address || endpoint.port_.value() == 0) {
        throw std::invalid_argument("connection service requires a numeric address and nonzero port");
    }
    return {*address, endpoint.port_.value()};
}

template <typename input_type>
std::shared_ptr<owned_listen_configs> own_listen(input_type input) {
    owned_listen_configs guard(std::move(input));
    // Allocation precedes transfer so allocation failure still closes handles.
    return std::make_shared<owned_listen_configs>(std::move(guard));
}

struct owned_endpoint final {
    std::string address_;
    port_number port_;
    explicit owned_endpoint(const listener_endpoint& endpoint)
        : address_(endpoint.address_.view()),
          port_(endpoint.port_) {}
    listener_endpoint view() const {
        return {.address_ = address_, .port_ = port_};
    }
};

struct socket_owner final {
    native_socket socket_;
    explicit socket_owner(native_socket socket) noexcept
        : socket_(socket) {}
    socket_owner(socket_owner&& other) noexcept
        : socket_(std::exchange(other.socket_, invalid_native_socket)) {}
    socket_owner(const socket_owner&) = delete;
    ~socket_owner() {
        close_native_socket(socket_);
    }
};

struct owned_adopt_options final {
    std::vector<std::byte> prefix_{};
    std::optional<ip_endpoint> remote_;
    explicit owned_adopt_options(adopt_options options)
        : prefix_(options.prefix_.begin(), options.prefix_.end()),
          remote_(options.remote_) {}
    adopt_options view() const noexcept {
        return {.prefix_ = prefix_, .remote_ = remote_};
    }
};

using definitions = std::vector<http_server_listener_definition>;
using sessions = std::vector<prepared_http_server_session>;
struct listener_candidate final {
    definitions definitions_{};
    std::vector<sessions> sessions_;
};

listener_candidate load_candidate(const std::vector<std::shared_ptr<owned_listen_configs>>& inputs,
    bool external, server_config config, const std::shared_ptr<http_server_callbacks>& callbacks,
    std::size_t count, std::chrono::steady_clock::time_point origin) {
    listener_candidate result;
    const auto options = standalone_options(config, *callbacks, count);
    for (const auto& input : inputs) {
        for (auto& config : input->configs_) {
            if (!config.dispatch_) {
                if (static_cast<bool>(callbacks->connection_) == static_cast<bool>(callbacks->request_stream_)) {
                    throw std::invalid_argument("standalone listener requires an unambiguous callback or explicit dispatch");
                }
                config.dispatch_ = callbacks->connection_ ? listener_dispatch::connection : listener_dispatch::request_stream;
            }
        }
        auto loaded = input->load();
        for (const auto& definition : loaded) {
            if (!definition.connection_ && !definition.request_stream_ &&
                !std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_)) {
                throw std::invalid_argument("HTTP server requires request_stream or connection dispatch");
            }
            if (external && (definition.http3_ || definition.inherited_sockets_ ||
                                std::holds_alternative<http_server_listener_definition::redirect_http_to_https>(definition.transport_))) {
                throw std::invalid_argument("an externally accepted service cannot own a listener or redirect");
            }
            ensure_new_listener_endpoints(result.definitions_, std::span<const http_server_listener_definition>(&definition, 1));
            result.definitions_.push_back(validate_run_listener(definition, options, origin));
        }
    }
    return result;
}

void prepare_sessions(listener_candidate& candidate, std::size_t count, bool observe) {
    candidate.sessions_.resize(count);
    for (auto& worker : candidate.sessions_) {
        worker.reserve(candidate.definitions_.size());
        for (const auto& definition : candidate.definitions_) {
            worker.push_back(std::holds_alternative<http_server_listener_definition::tls>(definition.transport_)
                                 ? prepare_http_server_session_config(definition, observe)
                                 : prepared_http_server_session{});
        }
    }
}

bool needs_tls_preparation(const std::vector<std::shared_ptr<owned_listen_configs>>& inputs, const definitions& existing = {}) {
    for (const auto& input : inputs) {
        if (input->needs_tls_preparation()) {
            return true;
        }
    }
    for (const auto& definition : existing) {
        if (std::holds_alternative<http_server_listener_definition::tls>(definition.transport_)) {
            return true;
        }
    }
    return false;
}

template <typename function_type>
task<std::invoke_result_t<function_type&>> prepare_work(worker_ref caller, bool blocking, function_type function) {
    using result_type = std::invoke_result_t<function_type&>;
    if (!caller.is_current() || !caller.accepting()) {
        throw std::logic_error("HTTP server caller is not running");
    }
    if (blocking) {
        auto result = co_await run_blocking(caller, std::move(function));
        if (!caller.accepting()) {
            throw std::logic_error("HTTP server caller is stopping");
        }
        co_return std::move(result).take_value();
    }
    if constexpr (std::is_void_v<result_type>) {
        function();
        co_return;
    } else {
        co_return function();
    }
}

}  // namespace

void close_server_group(const std::weak_ptr<http_server_group_state>& group) noexcept;
void fail_server_group(const std::weak_ptr<http_server_group_state>& group, const std::exception_ptr& failure) noexcept;

class http_server_state final : public std::enable_shared_from_this<http_server_state> {
    struct service final {
        http_server_listener_definition definition_;
        bool external_;
        bool active_{true};
        std::optional<std::size_t> http3_slot_;
        bool published_{false};
    };
    struct activity final {
        http_server_state& self_;
        explicit activity(http_server_state& self)
            : self_(self) {
            ++self_.controls_;
        }
        ~activity() {
            --self_.controls_;
            self_.changed_.notify();
        }
    };

public:
    http_server_state(event_loop loop, std::shared_ptr<http_server_callbacks> callbacks, server_config config,
        std::size_t index = 0, std::size_t count = 1
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        std::shared_ptr<http3_worker_directory> directory = {}
#endif
        )
        : loop_(require_loop(std::move(loop))),
          worker_(loop_.worker()),
          callbacks_(std::move(callbacks)),
          options_(standalone_options(config, *callbacks_, count)),
          inbound_budget_(options_.server_.max_inbound_buffer_bytes_per_worker_),
          controllers_(std::span<const controller_registrar>{}),
          routes_(plan_, controllers_),
          tasks_(worker_),
          capabilities_(loop_.io_context(), worker_, {},
              {.max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                  .http_client_worker_ = options_.server_.http_client_worker_,
                  .trusted_proxies_ = &options_.trusted_proxies_,
                  .precompressed_static_files_ = options_.compression_.has_value(),
                  .file_io_timeout_ = options_.server_.write_timeout_,
                  .connection_failure_ = &options_.connection_failure_}),
          connections_(loop_.io_context(), worker_, routes_, capabilities_, options_, token_, phase_, tasks_, {},
              {.target_ = this, .invoke_ = &fail}, index
#if defined(RUVIA_ENABLE_HTTP3)
              ,
              {.directory_ = directory.get(), .worker_index_ = index}
#endif
              ),
          services_(),
          changed_(worker_)
#if defined(_WIN32)
          ,
          worker_index_(index)
#endif
    {
        options_.inbound_budget_ = &inbound_budget_;
        options_.codecs_ = &codecs_;
        closed_waiters_.reserve(2);
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::move(directory);
#endif
    }
    ~http_server_state() {
        if (failure_ && !failure_observed_) {
            report_unhandled_failure("HTTP server", failure_);
        }
    }
    void bind_stop() {
        std::weak_ptr<http_server_state> weak = shared_from_this();
        stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
            if (auto state = weak.lock()) {
                const bool observe = !state->grouped_;
                co_await shutdown_owned(std::move(state), observe);
            }
        });
    }
    void stage(std::shared_ptr<owned_listen_configs> input) {
        require_current();
        require_open();
        if (started_ || starting_) {
            throw std::logic_error("listen() is only available before HTTP server startup");
        }
        staged_.push_back(std::move(input));
    }
    void trusted_proxies(trusted_proxy_config config) {
        require_current();
        require_open();
        if (started_ || starting_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server startup");
        }
        options_.set_trusted_proxies(config);
    }
    task<void> add(std::shared_ptr<owned_listen_configs> input, bool external) {
        return add_owned(shared_from_this(), std::move(input), external);
    }
    task<prepared_listener> prepare(std::shared_ptr<owned_listen_configs> input) {
        auto transaction = prepared_listener_access::make(worker_);
        return prepared_listener_access::launch(transaction, add_owned(shared_from_this(), std::move(input), false, transaction));
    }
    task<void> start() {
        return start_owned(shared_from_this());
    }
    task<void> replace(owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        return replace_owned(shared_from_this(), std::move(endpoint), std::move(replacement));
    }
    void adopt(const listener_endpoint& endpoint, native_socket socket, adopt_options options) {
        std::size_t index;
        try {
            require_current();
            require_running();
            index = find(endpoint);
        } catch (...) {
            close_native_socket(socket);
            throw;
        }
        connections_.adopt_connection(index, socket, options);
    }
    bool close_connection(std::uint64_t id) {
        require_current();
        require_running();
        return connections_.close_connection(id);
    }
    task<void> wait_connections(owned_endpoint endpoint, operation_options options) {
        return wait_connections_owned(shared_from_this(), std::move(endpoint), std::move(options));
    }
    void remove(const listener_endpoint& endpoint, std::shared_ptr<void> receipt = {}, listener_remove_options options = {}) {
        require_current();
        require_running();
        const auto index = find(endpoint);
#if defined(_WIN32)
        if (!services_[index].external_ && (!grouped_ || worker_index_ == 0)) {
            struct removal_command final {
                std::shared_ptr<void> receipt_;
                std::shared_ptr<http_server_state> owner_;
                std::size_t index_;
                void operator()() {
                    owner_->acceptor_->remove_listener(index_, receipt_);
                }
            };
            if (!acceptor_->post_control(removal_command{receipt, shared_from_this(), index})) {
                throw std::logic_error("HTTP server acceptor is stopping");
            }
        }
#endif
        connections_.remove_listener(index, std::move(receipt), options);
        services_[index].active_ = false;
    }
#ifndef _WIN32
    listener_sockets duplicate(const listener_endpoint& endpoint) {
        require_current();
        require_running();
        const auto index = find(endpoint);
        listener_sockets sockets;
        auto& tcp = std::holds_alternative<http_server_listener_definition::tls>(services_[index].definition_.transport_) ? sockets.https_ : sockets.http_;
        tcp.reserve(1);
        sockets.http3_.reserve(1);
        const auto result = connections_.duplicate_listener(index);
        tcp.push_back(result.tcp_);
        if (result.udp_ != invalid_native_socket) {
            sockets.http3_.push_back(result.udp_);
        }
        return sockets;
    }
#endif
    void request_close() noexcept {
        const auto self = shared_from_this();
        close_server_group(group_);
        stop_.request_stop();
        if (closing_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        group_retirement_ = group_.lock();
        if (worker_.is_current()) {
            begin_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([self] { self->begin_close(); })) {
                if (io_started_.load(std::memory_order_acquire)) {
                    std::terminate();
                }
                finish_closed();
            }
        } catch (...) {
            std::terminate();
        }
    }
    task<void> shutdown() {
        return shutdown_serialized(shared_from_this());
    }
    http_server_stats stats() const {
        auto result = connections_.stats();
        result.worker_failures_ = fatal_count_.load(std::memory_order_relaxed);
        return result;
    }
    const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class http_server_group_state;
    static event_loop require_loop(event_loop loop) {
        if (!loop.valid()) {
            throw std::invalid_argument("HTTP server requires a valid event loop");
        }
        return loop;
    }
    void require_current() const {
        if (!worker_.is_current()) {
            throw std::logic_error("HTTP server operation must run on its bound event loop");
        }
    }
    void require_open() const {
        if (closing_.load(std::memory_order_acquire)) {
            throw std::logic_error("HTTP server is closing");
        }
    }
    void require_running() const {
        require_open();
        if (!started_ || starting_) {
            throw std::logic_error("HTTP server is not started");
        }
    }
    std::size_t find(const listener_endpoint& endpoint) const {
        const auto target = service_endpoint(endpoint);
        for (std::size_t index = 0; index != services_.size(); ++index) {
            if (services_[index].active_ && services_[index].published_ && services_[index].definition_.endpoint_ == target) {
                return index;
            }
        }
        throw std::invalid_argument("no connection service serves this endpoint");
    }
    static void fail(void* target, const std::exception_ptr& error) noexcept {
        auto& self = *static_cast<http_server_state*>(target);
        {
            std::lock_guard lock(self.failure_mutex_);
            if (!self.failure_) {
                self.failure_ = error;
            }
        }
        self.fatal_count_.fetch_add(1, std::memory_order_relaxed);
        fail_server_group(self.group_, error);
        self.request_close();
    }
    void validate_new(const definitions& definitions_value) const {
        require_open();
        for (const auto& existing : services_) {
            if (existing.active_) {
                ensure_new_listener_endpoints(std::span<const http_server_listener_definition>(&existing.definition_, 1), definitions_value);
            }
        }
    }
    void append(const definitions& definitions_value, bool external) {
        validate_new(definitions_value);
        services_.reserve(services_.size() + definitions_value.size());
        // Clone before mutating the live list.
        std::vector<service> added;
        added.reserve(definitions_value.size());
        auto next = http3_slots_;
        for (const auto& definition : definitions_value) {
            const auto slot = definition.http3_ ? std::optional<std::size_t>(next++) : std::nullopt;
            added.push_back({definition.clone(), external, true, slot});
        }
        for (auto& entry : added) {
            services_.push_back(std::move(entry));
        }
        http3_slots_ = next;
    }
#if defined(_WIN32)
    template <typename callable_type>
    static task<void> on_acceptor(std::shared_ptr<http_server_state> self, callable_type callable) {
        runtime_control_completion completion(self->worker_);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = self->acceptor_->post_control(completion.make_command(
                [self, callable = std::move(callable)](const auto& receipt) mutable {
                    try {
                        callable();
                        receipt.complete();
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    }
    task<void> ensure_acceptor() {
        if (grouped_ || acceptor_) {
            co_return;
        }
        bool needed = false;
        for (const auto& entry : services_) {
            needed |= entry.active_ && !entry.external_;
        }
        if (!needed) {
            co_return;
        }
        acceptor_ = std::make_unique<windows_acceptor>(std::span<const http_server_listener_definition>{}, 1,
#if defined(RUVIA_ENABLE_HTTP3)
            directory_.get(),
#endif
            worker_failure_sink{.target_ = this, .invoke_ = &fail});
        connections_.bind_acceptor(*acceptor_);
        acceptor_->bind_worker(0, connections_.acceptor_target());
        co_await acceptor_->start(worker_);
        co_await acceptor_->serve(worker_);
    }
#endif
    static task<void> prepare_range(std::shared_ptr<http_server_state> self, std::size_t first, sessions prepared) {
#if defined(RUVIA_ENABLE_HTTP3)
        for (const auto& entry : self->services_) {
            if (entry.http3_slot_ && !self->directory_) {
                self->directory_ = std::make_unique<http3_worker_directory>(1);
                self->directory_->bind_worker(0, self->worker_);
                break;
            }
        }
#endif
#if defined(_WIN32)
        co_await self->ensure_acceptor();
#endif
        for (std::size_t offset = 0; offset != prepared.size(); ++offset) {
            self->require_open();
            const auto index = first + offset;
            auto& configured = self->services_[index];
            if (!configured.active_) {
                continue;
            }
            self->connections_.prepare_listener(index, configured.http3_slot_, configured.definition_, configured.external_, std::move(prepared[offset])
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                                                                 ,
                self->directory_.get()
#endif
            );
#if defined(_WIN32)
            if (!configured.external_ && (!self->grouped_ || self->worker_index_ == 0)) {
                co_await on_acceptor(self, [self, index] {
                    auto& entry = self->services_[index];
                    self->acceptor_->prepare_listener(index, entry.http3_slot_, entry.definition_
#if defined(RUVIA_ENABLE_HTTP3)
                        ,
                        self->directory_.get()
#endif
                    );
                });
            }
#endif
            configured.definition_.inherited_sockets_.reset();
        }
        co_return;
    }
    static task<void> open_range(std::shared_ptr<http_server_state> self, std::size_t first,
        std::shared_ptr<prepared_listener_state> transaction = {}) {
#if defined(_WIN32)
        runtime_control_completion completion(self->worker_);
        auto command = completion.make_command([self, first](const auto& receipt) noexcept {
            for (auto index = first; index != self->services_.size(); ++index) {
                if (self->services_[index].active_ && !self->services_[index].external_) {
                    self->acceptor_->open_listener(index);
                }
            }
            receipt.complete();
        });
        move_only_function<void()> accepting(std::move(command));
#endif
        if (transaction) {
            co_await transaction->checkpoint(self->token_);
        }
        self->require_open();
        for (auto index = first; index != self->services_.size(); ++index) {
            if (self->services_[index].active_) {
                self->connections_.open_listener(index);
                self->services_[index].published_ = true;
            }
        }
#if defined(_WIN32)
        const bool posted = self->acceptor_ ? self->acceptor_->post_control(std::move(accepting)) : true;
        accepting = nullptr;
        co_await completion.wait();
        (void)completion.failure();
        if (!posted) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
#endif
        self->require_open();
    }
    static task<void> discard_range(std::shared_ptr<http_server_state> self, std::size_t first) {
        runtime_control_completion retired(self->worker_);
        auto receipt = std::make_shared<runtime_control_completion::receipt>(retired.make_receipt());
        for (auto index = first; index != self->services_.size(); ++index) {
            self->connections_.discard_listener(index, receipt);
            self->services_[index].active_ = false;
            self->services_[index].definition_.inherited_sockets_.reset();
#if defined(_WIN32)
            if (self->acceptor_ && !self->services_[index].external_ && (!self->grouped_ || self->worker_index_ == 0)) {
                co_await on_acceptor(self, [self, index, receipt] { self->acceptor_->discard_listener(index, receipt); });
            }
#endif
        }
        receipt.reset();
        co_await retired.wait();
        (void)retired.failure();
    }
    static task<void> prepare_start_owned(std::shared_ptr<http_server_state> self, sessions prepared) {
        self->require_current();
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        self->starting_ = true;
        activity active(*self);
        self->phase_ = http_server_worker_state::running;
        std::exception_ptr failure;
        try {
            self->io_started_.store(true, std::memory_order_release);
            self->connections_.prepare();
            co_await prepare_range(self, 0, std::move(prepared));
            co_await self->capabilities_.start();
            self->require_open();
        } catch (...) {
            failure = std::current_exception();
        }
        self->starting_ = false;
        self->changed_.notify();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> start_owned(std::shared_ptr<http_server_state> self) {
        self->require_current();
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_open();
        if (self->started_ || self->starting_) {
            throw std::logic_error("HTTP server can only start once");
        }
        std::exception_ptr failure;
        {
            activity active(*self);
            self->starting_ = true;
            try {
                auto inputs = std::move(self->staged_);
                auto existing = std::make_shared<listener_candidate>();
                for (const auto& entry : self->services_) {
                    existing->definitions_.push_back(entry.definition_.clone());
                }
                const bool blocking = needs_tls_preparation(inputs, existing->definitions_);
                const auto first = co_await prepare_work(self->worker_, blocking, [inputs = std::move(inputs), existing, config = self->options_.server_, callbacks = self->callbacks_, origin = self->key_origin_]() mutable {
                    auto loaded = load_candidate(inputs, false, config, callbacks, 1, origin);
                    const auto first = existing->definitions_.size();
                    ensure_new_listener_endpoints(existing->definitions_, loaded.definitions_);
                    for (auto& definition : loaded.definitions_) {
                        existing->definitions_.push_back(std::move(definition));
                    }
                    prepare_sessions(*existing, 1, static_cast<bool>(callbacks->tls_client_hello_));
                    return first;
                });
                self->require_open();
                definitions added;
                for (auto index = first; index != existing->definitions_.size(); ++index) {
                    added.push_back(existing->definitions_[index].clone());
                }
                self->append(added, false);
                self->starting_ = false;
                co_await prepare_start_owned(self, std::move(existing->sessions_.front()));
                co_await open_range(self, 0);
                self->started_ = true;
            } catch (...) {
                failure = std::current_exception();
            }
            self->starting_ = false;
            self->changed_.notify();
        }
        if (failure) {
            self->request_close();
            co_await shutdown_owned(self);
            std::rethrow_exception(failure);
        }
    }
    static task<void> add_owned(std::shared_ptr<http_server_state> self, std::shared_ptr<owned_listen_configs> input, bool external,
        std::shared_ptr<prepared_listener_state> transaction = {}) {
        self->require_current();
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_open();
        if (!external) {
            self->require_running();
        }
        activity active(*self);
        const bool blocking = input->needs_tls_preparation();
        auto candidate = co_await prepare_work(self->worker_, blocking, [input = std::move(input), config = self->options_.server_, callbacks = self->callbacks_, origin = self->key_origin_, external, running = self->started_] {
            auto candidate = load_candidate({input}, external, config, callbacks, 1, origin);
            if (running) {
                prepare_sessions(candidate, 1, static_cast<bool>(callbacks->tls_client_hello_));
            }
            return candidate;
        });
        self->require_open();
        const auto first = self->services_.size();
        self->append(candidate.definitions_, external);
        if (!self->started_) {
            co_return;
        }
        std::exception_ptr failure;
        try {
            co_await prepare_range(self, first, std::move(candidate.sessions_.front()));
            co_await open_range(self, first, std::move(transaction));
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            try {
                co_await discard_range(self, first);
            } catch (...) {
                self->request_close();
            }
            std::rethrow_exception(failure);
        }
    }
    static task<void> wait_connections_owned(std::shared_ptr<http_server_state> self, owned_endpoint endpoint, operation_options options) {
        self->require_current();
        self->require_running();
        activity active(*self);
        co_await connection_count::wait(self->connections_.listener_connections(service_endpoint(endpoint.view())),
            self->worker_, std::move(options), self->token_);
    }
    static task<void> replace_owned(std::shared_ptr<http_server_state> self, owned_endpoint endpoint,
        std::shared_ptr<owned_listen_configs> replacement) {
        self->require_current();
        // Declared before the lease so a selector destructor can reenter control.
        std::shared_ptr<const http_server_tls_identities> retired;
        auto lease = co_await self->gate_.acquire(self->worker_);
        self->require_running();
        activity active(*self);
        const auto index = self->find(endpoint.view());
        const auto* policy = std::get_if<http_server_listener_definition::tls>(&self->services_[index].definition_.transport_);
        if (!policy) {
            throw std::invalid_argument("the connection service does not use TLS");
        }
        ensure_same_http_server_tls_policy(*policy, replacement->configs_.front().tls_);
        auto identities = co_await prepare_work(self->worker_, true, [replacement] {
            auto loaded = replacement->load();
            return std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
        });
        self->require_running();
        if (self->find(endpoint.view()) != index) {
            throw std::logic_error("TLS service changed during preparation");
        }
        auto& current = std::get<http_server_listener_definition::tls>(self->services_[index].definition_.transport_);
        retired = current.identities_;
        self->connections_.adopt_tls_identities(index, identities);
        current.identities_ = std::move(identities);
    }
    void finish_closed() noexcept {
        std::vector<runtime_control_completion::receipt> waiters;
        {
            std::lock_guard lock(closed_mutex_);
            closed_.store(true, std::memory_order_release);
            waiters.swap(closed_waiters_);
        }
        for (const auto& waiter : waiters) {
            waiter.complete();
        }
        group_retirement_.reset();
    }
    void subscribe_closed(runtime_control_completion::receipt receipt) {
        {
            std::lock_guard lock(closed_mutex_);
            if (!closed_.load(std::memory_order_acquire)) {
                closed_waiters_.push_back(std::move(receipt));
                return;
            }
        }
        receipt.complete();
    }
    void begin_close() noexcept {
        require_current();
        closing_.store(true, std::memory_order_release);
        stop_.request_stop();
        if (close_started_) {
            return;
        }
        close_started_ = true;
#if defined(_WIN32)
        if (acceptor_) {
            acceptor_->stop_admission();
        }
#endif
        phase_ = http_server_worker_state::stopped;
        connections_.stop();
        capabilities_.close_now();
        try {
            ruvia::async_start(cleanup(), asio::bind_executor(loop_.executor(), [self = shared_from_this()](std::exception_ptr failure) noexcept {
                if (failure) {
                    {
                        std::lock_guard lock(self->failure_mutex_);
                        if (!self->failure_) {
                            self->failure_ = failure;
                        }
                    }
                    fail_server_group(self->group_, failure);
                }
                self->finish_closed();
                self->changed_.notify();
            }));
        } catch (...) {
            std::terminate();
        }
    }
    task<void> cleanup() {
        while (starting_ || controls_) {
            co_await changed_.wait();
        }
        std::exception_ptr failure;
        try {
            co_await tasks_.join();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await capabilities_.stop();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        connections_.retire_tls();
        if (!grouped_) {
            for (auto& entry : services_) {
                entry.definition_.inherited_sockets_.reset();
            }
        }
#if defined(_WIN32)
        // One native thread barrier per acceptor. The first member's cleanup
        // remains live on its loop until the HANDLE wait and reclaim finish.
        if (acceptor_ && (!grouped_ || worker_index_ == 0)) {
            acceptor_->request_stop();
            co_await acceptor_->join(worker_, loop_.io_context());
        }
#endif
        staged_.clear();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> shutdown_owned(std::shared_ptr<http_server_state> self, bool observe = true) {
        self->require_current();
        self->request_close();
        runtime_control_completion completion(self->worker_);
        self->subscribe_closed(completion.make_receipt());
        co_await completion.wait();
        (void)completion.failure();
        if (!observe) {
            co_return;
        }
        std::exception_ptr failure;
        {
            std::lock_guard lock(self->failure_mutex_);
            failure = self->failure_;
            self->failure_observed_ = true;
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<void> shutdown_serialized(std::shared_ptr<http_server_state> self) {
        self->require_current();
        self->request_close();
        auto lease = co_await self->gate_.acquire(self->worker_);
        co_await shutdown_owned(self);
    }

    event_loop loop_;
    worker_ref worker_;
    std::shared_ptr<http_server_callbacks> callbacks_;
    http_server_options options_;
    inbound_buffer_budget inbound_budget_;
    worker_codecs codecs_;
    compiled_route_plan plan_;
    controller_store controllers_;
    route_table routes_;
    stop_source stop_;
    stop_token token_{stop_.token()};
    http_server_worker_state phase_{http_server_worker_state::fresh};
    task_scope tasks_;
    worker_capabilities capabilities_;
#if defined(RUVIA_ENABLE_HTTP3)
    std::shared_ptr<http3_worker_directory> directory_;
#endif
#if defined(_WIN32)
    std::shared_ptr<windows_acceptor> acceptor_;
#endif
    worker_connections connections_;
    std::vector<service> services_;
    std::vector<std::shared_ptr<owned_listen_configs>> staged_;
    std::size_t http3_slots_{};
    std::chrono::steady_clock::time_point key_origin_{std::chrono::steady_clock::now()};
    worker_signal changed_;
    runtime_control_gate gate_;
    std::size_t controls_{};
    bool starting_{};
    bool started_{};
    bool close_started_{};
    std::atomic<bool> closing_{};
    std::atomic<bool> closed_{};
    std::atomic<bool> io_started_{};
    std::mutex closed_mutex_;
    std::vector<runtime_control_completion::receipt> closed_waiters_;
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    std::atomic<std::size_t> fatal_count_{};
    bool grouped_{};
#if defined(_WIN32)
    std::size_t worker_index_{};
#endif
    std::weak_ptr<http_server_group_state> group_;
    std::shared_ptr<http_server_group_state> group_retirement_;
    event_loop_stop_registration stop_registration_;
};

class http_server_group_state final : public std::enable_shared_from_this<http_server_group_state> {
public:
    http_server_group_state(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config)
        : callbacks_factory_(std::move(callbacks)) {
        if (!callbacks_factory_) {
            throw std::invalid_argument("HTTP server group requires a callback factory");
        }
        if (loops.empty()) {
            throw std::invalid_argument("HTTP server group requires at least one event loop");
        }
        for (std::size_t index = 0; index != loops.size(); ++index) {
            if (!loops[index].valid() || !loops[index].worker().accepting()) {
                throw std::invalid_argument("HTTP server group requires valid event loops");
            }
            for (std::size_t previous = 0; previous != index; ++previous) {
                if (loops[index].worker().id() == loops[previous].worker().id()) {
                    throw std::invalid_argument("HTTP server group requires distinct event loops");
                }
            }
        }
#if defined(RUVIA_ENABLE_HTTP3)
        directory_ = std::make_shared<http3_worker_directory>(loops.size());
#endif
        workers_.reserve(loops.size());
        for (std::size_t index = 0; index != loops.size(); ++index) {
            auto worker = std::make_shared<http_server_state>(loops[index], std::make_shared<http_server_callbacks>(), config, index, loops.size()
#if defined(RUVIA_ENABLE_HTTP3)
                                                                                                                                          ,
                directory_
#endif
            );
            worker->grouped_ = true;
#if defined(RUVIA_ENABLE_HTTP3)
            directory_->bind_worker(index, worker->worker_);
#endif
            workers_.push_back(std::move(worker));
        }
#if defined(_WIN32)
        acceptor_ = std::make_shared<windows_acceptor>(std::span<const http_server_listener_definition>{}, loops.size(),
#if defined(RUVIA_ENABLE_HTTP3)
            directory_.get(),
#endif
            worker_failure_sink{.target_ = this, .invoke_ = [](void* target, const std::exception_ptr& error) noexcept {
                                    auto& self = *static_cast<http_server_group_state*>(target);
                                    http_server_state::fail(self.workers_.front().get(), error);
                                }});
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            workers_[index]->acceptor_ = acceptor_;
            workers_[index]->connections_.bind_acceptor(*acceptor_);
            acceptor_->bind_worker(index, workers_[index]->connections_.acceptor_target());
        }
#endif
    }
    void bind_stop() {
        try {
            for (auto& worker : workers_) {
                worker->group_ = shared_from_this();
                worker->bind_stop();
            }
        } catch (...) {
            close();
            throw;
        }
    }
    void stage(std::shared_ptr<owned_listen_configs> input) {
        std::lock_guard lock(config_mutex_);
        require_open();
        if (startup_requested_) {
            throw std::logic_error("listen() is only available before HTTP server group startup");
        }
        staged_.push_back(std::move(input));
    }
    void trusted_proxies(trusted_proxy_config config) {
        std::lock_guard lock(config_mutex_);
        require_open();
        if (startup_requested_) {
            throw std::logic_error("cannot change trusted proxies after HTTP server group startup");
        }
        http_server_options validated;
        validated.set_trusted_proxies(config);
        std::vector<http_client_ip_extractor> candidates;
        candidates.reserve(workers_.size());
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            candidates.emplace_back();
            candidates.back() = validated.trusted_proxies_;
        }
        for (std::size_t index = 0; index != workers_.size(); ++index) {
            workers_[index]->options_.trusted_proxies_ = std::move(candidates[index]);
        }
    }
    task<void> start(worker_ref caller) {
        return start_owned(shared_from_this(), std::move(caller));
    }
    task<void> add(worker_ref caller, std::shared_ptr<owned_listen_configs> input, bool external) {
        return add_owned(shared_from_this(), std::move(caller), std::move(input), external);
    }
    task<prepared_listener> prepare(worker_ref caller, std::shared_ptr<owned_listen_configs> input) {
        auto transaction = prepared_listener_access::make(caller);
        return prepared_listener_access::launch(transaction, add_owned(shared_from_this(), std::move(caller), std::move(input), false, transaction));
    }
    task<void> remove(worker_ref caller, owned_endpoint endpoint, listener_remove_options options) {
        return remove_owned(shared_from_this(), std::move(caller), std::move(endpoint), options);
    }
    task<void> replace(worker_ref caller, owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        return replace_owned(shared_from_this(), std::move(caller), std::move(endpoint), std::move(replacement));
    }
    task<bool> close_connection(worker_ref caller, std::uint64_t id) {
        return close_connection_owned(shared_from_this(), std::move(caller), id);
    }
    task<void> wait_connections(worker_ref caller, owned_endpoint endpoint, operation_options options) {
        return wait_connections_owned(shared_from_this(), std::move(caller), std::move(endpoint), std::move(options));
    }
#ifndef _WIN32
    task<listener_sockets> duplicate(worker_ref caller, owned_endpoint endpoint) {
        return duplicate_owned(shared_from_this(), std::move(caller), std::move(endpoint));
    }
#endif
    task<void> adopt(worker_ref caller, std::size_t index, owned_endpoint endpoint, socket_owner socket, owned_adopt_options options) {
        return adopt_owned(shared_from_this(), std::move(caller), index, std::move(endpoint), std::move(socket), std::move(options));
    }
    task<void> shutdown(worker_ref caller) {
        return shutdown_owned(shared_from_this(), std::move(caller));
    }
    void record_failure(const std::exception_ptr& failure) noexcept {
        {
            std::lock_guard lock(failure_mutex_);
            if (!failure_) {
                failure_ = failure;
            }
        }
        close();
    }
    void close() noexcept {
        if (closing_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        for (const auto& worker : workers_) {
            worker->request_close();
        }
#if defined(_WIN32)
        acceptor_->request_stop();
#endif
    }
    http_server_stats stats() const {
        http_server_stats result;
        for (const auto& worker : workers_) {
            const auto value = worker->stats();
            result.active_connections_ += value.active_connections_;
            result.connections_refused_ += value.connections_refused_;
            result.connection_failures_ += value.connection_failures_;
            result.accept_failures_ += value.accept_failures_;
            result.worker_failures_ += value.worker_failures_;
            append_listener_stats(result, value);
        }
        return result;
    }
    std::size_t worker_count() const noexcept {
        return workers_.size();
    }
    const worker_ref& worker(std::size_t index) const {
        return workers_.at(index)->worker_;
    }

private:
    task<void> build_callbacks(worker_ref caller) {
        if (!callbacks_factory_) {
            co_return;
        }
        std::exception_ptr failure;
        try {
            for (const auto& worker : workers_) {
                co_await control(caller, worker, [this](http_server_state& state) {
                    auto callbacks = callbacks_factory_(state.worker_);
                    *state.callbacks_ = std::move(callbacks);
                    const auto configured = standalone_options(state.options_.server_, *state.callbacks_, workers_.size());
                    state.options_.connection_ = configured.connection_;
                    state.options_.request_stream_ = configured.request_stream_;
                    state.options_.access_log_.callback_ = configured.access_log_.callback_;
                    state.options_.connection_failure_.callback_ = configured.connection_failure_.callback_;
                    state.options_.tls_client_hello_ = configured.tls_client_hello_;
                    state.options_.http2_input_ = configured.http2_input_;
#if defined(RUVIA_ENABLE_HTTP2)
                    state.options_.http2_frame_ = configured.http2_frame_;
                    state.options_.http2_headers_ = configured.http2_headers_;
#endif
                    state.options_.connection_admission_ = configured.connection_admission_;
                    state.options_.connection_open_ = configured.connection_open_;
                    state.options_.connection_close_ = configured.connection_close_;
                    if (&state != workers_.front().get()) {
                        const auto& first = *workers_.front()->callbacks_;
                        const auto& current = *state.callbacks_;
                        if (static_cast<bool>(first.connection_) != static_cast<bool>(current.connection_) ||
                            static_cast<bool>(first.request_stream_) != static_cast<bool>(current.request_stream_) ||
                            static_cast<bool>(first.access_) != static_cast<bool>(current.access_) ||
                            static_cast<bool>(first.connection_failure_) != static_cast<bool>(current.connection_failure_) ||
                            static_cast<bool>(first.tls_client_hello_) != static_cast<bool>(current.tls_client_hello_) ||
                            static_cast<bool>(first.http2_input_) != static_cast<bool>(current.http2_input_) ||
#if defined(RUVIA_ENABLE_HTTP2)
                            static_cast<bool>(first.http2_frame_) != static_cast<bool>(current.http2_frame_) ||
                            static_cast<bool>(first.http2_headers_) != static_cast<bool>(current.http2_headers_) ||
#endif
                            static_cast<bool>(first.connection_admission_) != static_cast<bool>(current.connection_admission_) ||
                            static_cast<bool>(first.connection_open_) != static_cast<bool>(current.connection_open_) ||
                            static_cast<bool>(first.connection_close_) != static_cast<bool>(current.connection_close_)) {
                            throw std::invalid_argument("HTTP server group callbacks must have identical presence");
                        }
                    }
                });
            }
        } catch (...) {
            failure = std::current_exception();
        }
        callbacks_factory_ = {};
        if (failure) {
            close();
            co_await join_closed(caller);
            std::rethrow_exception(failure);
        }
    }
    // Every closure owns its receipt, including the spawned coroutine callback.
    // A failed post is joined as well, before any borrowed command state dies.
    template <typename function_type>
    static task<void> control(worker_ref caller, std::shared_ptr<http_server_state> worker, function_type function) {
        runtime_control_completion completion(caller);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = worker->loop_.defer_cleanup(completion.make_command(
                [worker, function = std::move(function)](const auto& receipt) mutable {
                    try {
                        function(*worker);
                        receipt.complete();
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server group worker has retired");
        }
    }
    template <typename function_type>
    static task<void> async_control(worker_ref caller, std::shared_ptr<http_server_state> worker, function_type function) {
        runtime_control_completion completion(caller);
        bool posted = false;
        std::exception_ptr posting_failure;
        try {
            posted = worker->loop_.defer_cleanup(completion.make_command(
                [worker, function = std::move(function)](const auto& receipt) mutable {
                    try {
                        // Keep the callable address-stable until the task and
                        // its completion callback have both been reclaimed.
                        auto operation = std::make_shared<function_type>(std::move(function));
                        auto finish = receipt;
                        ++worker->controls_;
                        try {
                            ruvia::async_start((*operation)(worker), asio::bind_executor(worker->loop_.executor(), std::move(finish).bind([worker, operation](const auto& done, std::exception_ptr failure) {
                                --worker->controls_;
                                worker->changed_.notify();
                                done.complete(failure);
                            })));
                        } catch (...) {
                            --worker->controls_;
                            worker->changed_.notify();
                            throw;
                        }
                    } catch (...) {
                        receipt.complete(std::current_exception());
                    }
                }));
        } catch (...) {
            posting_failure = std::current_exception();
        }
        co_await completion.wait();
        if (const auto failure = completion.failure()) {
            std::rethrow_exception(failure);
        }
        if (posting_failure) {
            std::rethrow_exception(posting_failure);
        }
        if (!posted || completion.completed() != 1) {
            throw std::logic_error("HTTP server group worker has retired");
        }
    }
    static task<void> open_prepared(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::size_t first, std::shared_ptr<prepared_listener_state> transaction = {}) {
        runtime_control_completion completion(caller);
        std::vector<move_only_function<void()>> commands;
        commands.reserve(self->workers_.size());
        for (const auto& worker : self->workers_) {
            commands.emplace_back(completion.make_command([worker, first](const auto& receipt) noexcept {
                if (!worker->closing_.load(std::memory_order_acquire)) {
                    for (auto index = first; index != worker->services_.size(); ++index) {
                        if (worker->services_[index].active_) {
                            worker->connections_.open_listener(index);
                            worker->services_[index].published_ = true;
                        }
                    }
                    worker->started_ = true;
                }
                receipt.complete();
            }));
        }
#if defined(_WIN32)
        runtime_control_completion accepting_completion(caller);
        move_only_function<void()> accepting(accepting_completion.make_command([self, first](const auto& receipt) noexcept {
            const auto& services = self->workers_.front()->services_;
            for (auto index = first; index != services.size(); ++index) {
                if (services[index].active_ && !services[index].external_) {
                    self->acceptor_->open_listener(index);
                }
            }
            receipt.complete();
        }));
#endif
        if (transaction) {
            co_await transaction->checkpoint(self->workers_.front()->token_);
        }
        self->require_open();
        bool posted = true;
        std::exception_ptr failure;
        try {
            for (std::size_t index = 0; index != commands.size() && posted; ++index) {
                posted = self->workers_[index]->loop_.defer_cleanup(std::move(commands[index]));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        commands.clear();
        co_await completion.wait();
        (void)completion.failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!posted) {
            throw std::logic_error("HTTP server group worker has retired");
        }
        self->require_open();
#if defined(_WIN32)
        posted = self->acceptor_->post_control(std::move(accepting));
        accepting = nullptr;
        co_await accepting_completion.wait();
        (void)accepting_completion.failure();
        if (!posted) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
#endif
        self->require_open();
    }
    static task<void> start_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_open();
        std::vector<std::shared_ptr<owned_listen_configs>> inputs;
        {
            std::lock_guard lock(self->config_mutex_);
            if (self->startup_requested_) {
                throw std::logic_error("HTTP server group can only start once");
            }
            self->startup_requested_ = true;
            inputs.swap(self->staged_);
        }
        std::exception_ptr failure;
        try {
            co_await self->build_callbacks(caller);
            auto existing = std::make_shared<listener_candidate>();
            co_await control(caller, self->workers_.front(), [existing](http_server_state& state) {
                for (const auto& entry : state.services_) {
                    existing->definitions_.push_back(entry.definition_.clone());
                }
            });
            const auto first = existing->definitions_.size();
            const auto& source = self->workers_.front();
            const bool blocking = needs_tls_preparation(inputs, existing->definitions_);
            co_await prepare_work(caller, blocking, [inputs = std::move(inputs), existing, config = source->options_.server_, callbacks = source->callbacks_, count = self->workers_.size(), origin = source->key_origin_]() mutable {
                auto loaded = load_candidate(inputs, false, config, callbacks, count, origin);
                ensure_new_listener_endpoints(existing->definitions_, loaded.definitions_);
                for (auto& definition : loaded.definitions_) {
                    existing->definitions_.push_back(std::move(definition));
                }
                prepare_sessions(*existing, count, static_cast<bool>(callbacks->tls_client_hello_));
            });
            self->require_open();
            auto added = std::make_shared<definitions>();
            for (auto index = first; index != existing->definitions_.size(); ++index) {
                added->push_back(existing->definitions_[index].clone());
            }
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [added](http_server_state& state) { state.append(*added, false); });
            }
            self->listener_slots_ = existing->definitions_.size();
#if defined(_WIN32)
            co_await self->acceptor_->start(caller);
            co_await self->acceptor_->serve(caller);
#endif
            // Every owner prepares, but none admits before the final prepare.
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await async_control(caller, self->workers_[index], [prepared = std::move(existing->sessions_[index])](auto worker) mutable {
                    return http_server_state::prepare_start_owned(std::move(worker), std::move(prepared));
                });
            }
            if (!caller.accepting()) {
                throw std::logic_error("HTTP server caller is stopping");
            }
            self->require_open();
            co_await open_prepared(self, caller, 0);
            self->require_open();
            self->started_ = true;
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            self->close();
            co_await self->join_closed(caller);
            if (const auto fatal = self->observe_failure()) {
                std::rethrow_exception(fatal);
            }
            std::rethrow_exception(failure);
        }
    }
    static task<void> add_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::shared_ptr<owned_listen_configs> input, bool external, std::shared_ptr<prepared_listener_state> transaction = {}) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_open();
        if (!external) {
            self->require_running();
        }
        co_await self->build_callbacks(caller);
        const auto& source = self->workers_.front();
        const bool blocking = input->needs_tls_preparation();
        auto prepared = co_await prepare_work(caller, blocking, [input = std::move(input), external, config = source->options_.server_, callbacks = source->callbacks_, count = self->workers_.size(), origin = source->key_origin_, running = self->started_] {
            auto candidate = load_candidate({input}, external, config, callbacks, count, origin);
            if (running) {
                prepare_sessions(candidate, count, static_cast<bool>(callbacks->tls_client_hello_));
            }
            return candidate;
        });
        auto candidate = std::make_shared<listener_candidate>(std::move(prepared));
        self->require_open();
        for (const auto& worker : self->workers_) {
            co_await control(caller, worker, [candidate](http_server_state& state) {
                state.validate_new(candidate->definitions_);
                state.services_.reserve(state.services_.size() + candidate->definitions_.size());
            });
        }
        auto staged = std::make_shared<std::vector<std::vector<http_server_state::service>>>();
        staged->reserve(self->workers_.size());
        auto next_http3 = source->http3_slots_;
        for (std::size_t worker = 0; worker != self->workers_.size(); ++worker) {
            auto& entries = staged->emplace_back();
            entries.reserve(candidate->definitions_.size());
            next_http3 = source->http3_slots_;
            for (const auto& definition : candidate->definitions_) {
                const auto slot = definition.http3_ ? std::optional<std::size_t>(next_http3++) : std::nullopt;
                entries.push_back({definition.clone(), external, true, slot});
            }
        }
        const auto first = self->listener_slots_;
        std::exception_ptr failure;
        bool appended = false;
        if (!caller.accepting()) {
            throw std::logic_error("HTTP server caller is stopping");
        }
        try {
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await control(caller, self->workers_[index], [staged, index, next_http3](http_server_state& state) {
                    state.require_open();
                    for (auto& entry : (*staged)[index]) {
                        state.services_.push_back(std::move(entry));
                    }
                    state.http3_slots_ = next_http3;
                });
            }
            self->listener_slots_ += candidate->definitions_.size();
            appended = true;
            if (!self->started_) {
                co_return;
            }
            for (std::size_t index = 0; index != self->workers_.size(); ++index) {
                co_await async_control(caller, self->workers_[index], [first, prepared = std::move(candidate->sessions_[index])](auto owner) mutable {
                    return http_server_state::prepare_range(std::move(owner), first, std::move(prepared));
                });
            }
            if (!caller.accepting()) {
                throw std::logic_error("HTTP server caller is stopping");
            }
            self->require_open();
            co_await open_prepared(self, caller, first, std::move(transaction));
            self->require_open();
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            if (!appended) {
                self->close();
                co_await self->join_closed(caller);
                std::rethrow_exception(failure);
            }
            for (const auto& worker : self->workers_) {
                try {
                    co_await async_control(caller, worker, [first](auto owner) { return http_server_state::discard_range(std::move(owner), first); });
                } catch (...) {
                    self->close();
                }
            }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
            for (std::size_t index = 0; index != self->workers_.front()->http3_slots_; ++index) {
                self->directory_->close_unclaimed_listener_sockets(index);
            }
#endif
            std::rethrow_exception(failure);
        }
    }
    static task<void> remove_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, owned_endpoint endpoint, listener_remove_options options) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        // Removal's native accepts hold this receipt until their completion.
        runtime_control_completion completion(caller);
        auto lifetime = std::make_shared<runtime_control_completion::receipt>(completion.make_receipt());
        std::exception_ptr failure;
        try {
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [&endpoint, lifetime, options](http_server_state& state) { state.remove(endpoint.view(), lifetime, options); });
            }
        } catch (...) {
            failure = std::current_exception();
        }
        lifetime.reset();
        co_await completion.wait();
        (void)completion.failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    static task<bool> close_connection_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, std::uint64_t id) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        for (const auto& worker : self->workers_) {
            bool closed = false;
            co_await control(caller, worker, [&closed, id](http_server_state& state) { closed = state.close_connection(id); });
            if (closed) {
                co_return true;
            }
        }
        co_return false;
    }
    static task<void> replace_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        owned_endpoint endpoint, std::shared_ptr<owned_listen_configs> replacement) {
        struct target final {
            std::size_t slot_;
            std::shared_ptr<const http_server_tls_identities> retired_;
        };
        std::vector<target> targets;
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        targets.reserve(self->workers_.size());
        for (const auto& worker : self->workers_) {
            co_await control(caller, worker, [&targets, &endpoint, replacement](http_server_state& state) {
                state.require_running();
                const auto slot = state.find(endpoint.view());
                const auto* policy = std::get_if<http_server_listener_definition::tls>(&state.services_[slot].definition_.transport_);
                if (!policy) {
                    throw std::invalid_argument("the connection service does not use TLS");
                }
                ensure_same_http_server_tls_policy(*policy, replacement->configs_.front().tls_);
                targets.push_back({slot, policy->identities_});
            });
        }
        auto identities = co_await prepare_work(caller, true,
            [replacement] {
                auto loaded = replacement->load();
                return std::get<http_server_listener_definition::tls>(loaded.front().transport_).identities_;
            });
        self->require_running();
        for (std::size_t index = 0; index != self->workers_.size(); ++index) {
            co_await control(caller, self->workers_[index], [slot = targets[index].slot_, identities](http_server_state& state) {
                state.require_running();
                state.connections_.adopt_tls_identities(slot, identities);
                std::get<http_server_listener_definition::tls>(state.services_[slot].definition_.transport_).identities_ = identities;
            });
        }
    }
#ifndef _WIN32
    static task<listener_sockets> duplicate_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller, owned_endpoint endpoint) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        listener_sockets result;
        result.http_.reserve(self->workers_.size());
        result.https_.reserve(self->workers_.size());
        result.http3_.reserve(self->workers_.size());
        try {
            for (const auto& worker : self->workers_) {
                co_await control(caller, worker, [&result, &endpoint](http_server_state& state) {
                    auto sockets = state.duplicate(endpoint.view());
                    result.http_.insert(result.http_.end(), sockets.http_.begin(), sockets.http_.end());
                    result.https_.insert(result.https_.end(), sockets.https_.begin(), sockets.https_.end());
                    result.http3_.insert(result.http3_.end(), sockets.http3_.begin(), sockets.http3_.end());
                });
            }
        } catch (...) {
            for (const auto socket : result.http_) {
                close_native_socket(socket);
            }
            for (const auto socket : result.https_) {
                close_native_socket(socket);
            }
            for (const auto socket : result.http3_) {
                close_native_socket(socket);
            }
            throw;
        }
        co_return result;
    }
#endif
    static task<void> adopt_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        std::size_t index, owned_endpoint endpoint, socket_owner socket, owned_adopt_options options) {
        auto lease = co_await self->gate_.acquire(caller);
        self->require_running();
        if (index >= self->workers_.size()) {
            throw std::out_of_range("HTTP server group worker index");
        }
        co_await control(caller, self->workers_[index], [&socket, &endpoint, &options](http_server_state& state) {
            state.adopt(endpoint.view(), std::exchange(socket.socket_, invalid_native_socket), options.view());
        });
    }
    static task<void> wait_connections_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller,
        owned_endpoint endpoint, operation_options options) {
        operation_steps steps(options);
        std::vector<std::shared_ptr<connection_count>> counts;
        std::vector<stop_token> stops;
        {
            auto lease = co_await self->gate_.acquire(caller);
            self->require_running();
            const auto target = service_endpoint(endpoint.view());
            counts.reserve(self->workers_.size());
            stops.reserve(self->workers_.size());
            for (const auto& worker : self->workers_) {
                counts.push_back(worker->connections_.listener_connections(target));
                stops.push_back(worker->token_);
            }
        }
        stop_group stopping(stops);
        for (const auto& count : counts) {
            co_await connection_count::wait(count, caller, steps.next("wait_connections_closed"), stopping.token());
        }
    }
    static task<void> shutdown_owned(std::shared_ptr<http_server_group_state> self, worker_ref caller) {
        self->close();
        auto lease = co_await self->gate_.acquire(caller);
        co_await self->join_closed(caller);
        if (const auto failure = self->observe_failure()) {
            std::rethrow_exception(failure);
        }
    }
    task<void> join_closed(worker_ref caller) {
        runtime_control_completion completion(caller);
        std::exception_ptr registration_failure;
        try {
            for (const auto& worker : workers_) {
                worker->subscribe_closed(completion.make_receipt());
            }
        } catch (...) {
            registration_failure = std::current_exception();
        }
        co_await completion.wait();
        (void)completion.failure();
        if (registration_failure) {
            std::rethrow_exception(registration_failure);
        }
        for (const auto& worker : workers_) {
            for (auto& entry : worker->services_) {
                entry.definition_.inherited_sockets_.reset();
            }
        }
#if defined(RUVIA_ENABLE_HTTP3) && !defined(_WIN32)
        for (std::size_t index = 0; index != workers_.front()->http3_slots_; ++index) {
            directory_->close_unclaimed_listener_sockets(index);
        }
#endif
        std::vector<std::shared_ptr<owned_listen_configs>> staged;
        {
            std::lock_guard lock(config_mutex_);
            staged.swap(staged_);
        }
    }
    std::exception_ptr observe_failure() noexcept {
        std::exception_ptr failure;
        {
            std::lock_guard lock(failure_mutex_);
            if (!failure_observed_) {
                failure = failure_;
            }
            failure_observed_ = true;
        }
        for (const auto& worker : workers_) {
            std::lock_guard lock(worker->failure_mutex_);
            if (!worker->failure_observed_ && !failure) {
                failure = worker->failure_;
            }
            worker->failure_observed_ = true;
        }
        return failure;
    }
    void require_open() const {
        if (closing_.load(std::memory_order_acquire)) {
            throw std::logic_error("HTTP server group is closing");
        }
        for (const auto& worker : workers_) {
            if (!worker->loop_.valid() || !worker->loop_.worker().accepting()) {
                throw std::logic_error("HTTP server group worker is stopping");
            }
        }
    }
    void require_running() const {
        require_open();
        if (!started_) {
            throw std::logic_error("HTTP server group is not started");
        }
    }
    std::vector<std::shared_ptr<http_server_state>> workers_;
    http_server_callbacks_factory callbacks_factory_;
#if defined(RUVIA_ENABLE_HTTP3)
    std::shared_ptr<http3_worker_directory> directory_;
#endif
#if defined(_WIN32)
    std::shared_ptr<windows_acceptor> acceptor_;
#endif
    runtime_control_gate gate_;
    std::mutex config_mutex_;
    std::vector<std::shared_ptr<owned_listen_configs>> staged_;
    bool startup_requested_{};
    std::atomic<bool> closing_{};
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    bool started_{};
    std::size_t listener_slots_{};
};

void close_server_group(const std::weak_ptr<http_server_group_state>& group) noexcept {
    if (const auto owner = group.lock()) {
        owner->close();
    }
}
void fail_server_group(const std::weak_ptr<http_server_group_state>& group, const std::exception_ptr& failure) noexcept {
    if (const auto owner = group.lock()) {
        owner->record_failure(failure);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

prepared_listener::prepared_listener(std::shared_ptr<detail::prepared_listener_state> state) noexcept
    : state_(std::move(state)) {}
prepared_listener::prepared_listener(prepared_listener&&) noexcept = default;
prepared_listener& prepared_listener::operator=(prepared_listener&& other) noexcept {
    if (this != &other) {
        auto retired = std::exchange(state_, std::move(other.state_));
        if (retired) {
            retired->decide(false);
        }
    }
    return *this;
}
prepared_listener::~prepared_listener() {
    if (auto retired = std::exchange(state_, {})) {
        retired->decide(false);
    }
}
task<void> prepared_listener::activate() && {
    return detail::prepared_listener_access::finish(std::move(*this), true);
}
task<void> prepared_listener::cancel() && {
    return detail::prepared_listener_access::finish(std::move(*this), false);
}
bool prepared_listener::valid() const noexcept {
    return state_ != nullptr;
}

http_server::http_server(event_loop loop, http_server_callbacks callbacks, server_config config)
    : state_(std::make_shared<detail::http_server_state>(std::move(loop), std::make_shared<http_server_callbacks>(std::move(callbacks)), config)) {
    state_->bind_stop();
}
http_server::~http_server() {
    state_->request_close();
}
http_server& http_server::listen(listen_config config) & {
    const auto state = state_;
    state->stage(detail::own_listen(std::move(config)));
    return *this;
}
task<prepared_listener> http_server::prepare_listener(listen_config config) & {
    const auto state = state_;
    return state->prepare(detail::own_listen(std::move(config)));
}
task<prepared_listener> http_server::prepare_listeners(std::span<const listen_config> configs) & {
    const auto state = state_;
    return state->prepare(detail::own_listen(configs));
}
task<void> http_server::add_listener(listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(config)));
}
task<void> http_server::add_connection_service(listen_config config) & {
    const auto state = state_;
    return state->add(detail::own_listen(std::move(config)), true);
}
void http_server::adopt(const listener_endpoint& service, native_socket socket, adopt_options options) & {
    state_->adopt(service, socket, options);
}
bool http_server::close_connection(std::uint64_t id) & {
    return state_->close_connection(id);
}
http_server& http_server::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
task<void> http_server::start() & {
    const auto state = state_;
    return state->start();
}
void http_server::remove_listener(const listener_endpoint& endpoint, listener_remove_options options) & {
    state_->remove(endpoint, {}, options);
}
task<void> http_server::wait_connections_closed(const listener_endpoint& endpoint, operation_options options) & {
    return state_->wait_connections(detail::owned_endpoint(endpoint), std::move(options));
}
task<void> http_server::replace_tls_identities(const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    auto replacement = detail::own_listen(listen_config{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection});
    return state->replace(detail::owned_endpoint(endpoint), std::move(replacement));
}
#ifndef _WIN32
listener_sockets http_server::duplicate_listener_sockets(const listener_endpoint& endpoint) const& {
    return state_->duplicate(endpoint);
}
#endif
void http_server::close() & noexcept {
    state_->request_close();
}
task<void> http_server::shutdown() & {
    const auto state = state_;
    return state->shutdown();
}
http_server_stats http_server::stats() const {
    return state_->stats();
}
const worker_ref& http_server::worker() const& noexcept {
    return state_->worker();
}

http_server_group::http_server_group(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config)
    : state_(std::make_shared<detail::http_server_group_state>(loops, std::move(callbacks), config)) {
    state_->bind_stop();
}
http_server_group::~http_server_group() {
    state_->close();
}
http_server_group& http_server_group::listen(listen_config config) & {
    const auto state = state_;
    state->stage(detail::own_listen(std::move(config)));
    return *this;
}
task<prepared_listener> http_server_group::prepare_listener(worker_ref caller, listen_config config) & {
    const auto state = state_;
    return state->prepare(std::move(caller), detail::own_listen(std::move(config)));
}
task<prepared_listener> http_server_group::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    const auto state = state_;
    return state->prepare(std::move(caller), detail::own_listen(configs));
}
task<void> http_server_group::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}
task<void> http_server_group::add_connection_service(worker_ref caller, listen_config config) & {
    const auto state = state_;
    return state->add(std::move(caller), detail::own_listen(std::move(config)), true);
}
http_server_group& http_server_group::trusted_proxies(trusted_proxy_config config) & {
    state_->trusted_proxies(std::move(config));
    return *this;
}
task<void> http_server_group::start(worker_ref caller) & {
    const auto state = state_;
    return state->start(std::move(caller));
}
task<void> http_server_group::remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options) & {
    const auto state = state_;
    return state->remove(std::move(caller), detail::owned_endpoint(endpoint), options);
}
task<void> http_server_group::wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options) & {
    return state_->wait_connections(std::move(caller), detail::owned_endpoint(endpoint), std::move(options));
}
task<void> http_server_group::replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    const auto state = state_;
    auto replacement = detail::own_listen(listen_config{.address_ = endpoint.address_, .https_ = endpoint.port_, .tls_ = std::move(config), .dispatch_ = listener_dispatch::connection});
    return state->replace(std::move(caller), detail::owned_endpoint(endpoint), std::move(replacement));
}
#ifndef _WIN32
task<listener_sockets> http_server_group::duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const& {
    const auto state = state_;
    return state->duplicate(std::move(caller), detail::owned_endpoint(endpoint));
}
#endif
task<void> http_server_group::adopt(worker_ref caller, std::size_t index, const listener_endpoint& endpoint, native_socket socket, adopt_options options) & {
    const auto state = state_;
    detail::socket_owner owner(socket);
    return state->adopt(std::move(caller), index, detail::owned_endpoint(endpoint), std::move(owner), detail::owned_adopt_options(options));
}
task<bool> http_server_group::close_connection(worker_ref caller, std::uint64_t id) & {
    const auto state = state_;
    return state->close_connection(std::move(caller), id);
}
void http_server_group::close() & noexcept {
    state_->close();
}
task<void> http_server_group::shutdown(worker_ref caller) & {
    const auto state = state_;
    return state->shutdown(std::move(caller));
}
http_server_stats http_server_group::stats() const {
    return state_->stats();
}
std::size_t http_server_group::worker_count() const noexcept {
    return state_->worker_count();
}
const worker_ref& http_server_group::worker(std::size_t index) const& {
    return state_->worker(index);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

int quic_resumption_context_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int generate_quic_ticket(SSL* ssl, void*) noexcept {
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(
        SSL_get_ex_data(ssl, quic_resumption_context_index()));
    return context != nullptr &&
           SSL_SESSION_set1_ticket_appdata(SSL_get0_session(ssl), context->data(), context->size()) == 1;
}

SSL_TICKET_RETURN decrypt_quic_ticket(SSL* ssl, SSL_SESSION* ticket,
    const unsigned char*, std::size_t, SSL_TICKET_STATUS status, void*) noexcept {
    if (status != SSL_TICKET_SUCCESS && status != SSL_TICKET_SUCCESS_RENEW) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(
        SSL_get_ex_data(ssl, quic_resumption_context_index()));
    void* remembered = nullptr;
    std::size_t remembered_size = 0;
    // RFC 9001 §4.6.1 and RFC 9114 §7.2.8: changed transport limits or
    // SETTINGS require a fresh handshake, not acceptance of stale 0-RTT.
    if (context == nullptr || SSL_SESSION_get0_ticket_appdata(ticket, &remembered, &remembered_size) != 1 ||
        !constant_time_bytes_equal(
            std::string_view(static_cast<const char*>(remembered), remembered_size),
            std::string_view(reinterpret_cast<const char*>(context->data()), context->size()))) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    return status == SSL_TICKET_SUCCESS ? SSL_TICKET_RETURN_USE : SSL_TICKET_RETURN_USE_RENEW;
}

int client_hello_observation_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int local_address_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

bool select_handshake_certificate(SSL* ssl, http_server_session_config& session,
    std::string_view server_name, int* alert) noexcept;

// OpenSSL reports each inbound handshake message once it is reassembled and
// before it processes it, so a ClientHello reaches the observer before the
// client hello, server_name and certificate callbacks run.
void deliver_client_hello(int writing, int content_type, const void* bytes, std::size_t size, SSL* ssl) noexcept {
    // msg_type and the 3-byte length (RFC 9846 §4) precede every body.
    if (writing != 0 || content_type != SSL3_RT_HANDSHAKE || size < 4 ||
        *static_cast<const unsigned char*>(bytes) != SSL3_MT_CLIENT_HELLO) {
        return;
    }
    const auto* observation = static_cast<const tls_client_hello_observation*>(
        SSL_get_ex_data(ssl, client_hello_observation_index()));
    if (observation == nullptr || !observation->callback_) {
        return;
    }
    auto fields = observation->fields_;
    fields.message_ = std::span(static_cast<const std::byte*>(bytes), size);
    observation->callback_(server_observation_access::tls_client_hello(fields));
}

// TLS over TCP keeps the protocol policy's outgoing record count.
void observe_tcp_messages(int writing, int version, int content_type, const void* bytes, std::size_t size,
    SSL* ssl, void* argument) noexcept {
    count_tls_records(writing, version, content_type, bytes, size, ssl, argument);
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

// QUIC protects its own packets (RFC 9001 §4): there are no TLS records to count.
void observe_quic_messages(int writing, int, int content_type, const void* bytes, std::size_t size, SSL* ssl,
    void*) noexcept {
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

int select_tcp_alpn(SSL* ssl, std::span<const unsigned char> protocols,
    const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length) noexcept {
    if (SSL_version(ssl) >= TLS1_2_VERSION) {
        return SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols.data(),
                   static_cast<unsigned int>(protocols.size()), in, in_length) == OPENSSL_NPN_NEGOTIATED
                   ? SSL_TLSEXT_ERR_OK
                   : SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    // RFC 9113 §9.2: h2 cannot be selected on TLS 1.1, even when a raw
    // connection listener explicitly lists it among its own protocols.
    for (std::size_t offset = 0; offset < protocols.size();) {
        const auto* protocol = protocols.data() + offset;
        const auto length = protocol[0];
        offset += static_cast<std::size_t>(length) + 1;
        if (length == 2 && protocol[1] == 'h' && protocol[2] == '2') {
            continue;
        }
        if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocol,
                static_cast<unsigned int>(length) + 1, in, in_length) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// Every HTTP version this server speaks over TLS on TCP, in preference order;
// RFC 7301 §3.2 reserves the alert for offers naming none of them.
int select_tcp_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1', 8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
    return select_tcp_alpn(ssl, protocols, out, out_length, in, in_length);
}

// RFC 9114 §3.1: an HTTP/3 connection negotiates "h3", and RFC 9001 §8.1
// requires the handshake to fail when ALPN agrees on nothing.
int select_quic_alpn_protocol(SSL*, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {2, 'h', '3'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// A connection-dispatch listener offers its own protocols (tls_config::alpn_),
// still in server preference and failing an offer that names none of them.
int select_connection_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void* argument) noexcept {
    const auto& protocols = *static_cast<const std::string*>(argument);
    return select_tcp_alpn(ssl,
        {reinterpret_cast<const unsigned char*>(protocols.data()), protocols.size()},
        out, out_length, in, in_length);
}

// RFC 6066 §3: a session never resumes under a server_name other than the one
// it was established with. Each connection's session ID context names its
// offered server_name (ASCII case-insensitively), and OpenSSL resumes a
// session -- TLS 1.2 by ticket or session ID, TLS 1.3 by PSK -- only when the
// contexts match, so any other name, or none, gets a full handshake (RFC 9846
// §4.6.1 permits declining such a PSK). Names longer than a context are bound
// by their SHA-256 digest.
int bind_session_to_server_name(SSL* ssl, int* alert, void* argument) noexcept {
    // Without server_name: one NUL byte, which no host name contains.
    std::array<unsigned char, SSL_MAX_SID_CTX_LENGTH> context{};
    std::size_t context_size = 1;
    const unsigned char* extension = nullptr;
    std::size_t extension_size = 0;
    std::string_view server_name;
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &extension_size) == 1) {
        // One non-empty host_name entry: any other ServerNameList shape fails
        // the handshake before OpenSSL could resume a session under it.
        if (extension_size < 5 || static_cast<std::size_t>((extension[0] << 8) | extension[1]) != extension_size - 2 ||
            extension[2] != TLSEXT_NAMETYPE_host_name) {
            *alert = SSL_AD_DECODE_ERROR;
            return SSL_CLIENT_HELLO_ERROR;
        }
        const auto name_size = static_cast<std::size_t>((extension[3] << 8) | extension[4]);
        if (name_size == 0 || name_size != extension_size - 5 || name_size > TLSEXT_MAXLEN_host_name) {
            *alert = SSL_AD_DECODE_ERROR;
            return SSL_CLIENT_HELLO_ERROR;
        }
        server_name = std::string_view(reinterpret_cast<const char*>(extension + 5), name_size);
        if (name_size <= context.size()) {
            for (std::size_t i = 0; i < name_size; ++i) {
                context[i] = http_ascii_to_lower(extension[5 + i]);
            }
            context_size = name_size;
        } else {
            std::array<unsigned char, TLSEXT_MAXLEN_host_name> name;
            for (std::size_t i = 0; i < name_size; ++i) {
                name[i] = http_ascii_to_lower(extension[5 + i]);
            }
            unsigned int digest_size = 0;
            if (EVP_Digest(name.data(), name_size, context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
                digest_size != context.size()) {
                *alert = SSL_AD_INTERNAL_ERROR;
                return SSL_CLIENT_HELLO_ERROR;
            }
            context_size = digest_size;
        }
    }
    if (SSL_set_session_id_context(ssl, context.data(), static_cast<unsigned int>(context_size)) != 1) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_CLIENT_HELLO_ERROR;
    }
    if (!select_handshake_certificate(ssl, *static_cast<http_server_session_config*>(argument), server_name, alert)) {
        return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

std::span<const std::byte> client_hello_list(SSL* ssl, unsigned int type, std::size_t prefix) {
    const unsigned char* bytes = nullptr;
    std::size_t size = 0;
    if (SSL_client_hello_get0_ext(ssl, type, &bytes, &size) != 1) {
        return {};
    }
    if (size < prefix) {
        throw std::invalid_argument("malformed TLS selection extension");
    }
    const auto length = prefix == 1 ? static_cast<std::size_t>(bytes[0])
                                    : static_cast<std::size_t>((bytes[0] << 8) | bytes[1]);
    if (length != size - prefix) {
        throw std::invalid_argument("malformed TLS selection extension length");
    }
    return {reinterpret_cast<const std::byte*>(bytes + prefix), length};
}

std::vector<std::uint16_t> decode_u16_list(
    std::span<const std::byte> raw) {
    if (raw.size() % 2U != 0U) {
        throw std::invalid_argument("malformed TLS uint16 extension list");
    }
    std::vector<std::uint16_t> decoded;
    decoded.reserve(raw.size() / 2U);
    for (std::size_t i = 0; i < raw.size(); i += 2U) {
        const auto hi = static_cast<std::uint16_t>(std::to_integer<unsigned int>(raw[i]));
        const auto lo = static_cast<std::uint16_t>(std::to_integer<unsigned int>(raw[i + 1U]));
        decoded.push_back(static_cast<std::uint16_t>((hi << 8U) | lo));
    }
    return decoded;
}

std::vector<std::string_view> decode_alpn_list(
    std::span<const std::byte> raw) {
    std::vector<std::string_view> protocols;
    for (std::size_t pos = 0; pos < raw.size();) {
        const auto length = static_cast<std::size_t>(std::to_integer<unsigned int>(raw[pos]));
        if (length == 0U || pos + 1U + length > raw.size()) {
            throw std::invalid_argument("malformed TLS ALPN extension list");
        }
        protocols.emplace_back(
            reinterpret_cast<const char*>(raw.data() + pos + 1U), length);
        pos += 1U + length;
    }
    return protocols;
}

// ClientHello precedes OpenSSL's status_request parsing. Give this SSL its
// own requested staple so replacement cannot invalidate its selected pair.
bool present_fixed_identity(SSL& ssl, const http_server_tls_identities::identity& identity) noexcept {
    if (!identity.certificate_.present(ssl)) {
        return false;
    }
    const unsigned char* extension = nullptr;
    std::size_t size = 0;
    if (identity.ocsp_response_.empty() ||
        SSL_client_hello_get0_ext(&ssl, TLSEXT_TYPE_status_request, &extension, &size) != 1 ||
        size == 0 || extension[0] != TLSEXT_STATUSTYPE_ocsp) {
        return true;
    }
    auto* response = static_cast<unsigned char*>(OPENSSL_memdup(
        identity.ocsp_response_.data(), identity.ocsp_response_.size()));
    if (response == nullptr) {
        return false;
    }
    if (SSL_set_tlsext_status_ocsp_resp(&ssl, response, static_cast<int>(identity.ocsp_response_.size())) != 1) {
        OPENSSL_free(response);
        return false;
    }
    return true;
}

int staple_ocsp_response(SSL* ssl, void*) noexcept {
    const unsigned char* response = nullptr;
    return SSL_get_tlsext_status_ocsp_resp(ssl, &response) > 0
               ? SSL_TLSEXT_ERR_OK
               : SSL_TLSEXT_ERR_NOACK;
}

// Runs before session lookup, so neither resumption nor missing SNI bypasses a
// selector rejection. A HelloRetryRequest retains the first selected identity.
bool select_handshake_certificate(SSL* ssl, http_server_session_config& session,
    std::string_view server_name, int* alert) noexcept {
    if (SSL_get_certificate(ssl) != nullptr) {
        return true;
    }
    const auto& identities = *session.tls()->identities_;
    const auto* local = static_cast<const asio::ip::address*>(SSL_get_ex_data(ssl, local_address_index()));
    try {
        if (!identities.certificate_store_) {
            if (present_fixed_identity(*ssl, identities.select(server_name, local))) {
                return true;
            }
        } else {
            // Capture before calling user code: even a concurrent replacement
            // or a mutation made inside the callback cannot change this choice.
            const auto snapshot = tls_certificate_store_access::snapshot(*identities.certificate_store_);
            std::optional<tls_certificate_id> selected = identities.default_certificate_;
            if (identities.select_certificate_) {
                const auto* binding = static_cast<const tls_client_hello_observation*>(
                    SSL_get_ex_data(ssl, client_hello_observation_index()));
                if (!binding || binding->fields_.connection_id_ == 0 || local == nullptr) {
                    throw std::logic_error("TLS certificate selector requires connection metadata");
                }
                const unsigned char* ciphers = nullptr;
                const auto cipher_size = SSL_client_hello_get0_ciphers(ssl, &ciphers);
                const auto cipher_suites = decode_u16_list(
                    {reinterpret_cast<const std::byte*>(ciphers), cipher_size});
                const auto signature_algorithms = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_signature_algorithms, 2));
                const auto supported_versions = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_supported_versions, 1));
                const auto supported_groups = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_supported_groups, 2));
                const auto alpn = decode_alpn_list(
                    client_hello_list(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, 2));
                const tls_certificate_selection input{
                    .connection_id_ = binding->fields_.connection_id_,
                    .remote_address_ = ip_address::parse(binding->fields_.remote_address_).value_or(ip_address{}),
                    .remote_port_ = binding->fields_.remote_port_,
                    .server_name_ = server_name,
                    .local_address_ = local ? std::optional(to_ip_address(*local)) : std::nullopt,
                    .local_port_ = binding->fields_.local_port_,
                    .quic_ = binding->fields_.quic_,
                    .legacy_version_ = static_cast<std::uint16_t>(SSL_client_hello_get0_legacy_version(ssl)),
                    .cipher_suites_ = cipher_suites,
                    .signature_algorithms_ = signature_algorithms,
                    .supported_versions_ = supported_versions,
                    .supported_groups_ = supported_groups,
                    .alpn_ = alpn};
                selected = (*identities.select_certificate_)(input);
            }
            if (selected) {
                if (const auto* cert = snapshot->find(*selected); cert != nullptr && cert->present(*ssl)) {
                    return true;
                }
            }
        }
    } catch (...) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return false;
    }
    *alert = SSL_AD_ACCESS_DENIED;
    return false;
}

int acknowledge_server_name(SSL*, int*, void*) noexcept {
    return SSL_TLSEXT_ERR_OK;
}

enum class tls_context_family : unsigned char {
    tcp,
    quic,
};

void configure_family(http_server_session_config& session, tls_context_family family, bool observe_client_hello) {
    const auto* tls = session.tls();
    const bool quic = family == tls_context_family::quic;
    const bool early_data = quic && session.http3_->early_data_;
    auto& context_value = (quic ? session.quic_tls_context_ : session.tls_context_).emplace(asio::ssl::context::tls_server);
    auto* native = context_value.native_handle();
    if (quic) {
        // RFC 9001 §4.2: QUIC requires TLS 1.3.
        if (SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION) != 1) {
            throw std::runtime_error("failed to restrict the QUIC TLS context to TLS 1.3");
        }
        SSL_CTX_set_alpn_select_cb(native, select_quic_alpn_protocol, nullptr);
        if (SSL_CTX_set_max_early_data(native, early_data ? UINT32_MAX : 0) != 1 ||
            SSL_CTX_set_session_ticket_cb(native, generate_quic_ticket, decrypt_quic_ticket, nullptr) != 1) {
            throw std::runtime_error("failed to configure QUIC session tickets");
        }
        if (early_data) {
            // Ticket keys are shared across workers, not OpenSSL's local
            // replay cache. RFC 8470 admission rejects unsafe early requests.
            SSL_CTX_set_options(native, SSL_OP_NO_ANTI_REPLAY);
        }
    } else {
        context_value.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                                  asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                                  asio::ssl::context::single_dh_use);
        apply_tls_protocol_policy(*native, tls->max_outgoing_records_);
        if (session.connection_.has_value()) {
            SSL_CTX_set_alpn_select_cb(native, select_connection_alpn_protocol, &session.connection_->alpn_);
        } else {
            SSL_CTX_set_alpn_select_cb(native, select_tcp_alpn_protocol, nullptr);
        }
    }
    if (observe_client_hello) {
        SSL_CTX_set_msg_callback(native, quic ? observe_quic_messages : observe_tcp_messages);
    }
    // RFC 9325 §4.2.1: the server's order decides, whatever the client prefers.
    SSL_CTX_set_options(native, SSL_OP_CIPHER_SERVER_PREFERENCE);
    configure_http_server_client_certificates(*native, tls->client_certificates_);
    session.ticket_keys_->install(*native);
    // The client hello callback runs before OpenSSL looks up the offered
    // session, which the server_name callback would be too late for.
    SSL_CTX_set_client_hello_cb(native, &bind_session_to_server_name, &session);
    // The session outlives every SSL of its contexts, which its worker joins
    // before retiring them.
    SSL_CTX_set_tlsext_servername_callback(native, &acknowledge_server_name);
    SSL_CTX_set_tlsext_servername_arg(native, &session);
    SSL_CTX_set_tlsext_status_cb(native, &staple_ocsp_response);
}

}  // namespace

#if defined(RUVIA_ENABLE_HTTP3)
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    std::uint32_t version, const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context) {
    // Only remembered limits (RFC 9000 §7.4.1): connection IDs and reset
    // tokens change for each connection and must not prevent legitimate
    // ticket resumption.
    std::array<std::byte, 256> parameters{};
    const auto parameter_bytes = connection.encode_early_transport_parameters(parameters);
    if (!parameter_bytes.has_value()) {
        throw std::runtime_error("failed to encode QUIC early transport parameters");
    }
    const std::array<std::uint64_t, 7> values{version, options.max_field_section_size_,
        options.qpack_max_dtable_capacity_, options.qpack_blocked_streams_,
        static_cast<std::uint64_t>(options.enable_connect_protocol_),
        static_cast<std::uint64_t>(options.enable_h3_datagram_), static_cast<std::uint64_t>(early_data)};
    std::array<unsigned char, values.size() * sizeof(std::uint64_t) + parameters.size()> encoded{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            encoded[i * sizeof(std::uint64_t) + byte] =
                static_cast<unsigned char>(values[i] >> (8 * (sizeof(std::uint64_t) - byte - 1)));
        }
    }
    const auto encoded_size = values.size() * sizeof(std::uint64_t) + *parameter_bytes;
    std::memcpy(encoded.data() + values.size() * sizeof(std::uint64_t), parameters.data(), *parameter_bytes);
    unsigned int digest_size = 0;
    const auto index = quic_resumption_context_index();
    if (index < 0 ||
        EVP_Digest(encoded.data(), encoded_size, resumption_context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != resumption_context.size() ||
        SSL_set_ex_data(&tls, index, &resumption_context) != 1 ||
        SSL_set_quic_tls_early_data_enabled(&tls, early_data ? 1 : 0) != 1) {
        throw std::runtime_error("failed to bind QUIC TLS resumption settings");
    }
}
#endif

void configure_http_server_tls_contexts(http_server_session_config& session, bool observe_client_hello) {
    session.tls_context_.reset();
    session.quic_tls_context_.reset();
    session.ticket_keys_.reset();
    const auto* tls = session.tls();
    if (tls == nullptr) {
        return;
    }
    auto& ticket_keys = session.ticket_keys_.emplace();
    if (!tls_session_ticket_keys(*tls).copy_to(std::chrono::steady_clock::now(), ticket_keys)) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
    configure_family(session, tls_context_family::tcp, observe_client_hello);
    if (session.http3_.has_value()) {
        configure_family(session, tls_context_family::quic, observe_client_hello);
    }
}

prepared_http_server_session prepare_http_server_session_config(
    const http_server_listener_definition& definition, bool observe_client_hello) {
    auto session = std::make_unique<http_server_session_config>(definition);
    configure_http_server_tls_contexts(*session, observe_client_hello);
    return session;
}

void bind_tls_local_address(SSL& tls, const asio::ip::address& local_address) {
    const auto index = local_address_index();
    // OpenSSL's ex_data stores a mutable pointer; selection only reads it.
    if (index < 0 || SSL_set_ex_data(&tls, index, const_cast<asio::ip::address*>(&local_address)) != 1) {
        throw std::runtime_error("failed to attach the TLS connection's local address");
    }
}

bool tls_certificate_selection_enabled(const http_server_session_config& session) noexcept {
    const auto* tls = session.tls();
    return tls != nullptr && static_cast<bool>(tls->identities_->select_certificate_);
}

void observe_tls_client_hello(SSL& tls, const tls_client_hello_observation& observation) {
    const auto index = client_hello_observation_index();
    // OpenSSL's ex_data stores a mutable pointer; the observer only reads it.
    if (index < 0 || SSL_set_ex_data(&tls, index, const_cast<tls_client_hello_observation*>(&observation)) != 1) {
        throw std::runtime_error("failed to attach the TLS ClientHello observer");
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// In-memory PEM text of a tls_identity_config, borrowed while it is parsed.
struct tls_identity_pem final {
    std::string_view certificate_chain_;
    std::string_view private_key_;
};

void validate_ocsp_response(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("TLS OCSP response is too large");
    }
    ERR_clear_error();
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto* const end = cursor + bytes.size();
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> response(
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
    if (!response) {
        const auto error = ERR_peek_last_error();
        ERR_clear_error();
        if (ERR_GET_REASON(error) == ERR_R_MALLOC_FAILURE) {
            throw std::bad_alloc();
        }
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    const auto size = i2d_OCSP_RESPONSE(response.get(), nullptr);
    if (cursor != end || size <= 0 || static_cast<std::size_t>(size) != bytes.size()) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    // d2i also accepts BER; require the canonical DER encoding we advertise.
    std::vector<std::byte> canonical(bytes.size());
    auto* output = reinterpret_cast<unsigned char*>(canonical.data());
    if (i2d_OCSP_RESPONSE(response.get(), &output) != size ||
        !std::equal(bytes.begin(), bytes.end(), canonical.begin())) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
}

// One identity's validated source and the password its key needs.
struct tls_identity_source final {
    std::variant<tls_identity_files, tls_identity_pem> location_;
    std::string_view private_key_password_;
    std::span<const std::byte> ocsp_response_;
};

[[nodiscard]] tls_identity_source make_tls_identity_source(
    const tls_identity_config& identity,
    std::span<const std::byte> ocsp_response = {}) {
    validate_ocsp_response(ocsp_response);
    const bool files = !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty();
    const bool pem = !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty();
    if (files && pem) {
        throw std::invalid_argument("TLS identity must come from PEM files or in-memory PEM, not both");
    }
    if (!pem) {
        // Both names are required: an empty one throws.
        return {.location_ = make_tls_identity_files(identity),
            .private_key_password_ = identity.private_key_password_,
            .ocsp_response_ = ocsp_response};
    }
    if (identity.certificate_chain_pem_.empty() || identity.private_key_pem_.empty()) {
        throw std::invalid_argument("in-memory TLS identity requires both certificate chain and private key PEM");
    }
    return {.location_ = tls_identity_pem{.certificate_chain_ = identity.certificate_chain_pem_,
                .private_key_ = identity.private_key_pem_},
        .private_key_password_ = identity.private_key_password_,
        .ocsp_response_ = ocsp_response};
}

[[nodiscard]] tls_certificate load_tls_identity(const tls_identity_source& source) {
    if (const auto* files = std::get_if<tls_identity_files>(&source.location_)) {
        return load_tls_certificate(*files, source.private_key_password_);
    }
    const auto& pem = std::get<tls_identity_pem>(source.location_);
    return tls_certificate::load_pem(pem.certificate_chain_, pem.private_key_, source.private_key_password_);
}

[[nodiscard]] http_server_tls_identities::identity load_fixed_tls_identity(
    const tls_identity_source& source) {
    return {load_tls_identity(source), source.ocsp_response_};
}

// RFC 6066 §3 HostName: an ASCII DNS name, without the trailing dot.
[[nodiscard]] bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

// A connection reaching an IPv4 address through a dual-stack socket reports
// its IPv4-mapped IPv6 form; both name the same local address.
[[nodiscard]] asio::ip::address canonical_local_address(const asio::ip::address& address) noexcept {
    if (address.is_v6()) {
        if (const auto v6 = address.to_v6(); v6.is_v4_mapped()) {
            return asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
        }
    }
    return address;
}

[[nodiscard]] asio::ip::address make_tls_local_address(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("TLS local address must not be empty");
    }
    const auto parsed = ruvia::parse_ip_address(text);
    if (!parsed) {
        throw std::invalid_argument("TLS local address must be a numeric IP address");
    }
    const auto address = canonical_local_address(*parsed);
    // No connection arrives at an unspecified address.
    if (address.is_unspecified()) {
        throw std::invalid_argument("TLS local address must not be unspecified");
    }
    return address;
}

// Every identity setting of one tls_config, validated before anything is read.
class tls_identity_plan final {
public:
    tls_identity_plan(const tls_config& config);

    [[nodiscard]] std::shared_ptr<const http_server_tls_identities> load() const;

private:
    struct named_source final {
        // ASCII lowercase; a wildcard keeps the parent domain after its "*.".
        std::string name_;
        bool wildcard_;
        tls_identity_source source_;
    };

    struct addressed_source final {
        asio::ip::address address_;
        tls_identity_source source_;
    };

    std::optional<tls_identity_source> primary_;
    std::shared_ptr<tls_certificate_store> store_;
    std::shared_ptr<const tls_certificate_selector> selector_;
    tls_certificate_id default_certificate_{};
    std::vector<named_source> names_;
    std::vector<addressed_source> addresses_;
};

tls_identity_plan::tls_identity_plan(const tls_config& config) {
    if (config.certificate_store_) {
        const auto& identity = config.identity_;
        if (!identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() ||
            !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() ||
            !identity.private_key_password_.empty() || !config.ocsp_response_.empty() ||
            !config.sni_.empty() || !config.local_addresses_.empty()) {
            throw std::invalid_argument("TLS certificate library cannot be combined with fixed identities");
        }
        if (!config.select_certificate_ && config.default_certificate_ == 0) {
            throw std::invalid_argument("TLS certificate library requires a selector or default certificate ID");
        }
        store_ = config.certificate_store_;
        selector_ = config.select_certificate_;
        default_certificate_ = config.default_certificate_;
        return;
    }
    if (config.select_certificate_ || config.default_certificate_ != 0) {
        throw std::invalid_argument("TLS certificate selection requires a certificate library");
    }
    primary_.emplace(make_tls_identity_source(config.identity_, config.ocsp_response_));
    names_.reserve(config.sni_.size());
    for (const auto& sni : config.sni_) {
        if (sni.host_.empty()) {
            throw std::invalid_argument("SNI host must not be empty");
        }
        // RFC 6125 §6.4.3: a wildcard is the whole left-most label.
        const bool wildcard = sni.host_.starts_with("*.");
        const auto host = std::string_view(sni.host_).substr(wildcard ? 2 : 0);
        if (!is_valid_sni_host(host)) {
            throw std::invalid_argument("SNI host is invalid");
        }
        std::string name(host);
        for (auto& byte : name) {
            byte = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(byte)));
        }
        for (const auto& existing : names_) {
            if (existing.wildcard_ == wildcard && existing.name_ == name) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
        names_.push_back(named_source{
            .name_ = std::move(name),
            .wildcard_ = wildcard,
            .source_ = make_tls_identity_source(sni.identity_, sni.ocsp_response_),
        });
    }
    addresses_.reserve(config.local_addresses_.size());
    for (const auto& local : config.local_addresses_) {
        const auto address = make_tls_local_address(local.address_);
        for (const auto& existing : addresses_) {
            if (existing.address_ == address) {
                throw std::invalid_argument("TLS local addresses must be unique");
            }
        }
        addresses_.push_back(addressed_source{
            .address_ = address,
            .source_ = make_tls_identity_source(local.identity_, local.ocsp_response_),
        });
    }
}

std::shared_ptr<const http_server_tls_identities> tls_identity_plan::load() const {
    if (store_) {
        auto identities = std::make_shared<http_server_tls_identities>();
        identities->certificate_store_ = store_;
        identities->select_certificate_ = selector_;
        identities->default_certificate_ = default_certificate_;
        return identities;
    }
    auto identities = std::make_shared<http_server_tls_identities>(load_fixed_tls_identity(*primary_));
    for (const auto& named : names_) {
        auto& target = named.wildcard_ ? identities->wildcard_names_ : identities->exact_names_;
        target.emplace_back(named.name_, load_fixed_tls_identity(named.source_));
    }
    identities->local_addresses_.reserve(addresses_.size());
    for (const auto& addressed : addresses_) {
        identities->local_addresses_.push_back(http_server_tls_identities::addressed_identity{
            .address_ = addressed.address_,
            .value_ = load_fixed_tls_identity(addressed.source_),
        });
    }
    return identities;
}

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(
    tls_client_certificate_requirement requirement) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requirement == tls_client_certificate_requirement::required) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

}  // namespace

tls_certificate load_server_tls_certificate(const tls_identity_config& identity) {
    return load_tls_identity(make_tls_identity_source(identity));
}

const http_server_tls_identities::identity& http_server_tls_identities::select(
    std::string_view server_name, const asio::ip::address* local_address) const noexcept {
    if (!server_name.empty()) {
        for (const auto& named : exact_names_) {
            if (http_ascii_equals_ignore_case(named.name_, server_name)) {
                return named.value_;
            }
        }
        // RFC 6125 §6.4.3: "*" stands for exactly one non-empty label.
        if (const auto dot = server_name.find('.'); dot != std::string_view::npos && dot != 0) {
            const auto parent = server_name.substr(dot + 1);
            for (const auto& named : wildcard_names_) {
                if (http_ascii_equals_ignore_case(named.name_, parent)) {
                    return named.value_;
                }
            }
        }
    }
    if (local_address != nullptr && !local_addresses_.empty()) {
        const auto address = canonical_local_address(*local_address);
        for (const auto& addressed : local_addresses_) {
            if (addressed.address_ == address) {
                return addressed.value_;
            }
        }
    }
    return *primary_;
}

http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    // Every setting is validated before any credential is read.
    const tls_identity_plan identities(config);
    const auto& client = config.client_certificates_;
    std::optional<tls_file_name> client_ca;
    if (client.ca_file_.has_value() || client.requirement_ != tls_client_certificate_requirement::optional) {
        switch (client.requirement_) {
            case tls_client_certificate_requirement::optional:
            case tls_client_certificate_requirement::required:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (!client.ca_file_.has_value()) {
            throw std::invalid_argument("TLS client certificates require a CA bundle");
        }
        client_ca.emplace(make_tls_file_name(*client.ca_file_));
    }

    http_server_listener_definition::tls tls(identities.load());
    tls.max_outgoing_records_ = config.max_outgoing_records_;
    if (client_ca.has_value()) {
        auto trust_store = load_tls_trust_store(*client_ca);
        tls.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = client.requirement_,
            .client_ca_ = std::move(trust_store),
            .ca_file_ = std::move(*client_ca),
        });
    }
    return tls;
}

std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config) {
    return tls_identity_plan(config).load();
}

void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config) {
    if (config.max_outgoing_records_ != current.max_outgoing_records_) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's outgoing record limit");
    }
    const auto& client = config.client_certificates_;
    const auto& policy = current.client_certificates_;
    const bool same = policy.has_value()
                          ? client.requirement_ == policy->requirement_ && client.ca_file_.has_value() &&
                                make_tls_file_name(*client.ca_file_) == policy->ca_file_
                          : client.requirement_ == tls_client_certificate_requirement::optional &&
                                !client.ca_file_.has_value();
    if (!same) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's client certificate policy");
    }
}

void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates) {
    if (!client_certificates.has_value()) {
        return;
    }
    client_certificates->client_ca_.install(context_value);
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(&context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_credential_error("set_session_id_context");
    }
    SSL_CTX_set_verify(&context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// The identity set is immutable and shared, not copied.
http_server_listener_definition::tls clone_tls(
    const http_server_listener_definition::tls& source_value) {
    http_server_listener_definition::tls result_value(source_value.identities_);
    if (const auto& policy = source_value.client_certificates_; policy.has_value()) {
        result_value.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = policy->requirement_,
            .client_ca_ = policy->client_ca_,
            .ca_file_ = tls_file_name(policy->ca_file_),
        });
    }
    result_value.max_outgoing_records_ = source_value.max_outgoing_records_;
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    result_value.tcp_mode_ = source_value.tcp_mode_;
    result_value.plaintext_redirect_ = source_value.plaintext_redirect_;
    return result_value;
}

http_server_listener_definition::transport_type clone_transport(
    const http_server_listener_definition::transport_type& source_value) {
    return std::visit(
        []<typename transport_type>(
            const transport_type& transport) -> http_server_listener_definition::transport_type {
            if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls>) {
                return clone_tls(transport);
            } else {
                return transport;
            }
        },
        source_value);
}

std::optional<http_server_listener_definition::connection_dispatch> clone_connection_dispatch(
    const std::optional<http_server_listener_definition::connection_dispatch>& source_value) {
    std::optional<http_server_listener_definition::connection_dispatch> result_value;
    if (source_value.has_value()) {
        result_value.emplace(source_value->alpn_);
    }
    return result_value;
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone() const {
    http_server_listener_definition result_value(
        endpoint_, clone_transport(transport_), http3_);
    result_value.connection_ = clone_connection_dispatch(connection_);
    result_value.inherited_sockets_ = inherited_sockets_;
    result_value.ipv6_only_ = ipv6_only_;
    result_value.proxy_protocol_ = proxy_protocol_;
    result_value.request_stream_ = request_stream_;
    return result_value;
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition)
    : transport_(clone_transport(definition.transport_)),
      ipv6_only_(definition.ipv6_only_),
      proxy_protocol_(definition.proxy_protocol_),
      http3_(definition.http3_),
      connection_(clone_connection_dispatch(definition.connection_)),
      request_stream_(definition.request_stream_) {
    if (http3_.has_value()) {
        // RFC 7838 §3: alt-authority with an empty host names this origin's
        // host on the UDP port; "ma" bounds how long clients cache it.
        alternative_service_.append("h3=\":");
        alternative_service_.append(std::to_string(http3_->port_));
        alternative_service_.append("\"; ma=");
        alternative_service_.append(std::to_string(http3_->alt_svc_max_age_.count()));
    }
}

void http_server_session_config::adopt_tls_identities(
    std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&transport_)) {
        tls->identities_.swap(identities);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

#if defined(_WIN32)
using socket_length = int;

[[nodiscard]] SOCKET os_socket(native_socket socket) noexcept {
    return static_cast<SOCKET>(socket);
}
#else
using socket_length = socklen_t;

[[nodiscard]] int os_socket(native_socket socket) noexcept {
    return socket;
}
#endif

// The integer SOL_SOCKET option `name`; nullopt when `socket` is no socket.
[[nodiscard]] std::optional<int> socket_option(native_socket socket, int name) noexcept {
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (::getsockopt(os_socket(socket), SOL_SOCKET, name, reinterpret_cast<char*>(&value), &length) != 0) {
        return std::nullopt;
    }
    return value;
}

// Whether `socket` is bound to exactly `endpoint`, in its address family.
template <typename endpoint_type>
[[nodiscard]] bool bound_to(native_socket socket, const endpoint_type& endpoint) {
    endpoint_type local;
    auto length = static_cast<socket_length>(local.capacity());
    if (::getsockname(os_socket(socket), local.data(), &length) != 0 || length <= 0 ||
        static_cast<std::size_t>(length) > local.capacity() || local.data()->sa_family != endpoint.data()->sa_family) {
        return false;
    }
    local.resize(static_cast<std::size_t>(length));
    return local == endpoint;
}

// Inherited handles leave with an exec (or a Windows child process) only when
// the application hands them out again.
void mark_close_on_exec(native_socket socket) {
#if defined(_WIN32)
    if (::SetHandleInformation(reinterpret_cast<HANDLE>(os_socket(socket)), HANDLE_FLAG_INHERIT, 0) == 0) {
        throw std::system_error(static_cast<int>(::GetLastError()), asio::system_category(),
            "failed to make an inherited listener socket non-inheritable");
    }
#else
    const int flags = ::fcntl(socket, F_GETFD);
    if (flags == -1 || ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to mark an inherited listener socket close-on-exec");
    }
#endif
}

void ensure_socket_kind(native_socket socket, int type, [[maybe_unused]] int protocol, const char* message) {
    if (socket_option(socket, SO_TYPE) != type) {
        throw std::invalid_argument(message);
    }
#if defined(SO_PROTOCOL)
    if (socket_option(socket, SO_PROTOCOL) != protocol) {
        throw std::invalid_argument(message);
    }
#endif
}

void ensure_ipv6_only(native_socket socket, bool ipv6, std::optional<bool> expected) {
    if (!expected) {
        return;
    }
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (!ipv6 || ::getsockopt(os_socket(socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&value), &length) != 0 || (value != 0) != *expected) {
        throw std::invalid_argument("inherited listener IPV6_V6ONLY does not match its configuration");
    }
}

}  // namespace

void close_native_socket(native_socket socket) noexcept {
#if defined(_WIN32)
    if (socket != invalid_native_socket) {
        (void)::closesocket(os_socket(socket));
    }
#else
    if (socket >= 0) {
        (void)::close(socket);
    }
#endif
}

void adopt_inherited_tcp_socket(native_socket socket, const asio::ip::tcp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_STREAM, IPPROTO_TCP, message);
    if (socket_option(socket, SO_ACCEPTCONN).value_or(0) == 0 || !bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

void adopt_inherited_udp_socket(native_socket socket, const asio::ip::udp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_DGRAM, IPPROTO_UDP, message);
    if (!bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

#if !defined(_WIN32)
native_socket duplicate_native_socket(native_socket socket) {
    const int duplicate = ::dup(socket);
    if (duplicate < 0) {
        throw std::system_error(errno, asio::system_category(), "failed to duplicate a listener socket");
    }
    return duplicate;
}
#endif

inherited_listener_sockets::~inherited_listener_sockets() {
    for (const auto socket : tcp_) {
        close_native_socket(socket);
    }
    for (const auto socket : udp_) {
        close_native_socket(socket);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// Only the connection's own transport can end it normally. A handler or
// upstream failure is a real fault even when it carries one of the codes
// is_inbound_connection_end() accepts: an upstream request that timed out or
// was reset says nothing about the inbound peer.
[[nodiscard]] bool is_connection_end(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const inbound_transport_error& error) {
        return is_inbound_connection_end(error.code());
    } catch (...) {
        return false;
    }
}

}  // namespace

void connection_failure_sink::invoke(ip_address remote_address, std::exception_ptr exception) const noexcept {
    if (exception == nullptr || is_connection_end(exception)) {
        return;
    }
    if (counter_ != nullptr) {
        counter_->fetch_add(1, std::memory_order_relaxed);
    }
    if (callback_) {
        callback_(connection_failure_record_access::make(remote_address, std::move(exception)));
        return;
    }
    ruvia::report_unhandled_failure("web connection", exception);
}

void connection_failure_sink::invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
    invoke(ip_address::parse(remote_address).value_or(ip_address{}), std::move(exception));
}

void http_server_options::set_server(const server_config& config) {
    validate_server_limits(config);
    if (const auto& compression = config.compression_) {
        ensure_positive_size(compression->min_bytes_, "compression minimum size must be greater than zero");
        if (compression->sync_bytes_ < compression->min_bytes_) {
            throw std::invalid_argument(
                "compression synchronous size must not be smaller than the minimum size");
        }
        if (compression->max_bytes_ < compression->sync_bytes_) {
            throw std::invalid_argument(
                "compression maximum size must not be smaller than the synchronous size");
        }
    }
    std::optional<http_cors_policy> cors;
    if (config.cors_) {
        cors.emplace(*config.cors_);
    }
    server_ = config;
    compression_ = config.compression_;
    cors_ = std::move(cors);
}

void http_server_options::set_rate_limit(const rate_limit_config& config) {
    validate_rate_limit_config(config);
    default_rate_limit_per_worker_ = config.rule_;
    rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
}

void http_server_options::set_trusted_proxies(const trusted_proxy_config& config) {
    http_client_ip_config ip_config;
    ip_config.enabled_ = !config.cidrs_.empty();
    ip_config.trusted_proxies_ = config.cidrs_;
    ip_config.forwarding_headers_ = config.forwarding_headers_;
    ip_config.trust_x_forwarded_proto_ = config.trust_x_forwarded_proto_;
    trusted_proxies_ = http_client_ip_extractor(ip_config);
}

http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin) {
    const auto worker_count = options.server_.worker_count_;
    if (const auto* redirect = std::get_if<http_server_listener_definition::redirect_http_to_https>(&listener.transport_)) {
        detail::ensure_non_zero_port(redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
    if (const auto* tls = std::get_if<http_server_listener_definition::tls>(&listener.transport_)) {
        switch (tls->tcp_mode_) {
            case http_server_listener_definition::tls_tcp_mode::tls_only:
                if (tls->plaintext_redirect_) {
                    throw std::invalid_argument("a plaintext redirect requires a mixed HTTP/HTTPS listener");
                }
                break;
            case http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls:
                if (tls->plaintext_redirect_) {
                    detail::ensure_non_zero_port(
                        tls->plaintext_redirect_->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
                }
                break;
            default:
                throw std::invalid_argument("invalid TLS TCP transport mode");
        }
    }
    if (listener.connection_.has_value() && !options.connection_) {
        throw std::invalid_argument("listener_dispatch::connection requires application::on_connection()");
    }
    if (listener.request_stream_ && !options.request_stream_) {
        throw std::invalid_argument("listener_dispatch::request_stream requires application::on_request_stream()");
    }
    // A QUIC connection ID names its owning worker in 16 bits.
    if (listener.http3_.has_value() && worker_count > http3_max_worker_count) {
        throw std::invalid_argument("HTTP/3 supports at most 65536 workers");
    }
#if !defined(_WIN32)
    // Worker i serves inherited socket i; listen() admitted one per service on Windows.
    if (const auto& inherited = listener.inherited_sockets_; inherited != nullptr &&
                                                             ((!inherited->tcp().empty() && inherited->tcp().size() != worker_count) ||
                                                                 (!inherited->udp().empty() && inherited->udp().size() != worker_count))) {
        throw std::invalid_argument("inherited listener sockets must number one per worker");
    }
#endif
    auto validated = listener.clone();
    // One ticket key ring per TLS listener for this run, so TLS resumption
    // does not depend on which worker accepts the connection. Credentials
    // were loaded once by listen(); every worker installs them.
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&validated.transport_)) {
        tls->session_ticket_keys_ = std::make_shared<tls_session_ticket_key_ring>(key_origin);
    }
    // One QUIC secret per HTTP/3 service, shared by every worker so any
    // worker can tell which one owns a connection ID.
    if (validated.http3_.has_value()) {
        auto secret = std::make_shared<std::array<std::uint8_t, 32>>();
        if (!fill_random_bytes(std::span<unsigned char>(secret->data(), secret->size()))) {
            throw std::runtime_error("failed to generate the HTTP/3 connection ID secret");
        }
        validated.http3_->secret_ = std::move(secret);
    }
    return validated;
}

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(),
      options_(std::move(options)),
      // Every listener's key epochs start together, so all workers rotate them
      // at the same instants.
      tls_key_origin_(std::chrono::steady_clock::now()) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(
            validate_run_listener(listener, options_, tls_key_origin_));
    }
}

namespace {

void validate_worker_queue_capacity(std::size_t capacity) {
    detail::ensure_positive_size(capacity, "worker queue capacity must be greater than zero");
    if (capacity == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
}

}  // namespace

void validate_server_limits(const server_config& config) {
    detail::ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner &&
        config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    detail::ensure_positive_optional_durations("configured server timeouts must be greater than zero",
        config.idle_timeout_, config.request_header_timeout_, config.request_body_timeout_,
        config.write_timeout_);
    detail::ensure_positive_duration(
        config.connection_scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(config.worker_queue_capacity_);
    detail::ensure_positive_size(config.max_buffered_body_bytes_, "buffered body limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_stream_body_bytes_, "configured stream body limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_websocket_message_bytes_, "websocket message limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_connections_per_worker_, "configured connection limit must be greater than zero");
    detail::ensure_positive_optional_size(config.max_requests_per_connection_,
        "configured requests-per-connection limit must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_worker_,
        "worker inbound buffer budget must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_connection_,
        "connection inbound buffer budget must be greater than zero");
    detail::ensure_positive_optional_durations("completion deadlines must be greater than zero",
        config.header_completion_timeout_, config.body_completion_timeout_);
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_retained_bytes_,
        "HTTP client retained result byte budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_in_flight_bytes_,
        "HTTP client in-flight response budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.max_concurrent_dns_refreshes_,
        "HTTP client DNS refresh concurrency must be greater than zero");
}

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

using snapshot_owner = std::unique_ptr<static_root>;

// Snapshots are released by whichever thread drops the last reference, so the
// control block and the root both come from the process resource.
[[nodiscard]] document_root_snapshot share_snapshot(snapshot_owner owner) {
    return document_root_snapshot(std::move(owner));
}

}  // namespace

document_root_publisher::document_root_publisher(const std::filesystem::path& root,
    const static_root_config_storage& config, std::chrono::milliseconds interval,
    static_root_precompression_options precompression, static_root_resident_options resident,
    std::size_t worker_count)
    : root_(root),
      interval_(interval),
      precompression_(precompression),
      resident_(resident),
      mailboxes_(worker_count) {
    auto initial = static_root_access::make(root, config);
    static_root_access::install_resident_bodies(*initial, nullptr, resident_);
    static_root_access::install_precompressed_variants(*initial, nullptr, precompression_);
    current_ = share_snapshot(std::move(initial));
}

document_root_publisher::~document_root_publisher() = default;

document_root_publisher::mailbox::~mailbox() {
    // The graph has joined every worker before destroying the mailbox host.
    delete pending_.exchange(nullptr);
}

document_root_publisher::mailbox& document_root_publisher::register_worker() noexcept {
    if (registered_workers_ == mailboxes_.size()) {
        std::terminate();
    }
    return mailboxes_[registered_workers_++];
}

void document_root_publisher::record_failure(std::exception_ptr failure) noexcept {
    failures_.fetch_add(1, std::memory_order_relaxed);
    ruvia::report_unhandled_failure("document root refresh", std::move(failure));
}

void document_root_publisher::publish(document_root_snapshot snapshot) {
    std::vector<publication_owner> publication;
    publication.reserve(mailboxes_.size());
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication.push_back(std::make_unique<document_root_snapshot>(snapshot));
    }
    // Every allocation has succeeded before any worker can see this snapshot.
    commit_publication(std::move(snapshot), publication);
}

void document_root_publisher::commit_publication(document_root_snapshot snapshot,
    std::vector<publication_owner>& publication) noexcept {
    current_.swap(snapshot);
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication_owner superseded(
            mailboxes_[i].pending_.exchange(publication[i].release(), std::memory_order_acq_rel));
    }
    // Unread nodes are reclaimed by the next publish; adopted snapshots live
    // only as long as a worker's current generation or request leases need them.
}

task<void> document_root_publisher::refresh(
    const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop) {
    // Shared ownership lets the blocking jobs read the previous snapshot while
    // workers keep serving it, and lets resident bytes and precompression
    // reuse its unchanged file versions.
    const auto previous = current_;
    snapshot_owner candidate(nullptr);
    try {
        // The unchanged-snapshot comparison walks every entry, so it runs on
        // the pool with the rebuild; an unchanged root yields no candidate.
        auto rebuilt = co_await ruvia::run_blocking(worker, {.stop_token_ = stop}, [previous, root = root_] {
            auto built = static_root_access::make(root,
                static_root_access::copy_config(*previous));
            if (static_root_access::fingerprint(*built) == static_root_access::fingerprint(*previous) &&
                static_root_access::same_snapshot(*built, *previous)) {
                built.reset();
            }
            return built;
        });
        if (!rebuilt.completed()) {
            // A refresh is transactional: keep serving the last complete
            // index, but expose filesystem/permission failures to metrics
            // instead of silently turning them into stale content. A rejected
            // offload (pool stopping) is not a refresh failure.
            if (rebuilt.failed()) {
                record_failure(rebuilt.error());
            }
            co_return;
        }
        candidate = std::move(rebuilt).take_value();
    } catch (...) {
        // The offload wrapper reports pool shutdown as a status, but creating
        // its one-shot channel or transporting a result can still throw.
        record_failure(std::current_exception());
        co_return;
    }

    if (candidate == nullptr || !http_server_worker_running(state)) {
        co_return;
    }

    if (precompression_.enabled() || resident_.enabled()) {
        try {
            auto prepared = co_await ruvia::run_blocking(worker, {.stop_token_ = stop},
                [candidate = std::move(candidate), previous, precompression = precompression_,
                    resident = resident_]() mutable {
                    static_root_access::install_resident_bodies(*candidate, previous.get(), resident);
                    static_root_access::install_precompressed_variants(
                        *candidate, previous.get(), precompression);
                    return std::move(candidate);
                });
            if (!prepared.completed()) {
                if (prepared.failed()) {
                    record_failure(prepared.error());
                }
                co_return;
            }
            candidate = std::move(prepared).take_value();
        } catch (...) {
            record_failure(std::current_exception());
            co_return;
        }
        if (!http_server_worker_running(state)) {
            co_return;
        }
    }

    try {
        publish(share_snapshot(std::move(candidate)));
    } catch (...) {
        record_failure(std::current_exception());
    }
}

static_root_runtime::static_root_runtime(http_server_options& options, const worker_ref& worker_value,
    const http_server_worker_state& state_value)
    : options_(options),
      publisher_(options.document_root_.publisher()),
      mailbox_(publisher_ == nullptr ? nullptr : &publisher_->register_worker()),
      worker_(worker_value),
      state_(state_value),
      pending_(nullptr),
      current_(nullptr),
      retired_(),
      refreshes_(publisher_ != nullptr && mailbox_ == publisher_->mailboxes_.data()) {
    if (publisher_ != nullptr) {
        // Worker construction is serialized before startup; no publisher can
        // replace this shared_ptr while its initial reference is copied.
        current_ = std::make_unique<document_root_generation>(publisher_->current_);
        options_.document_root_.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

void static_root_runtime::adopt_published() {
    if (auto* published = mailbox_->pending_.exchange(nullptr, std::memory_order_acq_rel);
        published != nullptr) {
        pending_.reset(published);
    }
    if (pending_ == nullptr) {
        return;
    }
    // The exchanged node is exclusively ours, so copying its shared_ptr cannot
    // race the publisher reclaiming or replacing the source object.
    auto next = std::make_unique<document_root_generation>(*pending_);
    if (current_->leased()) {
        // If vector growth fails both current_ and pending_ remain owned here;
        // adoption retries next interval, or skips to a newer mailbox node.
        retired_.push_back(std::move(current_));
    }
    current_ = std::move(next);
    options_.document_root_.publish(*current_);
    pending_.reset();
}

task<void> static_root_runtime::refresh(const stop_token& stop) {
    if (publisher_ == nullptr) {
        std::terminate();
    }
    const auto interval = publisher_->interval();
    for (;;) {
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (co_await sleep_for(worker_, interval, stop) == timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!http_server_worker_running(state_)) {
            co_return;
        }

        std::erase_if(retired_, [](const generation_owner& generation) { return !generation->leased(); });

        if (refreshes_) {
            co_await publisher_->refresh(worker_, state_, stop);
            if (!http_server_worker_running(state_)) {
                co_return;
            }
        }
        try {
            adopt_published();
        } catch (...) {
            // This worker keeps serving its last generation and retains the
            // exclusively claimed publication node for the next interval.
            publisher_->record_failure(std::current_exception());
        }
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities, blocking_pool* pool,
    std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : web_worker_runtime(configuration.listeners(), routes_value, capabilities, configuration.options(), pool,
          worker_index
#if defined(RUVIA_ENABLE_HTTP3)
          ,
          http3_placement
#endif
      ) {
}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners,
    const route_table& routes_value, worker_capability_definitions capabilities,
    http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : runtime_({.queue_capacity_ = validated_options.server_.worker_queue_capacity_,
          .io_policy_ = ruvia::worker_io_policy::single_owner,
          .blocking_pool_ = pool}),
      io_context_(runtime_.io_context()),
      finalize_signal_(runtime_.worker()),
      routes_(routes_value),
      inbound_budget_(validated_options.server_.max_inbound_buffer_bytes_per_worker_),
      background_tasks_(runtime_.worker()),
      options_(std::move(validated_options)),
      static_roots_(options_, runtime_.worker(), worker_state_),
      capabilities_(io_context_, runtime_.worker(), capabilities,
          worker_capability_options{
              .default_rate_limit_ = options_.default_rate_limit_per_worker_,
              .route_rate_limits_ = routes_.has_route_rate_limit() ? route_rate_limit_presence::present
                                                                   : route_rate_limit_presence::absent,
              .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
              .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
              .http_client_worker_ = options_.server_.http_client_worker_,
              .env_ = options_.env_,
              .trusted_proxies_ =
                  options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
              .precompressed_static_files_ = options_.compression_.has_value(),
              .file_io_timeout_ = options_.server_.write_timeout_,
              .connection_failure_ = &options_.connection_failure_,
          }),
      connections_(io_context_, runtime_.worker(), routes_, capabilities_, options_, stop_token_, worker_state_, background_tasks_, listeners,
          worker_failure_sink{.target_ = &runtime_,
              .invoke_ = [](void* target, const std::exception_ptr& failure) noexcept {
                  (void)static_cast<ruvia::worker_runtime*>(target)->fail(failure);
              }},
          worker_index
#if defined(RUVIA_ENABLE_HTTP3)
          ,
          http3_placement
#endif
          ),
      web_worker_dispatch_(std::make_shared<web_worker_dispatch>(runtime_, capabilities_, background_tasks_, stop_token_)) {
    options_.inbound_budget_ = &inbound_budget_;
    options_.codecs_ = &codecs_;
    runtime_.configure({
        .startup_ = [this] {
            worker_state_ = http_server_worker_state::running;
            // Startup runs before the context does; the task starts in its first turn.
            asio::post(io_context_, [this] {
                ruvia::async_start(run_worker(), asio::bind_executor(io_context_, [this](std::exception_ptr failure) noexcept {
                    if (failure) {
                        (void)runtime_.fail(failure);
                    }
                }));
            }); },
        .serve_ = [this] { serve_on_context(); },
        .stop_admission_ = [this] { stop_admission_on_context(); },
        .finalize_ = [this] { finalize_on_context(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            worker_failures_.fetch_add(1, std::memory_order_relaxed);
            options_.failure_.notify(failure); },
        .shutdown_ = [this]() noexcept {
            connections_.retire_tls();
            worker_state_ = http_server_worker_state::stopped; },
    });
}

web_worker_runtime::~web_worker_runtime() {
    runtime_.request_stop();
    runtime_.finalize();
    runtime_.join();
}

http_server_stats web_worker_runtime::stats() const {
    auto stats = connections_.stats();
    stats.worker_failures_ = worker_failures_.load(std::memory_order_relaxed);
    return stats;
}

web_worker_ref web_worker_runtime::web_worker() const {
    return web_worker_dispatch::make_ref(web_worker_dispatch_);
}

void web_worker_runtime::serve_on_context() {
    if (static_roots_.enabled()) {
        background_tasks_.spawn(static_roots_.refresh(stop_token_));
    }
    connections_.open_admission();
}

void web_worker_runtime::stop_admission_on_context() noexcept {
    worker_state_ = http_server_worker_state::stopped;
    stop_source_.request_stop();
    connections_.stop();
    runtime_.finalize();
}

void web_worker_runtime::finalize_on_context() noexcept {
    capabilities_.close_now();
    finalize_signal_.notify();
}

task<void> web_worker_runtime::run_worker() {
    try {
        // A stop applied before this task first runs has already stopped the
        // worker timers; starting the scanner or capability I/O would fail.
        if (!stop_token_.stop_requested()) {
            connections_.prepare();
            co_await capabilities_.start();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
        }
    } catch (...) {
        // A stop during startup closes the capabilities whose connect is still
        // pending; that abort is the requested cancellation, not a failure.
        if (!stop_token_.stop_requested()) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    // Retain worker-local state until finalization joins sessions and pending
    // listener operations on this same owner.
    co_await finalize_signal_.wait();
    try {
        co_await background_tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
    try {
        co_await capabilities_.stop();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

}  // namespace ruvia::detail
