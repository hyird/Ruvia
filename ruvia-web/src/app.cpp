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
#include "ruvia/web/middleware.h"

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

#include "ruvia/http/borrowed_text.h"

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

#include <cassert>
#include <charconv>
#include <new>

#include <asio.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include "ruvia/core/task.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/streaming.h"

#if defined(_WIN32)
#include <mswsock.h>
#include <ws2tcpip.h>

#include <asio/windows/object_handle.hpp>
#include <asio/windows/overlapped_ptr.hpp>
#endif
#if !defined(_WIN32)
#include <sys/socket.h>
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include <ngtcp2/ngtcp2.h>
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "route_table.h"
#endif
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
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

namespace ruvia::detail {
namespace {

void configure_accepted_socket(asio::ip::tcp::socket& socket) {
    // Accepted sockets disable Nagle and keep the system keep-alive default.
    constexpr client_tcp_config policies{.keep_alive_ = tcp_keep_alive_policy::system_default};
    if (const auto error = apply_tcp_socket_policies(socket, policies)) {
        throw std::system_error(error, "failed to configure accepted TCP socket");
    }
}

#if !defined(_WIN32)
void configure_listener_address(asio::ip::tcp::acceptor& socket) {
    socket.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
    const int enabled = 1;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable TCP listener SO_REUSEPORT");
    }
#else
    throw std::system_error(asio::error::operation_not_supported, "TCP worker listeners require SO_REUSEPORT");
#endif
}
#endif

// The scanner also collects the worker's thread heap after an idle interval.
[[nodiscard]] connection_scanner_options make_connection_scanner_options(
    const http_server_options& options) noexcept {
    const auto& server = options.server_;
    return connection_scanner_options{.scan_interval_ = server.connection_scan_interval_,
        .idle_timeout_ = server.idle_timeout_,
        .initial_read_timeout_ = server.request_header_timeout_,
        .payload_read_timeout_ = server.request_body_timeout_,
        .write_timeout_ = server.write_timeout_,
        .initial_read_completion_timeout_ = server.header_completion_timeout_,
        .payload_read_completion_timeout_ = server.body_completion_timeout_};
}

// Names the peer for the session's services and its failure report. Both stay
// empty when the socket no longer knew its peer.
void name_remote_endpoint(
    const std::optional<asio::ip::tcp::endpoint>& remote, std::string& address, std::uint16_t& port) {
    if (remote.has_value()) {
        assign_peer_address(address, remote->address());
        port = remote->port();
    }
}

}  // namespace

worker_connections::worker_connections(asio::io_context& io, const worker_ref& worker_value,
    const route_table& routes_value, worker_capabilities& capabilities,
    http_server_options& options, const stop_token& stop_token_value, http_server_worker_state& state_value,
    task_scope& tasks, std::span<const http_server_listener_definition> listeners, worker_failure_sink failure,
    std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : io_(io),
      worker_(worker_value),
      routes_(routes_value),
      capabilities_(capabilities),
      options_(options),
      stop_token_(stop_token_value),
      state_(state_value),
      tasks_(tasks),
      failure_(failure),
      worker_index_(worker_index),
      listeners_(),
      scanner_(worker_value, make_connection_scanner_options(options))
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      http3_placement_(http3_placement),
      http3_services_{.io_ = io,
          .worker_ = worker_value,
          .routes_ = routes_value,
          .capabilities_ = capabilities,
          .options_ = options,
          .scanner_ = scanner_,
          .connection_count_ = connection_count_,
          .connection_ids_ = connection_ids_,
          .registry_ = registry_,
          .stop_token_ = stop_token_value,
          .state_ = state_value,
          .refused_ = refused_,
          .placement_ = http3_placement},
      http3_endpoints_()
#endif
{
    listeners_.reserve(listeners.size());
    for (const auto& configured : listeners) {
        listeners_.push_back(
            std::make_unique<listener>(io, worker_value, configured));
        prepare_listener_gauge(listeners_.size() - 1, configured.endpoint_);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // Every worker (and the Windows acceptor) numbers the HTTP/3 listeners
    // alike, so a handed-over datagram names its endpoint by index.
    for (std::size_t index = 0; index < listeners_.size(); ++index) {
        if (listeners_[index]->session_.http3_.has_value()) {
            add_http3_endpoint(index, http3_endpoints_.size());
        }
    }
#endif
    options_.connection_failure_.counter_ = &failures_;
}
#if defined(RUVIA_ENABLE_HTTP3)
void worker_connections::add_http3_endpoint(std::size_t listener_index, std::size_t http3_index) {
    auto& configured = *listeners_[listener_index];
    if (http3_endpoints_.size() <= http3_index) {
        http3_endpoints_.resize(http3_index + 1);
    }
    http3_endpoints_[http3_index] = std::make_unique<http3_endpoint>(http3_services_, configured.session_, asio::ip::udp::endpoint(configured.endpoint_.address(), configured.session_.http3_->port_), http3_index, configured.tasks_, gauges_[listener_index]->count_);
    configured.http3_index_ = http3_index;
}
#endif
#if defined(_WIN32)
windows_acceptor::business_worker worker_connections::acceptor_target() noexcept {
    return {
        .worker_ = worker_,
        .load_ = &connection_count_,
        .target_ = this,
        .adopt_ = [](void* target, std::size_t listener_index, windows_accepted_socket accepted) {
            static_cast<worker_connections*>(target)->adopt_socket(listener_index, std::move(accepted));
        },
    };
}

void worker_connections::adopt_socket(std::size_t listener_index, windows_accepted_socket accepted) {
    assert(worker_.is_current());
    // A listener removed since the acceptor took the socket admits nothing:
    // the socket closes with `accepted`.
    if (listener_index >= listeners_.size() || listeners_[listener_index] == nullptr ||
        !listeners_[listener_index]->admitting_) {
        return;
    }
    tcp_socket_type socket(io_);
    std::error_code error;
    socket.assign(listeners_[listener_index]->endpoint_.protocol(), accepted.get(), error);
    if (error) {
        // The acceptor's socket closes with `accepted`.
        try {
            throw std::system_error(error, "failed to bind an accepted TCP socket to its worker");
        } catch (...) {
            report_accept_failure(std::current_exception(), false);
        }
        return;
    }
    // The socket now owns the handle.
    (void)accepted.release();
    accept_socket(listener_index, std::move(socket));
}
#endif
void worker_connections::prepare() {
    try {
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            prepare_listener_resources(index);
        }
        scanner_.start();
    } catch (...) {
        // The listeners already prepared retire with the failed worker's stop.
        for (auto& configured : listeners_) {
            close_listener_sockets(*configured);
        }
        scanner_.stop();
        retire_tls();
        throw;
    }
}
void worker_connections::prepare_listener_resources(std::size_t index) {
    auto& configured = *listeners_[index];
    if (configured.session_.tls() && !configured.session_.tls_context_) {
        configure_http_server_tls_contexts(configured.session_, static_cast<bool>(options_.tls_client_hello_));
    }
#if !defined(_WIN32)
    auto& socket = configured.socket_;
    const auto inherited =
        configured.inherited_ != nullptr ? configured.inherited_->claim_tcp(worker_index_) : invalid_native_socket;
    if (inherited != invalid_native_socket) {
        // Checked to listen on this endpoint when it was handed over.
        std::error_code error;
        socket.assign(configured.endpoint_.protocol(), inherited, error);
        if (error) {
            close_native_socket(inherited);
            throw std::system_error(error, "failed to adopt an inherited TCP listener socket");
        }
    } else if (!configured.externally_accepted_) {
        socket.open(configured.endpoint_.protocol());
        configure_listener_address(socket);
        if (configured.session_.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*configured.session_.ipv6_only_));
        }
        socket.bind(configured.endpoint_);
        socket.listen(asio::socket_base::max_listen_connections);
    }
#endif
    tasks_.spawn(retire_listener(index));
    configured.prepared_ = true;
#if defined(RUVIA_ENABLE_HTTP3)
    // After configure_http_server_tls_contexts built the QUIC context.
    if (configured.http3_index_.has_value()) {
#if defined(_WIN32)
        http3_endpoints_[*configured.http3_index_]->open(nullptr);
#else
        http3_endpoints_[*configured.http3_index_]->open(configured.inherited_.get());
#endif
    }
#endif
#if !defined(_WIN32)
    // What this worker did not claim belongs to the other workers and closes
    // with the last of them.
    configured.inherited_.reset();
#endif
    if (configured.session_.ticket_keys_.has_value() && !rotating_ticket_keys_) {
        tasks_.spawn(rotate_tls_ticket_keys());
    }
#if !defined(_WIN32)
    if (!configured.externally_accepted_) {
        configured.tasks_.spawn(run_accept_loop(index));
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_ && !http3_attached_) {
        http3_placement_.directory_->attach(http3_placement_.worker_index_, this,
            [](void* target, std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
                static_cast<worker_connections*>(target)->receive_http3_handoff(listener_index, datagram);
            });
        http3_attached_ = true;
    }
#endif
}
void worker_connections::open_admission() noexcept {
    for (std::size_t index = 0; index < listeners_.size(); ++index) {
        start_listener(index);
    }
}
void worker_connections::start_listener(std::size_t index) noexcept {
    auto& configured = *listeners_[index];
    configured.admitting_ = true;
    gauges_[index]->serving_.store(true, std::memory_order_relaxed);
    gauges_[index]->published_.store(true, std::memory_order_relaxed);
    configured.activated_.notify();
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_) {
        http3_endpoints_[*configured.http3_index_]->start();
    }
#endif
}
void worker_connections::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition, bool externally_accepted
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    prepare_listener(index, http3_index, definition, externally_accepted, {}
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        directory
#endif
    );
}
void worker_connections::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition, bool externally_accepted,
    prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        throw std::logic_error("the worker is stopping");
    }
    if (listeners_.size() <= index) {
        listeners_.resize(index + 1);
    }
    listeners_[index] = std::make_unique<listener>(io_, worker_, definition, std::move(session));
    prepare_listener_gauge(index, definition.endpoint_);
    listeners_[index]->externally_accepted_ = externally_accepted;
    try {
#if defined(RUVIA_ENABLE_HTTP3)
        if (http3_index.has_value()) {
            // The application creates the directory with its first HTTP/3 listener.
            if (http3_placement_.directory_ == nullptr) {
                http3_placement_.directory_ = directory;
                http3_services_.placement_.directory_ = directory;
            }
            add_http3_endpoint(index, *http3_index);
        }
#endif
        prepare_listener_resources(index);
    } catch (...) {
        if (listeners_[index]->prepared_) {
            discard_listener(index);
            throw;
        }
        // No task owns the listener yet.
#if defined(RUVIA_ENABLE_HTTP3)
        if (http3_index.has_value() && *http3_index < http3_endpoints_.size()) {
            http3_endpoints_[*http3_index].reset();
        }
#endif
        listeners_[index].reset();
        throw;
    }
}
void worker_connections::open_listener(std::size_t index) noexcept {
    assert(worker_.is_current());
    // A stopping worker admits nothing; its stop retires the listener.
    if (!http_server_worker_running(state_) || index >= listeners_.size() || listeners_[index] == nullptr) {
        return;
    }
    start_listener(index);
}
void worker_connections::discard_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    assert(worker_.is_current());
    if (index >= listeners_.size() || listeners_[index] == nullptr) {
        return;
    }
    // A listener in a slot was prepared, so a task awaits its retirement.
    auto& configured = *listeners_[index];
    configured.retirement_receipt_ = std::move(receipt);
    gauges_[index]->serving_.store(false, std::memory_order_relaxed);
    registry_.close_listener(&configured.session_);
    close_listener_sockets(configured);
    configured.retired_.notify();
}
void worker_connections::remove_listener(std::size_t index, [[maybe_unused]] std::shared_ptr<void> receipt, listener_remove_options options) noexcept {
    assert(worker_.is_current());
    // Not admitting: the worker's stop already retired it.
    if (index >= listeners_.size() || listeners_[index] == nullptr || !listeners_[index]->admitting_) {
        return;
    }
    auto& configured = *listeners_[index];
    configured.admitting_ = false;
    gauges_[index]->serving_.store(false, std::memory_order_relaxed);
    if (options.close_connections_) {
        registry_.close_listener(&configured.session_);
    }
#if !defined(_WIN32)
    // Closing cancels the pending accept; the loop then exits.
    asio::error_code ignored;
    configured.socket_.close(ignored);
    if (configured.accepting_) {
        configured.removal_receipt_ = std::move(receipt);
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_]->retire();
    }
#endif
    configured.retired_.notify();
}
#if !defined(_WIN32)
listener_socket_duplicates worker_connections::duplicate_listener(std::size_t index) {
    assert(worker_.is_current());
    if (index >= listeners_.size() || listeners_[index] == nullptr || !listeners_[index]->admitting_ ||
        !listeners_[index]->socket_.is_open()) {
        throw std::logic_error("the listener no longer accepts on this worker");
    }
    auto& configured = *listeners_[index];
    listener_socket_duplicates duplicates;
    duplicates.tcp_ = duplicate_native_socket(configured.socket_.native_handle());
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        try {
            duplicates.udp_ = http3_endpoints_[*configured.http3_index_]->duplicate_socket();
        } catch (...) {
            close_native_socket(duplicates.tcp_);
            throw;
        }
    }
#endif
    return duplicates;
}
#endif
void worker_connections::close_listener_sockets(listener& configured) noexcept {
    configured.admitting_ = false;
    configured.activated_.notify();
#if !defined(_WIN32)
    asio::error_code ignored;
    configured.socket_.close(ignored);
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_]->close();
    }
#endif
}
task<void> worker_connections::retire_listener(std::size_t index) {
    auto& configured = *listeners_[index];
    co_await configured.retired_.wait();
    co_await configured.tasks_.join();
#if defined(RUVIA_ENABLE_HTTP3)
    if (configured.http3_index_.has_value()) {
        http3_endpoints_[*configured.http3_index_].reset();
    }
#endif
    listeners_[index].reset();
}
void worker_connections::stop() noexcept {
    {
        std::lock_guard lock(gauges_mutex_);
        for (const auto& gauge : gauges_) {
            if (gauge) {
                gauge->serving_.store(false, std::memory_order_relaxed);
            }
        }
    }
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            configured->admitting_ = false;
        }
    }
#if !defined(_WIN32)
    close_listeners();
#endif
    scanner_.stop();
    scanner_.close_all();
#if defined(RUVIA_ENABLE_HTTP3)
    // After close_all() sent each QUIC connection's CONNECTION_CLOSE.
    if (std::exchange(http3_attached_, false)) {
        http3_placement_.directory_->detach(http3_placement_.worker_index_);
    }
    for (auto& endpoint : http3_endpoints_) {
        if (endpoint != nullptr) {
            endpoint->close();
        }
    }
#endif
    // Each listener retires once the tasks these closes end have unwound.
    for (auto& configured : listeners_) {
        if (configured != nullptr && configured->prepared_) {
            configured->activated_.notify();
            configured->retired_.notify();
        }
    }
}
#if defined(RUVIA_ENABLE_HTTP3)
void worker_connections::receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept {
    // A retired endpoint's slot stays empty.
    if (listener_index < http3_endpoints_.size() && http3_endpoints_[listener_index] != nullptr) {
        http3_endpoints_[listener_index]->receive_handoff(datagram);
    }
}
#endif
void worker_connections::retire_tls() noexcept {
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            auto& session = configured->session_;
            session.tls_context_.reset();
            session.quic_tls_context_.reset();
            session.ticket_keys_.reset();
        }
    }
}
void worker_connections::adopt_tls_identities(
    std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    assert(worker_.is_current());
    if (listener_index < listeners_.size() && listeners_[listener_index] != nullptr) {
        listeners_[listener_index]->session_.adopt_tls_identities(std::move(identities));
    }
}
// RFC 9325 §3.4: adopts each TLS listener's next ticket key epoch as it begins.
// It keeps running while the worker serves a TLS listener and ends when its
// timers stop, so this worker holds no key past its validity by more than the
// timer latency. The keys change between handshakes on this thread, never
// during one. Every listener of the run shares one key epoch origin, so a
// listener added while this waits rotates at the instant it waits for.
task<void> worker_connections::rotate_tls_ticket_keys() {
    rotating_ticket_keys_ = true;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        auto next = std::chrono::steady_clock::time_point::max();
        for (auto& configured : listeners_) {
            if (configured == nullptr) {
                continue;
            }
            auto& session = configured->session_;
            const auto* tls = session.tls();
            if (tls == nullptr || !session.ticket_keys_.has_value()) {
                continue;
            }
            auto& ring = tls_session_ticket_keys(*tls);
            if (!ring.copy_to(now, *session.ticket_keys_)) {
                // This worker issues no tickets until a copy succeeds; retry
                // soon rather than waiting out the epoch, since another worker
                // may already have generated the shared key.
                try {
                    throw std::runtime_error("failed to generate TLS session ticket keys");
                } catch (...) {
                    ruvia::report_unhandled_failure("TLS session ticket key rotation", std::current_exception());
                }
                next = std::min(next, now + tls_ticket_key_retry_delay);
            }
            next = std::min(next, ring.next_rotation(now));
        }
        if (next == std::chrono::steady_clock::time_point::max() ||
            co_await sleep_until(worker_, next, stop_token_) == timer_sleep_result::stop_requested) {
            break;
        }
    }
    // A TLS listener added later starts the next one.
    rotating_ticket_keys_ = false;
}
void worker_connections::prepare_listener_gauge(std::size_t index, const asio::ip::tcp::endpoint& endpoint) {
    auto gauge = std::make_shared<listener_gauge>(endpoint, connection_count_);
    std::lock_guard lock(gauges_mutex_);
    if (gauges_.size() <= index) {
        gauges_.resize(index + 1);
    }
    gauges_[index] = std::move(gauge);
}

std::shared_ptr<connection_count> worker_connections::listener_connections(const asio::ip::tcp::endpoint& endpoint) const {
    std::lock_guard lock(gauges_mutex_);
    for (auto it = gauges_.rbegin(); it != gauges_.rend(); ++it) {
        if (*it && (*it)->published_.load(std::memory_order_relaxed) && (*it)->endpoint_ == endpoint) {
            return std::shared_ptr<connection_count>(*it, &(*it)->count_);
        }
    }
    throw std::invalid_argument("no listener serves this endpoint");
}

http_server_stats worker_connections::stats() const {
    http_server_stats result;
    result.active_connections_ = connection_count_.active();
    result.connections_refused_ = refused_.load(std::memory_order_relaxed);
    result.connection_failures_ = failures_.load(std::memory_order_relaxed);
    result.accept_failures_ = accept_failures_.load(std::memory_order_relaxed);
    std::lock_guard lock(gauges_mutex_);
    for (const auto& gauge : gauges_) {
        if (!gauge) {
            continue;
        }
        const auto active = gauge->count_.active();
        if (gauge->serving_.load(std::memory_order_relaxed) || active != 0) {
            result.listeners_.push_back({.address_ = to_ip_address(gauge->endpoint_.address()),
                .port_ = gauge->endpoint_.port(),
                .active_connections_ = active});
        }
    }
    return result;
}

#if !defined(_WIN32)
void worker_connections::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        if (configured != nullptr) {
            asio::error_code ignored;
            configured->socket_.close(ignored);
        }
    }
}

task<void> worker_connections::run_accept_loop(std::size_t index) {
    auto& configured = *listeners_[index];
    co_await configured.activated_.wait();
    configured.accepting_ = true;
    try {
        while (http_server_worker_running(state_) && configured.socket_.is_open()) {
            // The accepted socket is born on this worker and never changes
            // executor. Only the long-lived listener task wraps acceptance.
            tcp_socket_type socket(io_);
            const auto completion = co_await ruvia::async_asio([&configured, &socket](auto handler) {
                configured.socket_.async_accept(socket, std::move(handler));
            });
            if (!http_server_worker_running(state_) || !configured.socket_.is_open()) {
                break;
            }
            const auto error = completion.error_code();
            if (!error) {
                accept_socket(index, std::move(socket));
                continue;
            }
            const auto action = classify_accept_error(error);
            if (action == accept_error_action::fail) {
                throw std::system_error(error, "fatal TCP accept error");
            }
            if (action == accept_error_action::retry) {
                continue;
            }
            try {
                throw std::system_error(error, "recoverable TCP accept error");
            } catch (...) {
                report_accept_failure(std::current_exception(), false);
            }
            if (co_await sleep_for(worker_, std::chrono::milliseconds(25), stop_token_) ==
                timer_sleep_result::stop_requested) {
                break;
            }
        }
    } catch (...) {
        report_accept_failure(std::current_exception(), true);
    }
    // A removal waits for this loop to exit.
    configured.accepting_ = false;
    configured.removal_receipt_.reset();
}
#endif

void worker_connections::report_accept_failure(std::exception_ptr failure, bool fatal) noexcept {
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (fatal) {
#if !defined(_WIN32)
        close_listeners();
#endif
        failure_.notify(std::move(failure));
    } else {
        ruvia::report_unhandled_failure("web accept", std::move(failure));
    }
}

// Plain and TLS sessions are separate coroutines so a plain connection's frame
// carries none of the TLS stream, handshake and certificate state. Both keep
// the remote address outside their try so the failure report can name the
// peer; it stays empty if the failure happened before the address was resolved.
task<void> worker_connections::run_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    inbound_socket_stream input(socket, prefix, &lifecycle);
    std::string remote_address;
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        if (listener_value.connection_.has_value()) {
            co_await serve_connection(input, socket,
                server_connection_fields{.id_ = connection_id,
                    .local_address_ = {},
                    .local_port_ = 0,
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .tls_ = std::nullopt,
                    .state_ = &lifecycle.state(),
                    .lifecycle_ = &lifecycle});
            ruvia::close_socket(socket);
            co_return;
        }
        const auto close_mode = co_await run_stream(listener_value, input, socket,
            capabilities_.make_context_services(stop_token_)
                .with_plain_transport(connection_id, remote_address, remote_port)
                .with_connection_lifecycle(lifecycle)
                .with_request_stream(listener_value.request_stream_ ? options_.request_stream_ : request_stream_callback_ref{}));
        // Plain TCP has no close alert: only a lingering close differs from an
        // immediate one. A worker stop closes at once; scanner close_all
        // reaches a lingering close that is already running.
        if (close_mode == http_connection_close::linger && !stop_token_.stop_requested()) {
            co_await linger_http_connection(socket, scanner_, worker_);
        }
        ruvia::close_socket(socket);
    } catch (...) {
        lifecycle.note_exception(std::current_exception());
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

task<void> worker_connections::run_tls_session(http_server_session_config& listener_value,
    accepted_connection_lease connection, std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id, std::vector<std::byte> prefix) {
    auto& socket = connection.socket();
    auto& lifecycle = connection.lifecycle();
    inbound_socket_stream input(socket, prefix, &lifecycle);
    std::string remote_address;
    std::uint16_t remote_port = 0;
    try {
        name_remote_endpoint(remote, remote_address, remote_port);
        // A stopping worker admits no new request, and a handshake leads only to one.
        if (!http_server_worker_running(state_) || !socket.is_open()) {
            co_return;
        }
        connection_scanner::entry handshake_entry;
        handshake_entry.bind_lifecycle(&lifecycle);
        std::optional<connection_scanner::guard> handshake_guard;
        handshake_guard.emplace(scanner_, handshake_entry, connection_transport::tcp(socket));
        handshake_entry.read_request_head(connection_scanner::phase::reading_initial);
        if (listener_value.accepts_plaintext()) {
            // Peek one byte under the same initial-read clocks used by TLS.
            // The TLS handshake below does not receive a fresh deadline.
            unsigned char first = prefix.empty() ? 0 : std::to_integer<unsigned char>(prefix.front());
            if (prefix.empty()) {
                const auto completion = co_await ruvia::async_asio<std::size_t>([&socket, &first](auto handler) {
                    socket.async_receive(asio::buffer(&first, 1), asio::socket_base::message_peek, std::move(handler));
                });
                if (completion.error_code() || completion.result() == 0 || handshake_entry.close_requested()) {
                    lifecycle.note_error(completion.error_code());
                    co_return;
                }
            }
            if (first != 0x16) {
                handshake_guard.reset();
                co_await run_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix));
                co_return;
            }
        }
        // Declared before the TLS stream: its SSL borrows them while it lives.
        // Identity selection matches the local address; the ClientHello
        // observer names it.
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        const auto local_address = local.address();
        std::string local_address_text;
        std::optional<tls_client_hello_observation> client_hello;
        if (options_.tls_client_hello_ || tls_certificate_selection_enabled(listener_value)) {
            if (!local_error) {
                assign_peer_address(local_address_text, local_address);
            }
            client_hello.emplace(tls_client_hello_observation{.callback_ = options_.tls_client_hello_,
                .fields_ = {.connection_id_ = connection_id,
                    .local_address_ = local_address_text,
                    .local_port_ = local_error ? std::uint16_t{0} : local.port(),
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .quic_ = false,
                    .message_ = {}}});
        }
        server_tls_stream tls_stream(input, *listener_value.tls_context_);
        if (!local_error) {
            bind_tls_local_address(*tls_stream.native_handle(), local_address);
        }
        if (client_hello.has_value()) {
            observe_tls_client_hello(*tls_stream.native_handle(), *client_hello);
        }
        const auto handshake_completion = co_await ruvia::async_asio([&tls_stream](auto handler) {
            tls_stream.async_handshake(asio::ssl::stream_base::server, std::move(handler));
        });
        if (handshake_completion.error_code() || handshake_entry.close_requested()) {
            lifecycle.note_error(handshake_completion.error_code());
            ruvia::close_socket(socket);
            co_return;
        }
        // Never retain the handshake clocks while dispatching HTTP requests.
        handshake_guard.reset();
        std::string client_certificate;
        std::optional<std::string_view> client_subject;
        if (listener_value.tls()->client_certificates_.has_value()) {
            client_subject = extract_tls_client_certificate(tls_stream.native_handle(), client_certificate);
        }
        auto& native_tls = *tls_stream.native_handle();
        const auto alpn = tls_selected_alpn(native_tls);
        const auto server_name = tls_server_name(native_tls);
        if (listener_value.connection_.has_value()) {
            co_await serve_connection(tls_stream, socket,
                server_connection_fields{.id_ = connection_id,
                    .local_address_ = {},
                    .local_port_ = 0,
                    .remote_address_ = remote_address,
                    .remote_port_ = remote_port,
                    .tls_ = server_connection_fields::tls_type{.client_certificate_subject_ = client_subject,
                        .alpn_ = alpn,
                        .server_name_ = server_name},
                    .state_ = &lifecycle.state(),
                    .lifecycle_ = &lifecycle});
            ruvia::close_socket(socket);
            co_return;
        }
        tls_served_certificate served_certificate(SSL_get_certificate(&native_tls));
        const auto services = capabilities_.make_context_services(stop_token_)
                                  .with_tls_transport(connection_id, remote_address, client_subject, alpn, server_name,
                                      remote_port, served_certificate)
                                  .with_connection_lifecycle(lifecycle)
                                  .with_alternative_service(listener_value.alternative_service_)
                                  .with_request_stream(listener_value.request_stream_ ? options_.request_stream_
                                                                                      : request_stream_callback_ref{});
#if defined(RUVIA_ENABLE_HTTP2)
        const auto close_mode = alpn == "h2"
                                    ? co_await run_http2(tls_stream, socket, services)
                                    : co_await run_stream(listener_value, tls_stream, socket, services);
#else
        const auto close_mode = co_await run_stream(listener_value, tls_stream, socket, services);
#endif
        // A worker stop keeps the immediate close; scanner close_all reaches a
        // graceful close that is already running.
        if (close_mode != http_connection_close::abort && !stop_token_.stop_requested()) {
            co_await close_http_connection_gracefully(tls_stream, close_mode, scanner_, worker_);
        }
        ruvia::close_socket(socket);
    } catch (...) {
        lifecycle.note_exception(std::current_exception());
        report_session_failure(socket, remote_address, std::current_exception());
    }
}

void worker_connections::report_session_failure(
    tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept {
    // Last-resort safety net: any exception that escapes the session
    // body (including bad_alloc, error-handler failures, or framework
    // bugs) must not reach tasks_, where a child failure stops the scope.
    // Socket state may be partially written or completely fine; we
    // cannot safely emit anything new, so just drop the connection.
    //
    // Dropping the connection must not also drop the reason: this is the
    // only place that reason exists, so it goes to the connection-failure
    // sink before the frame unwinds.
    ruvia::close_socket(socket);
    options_.connection_failure_.invoke(remote_address, std::move(failure));
}

void worker_connections::adopt_connection(std::size_t index, native_socket native, adopt_options options) {
    struct native_owner final {
        native_socket socket_;
        ~native_owner() {
            close_native_socket(socket_);
        }
    } owner{native};
    tcp_socket_type socket(io_);
    if (!worker_.is_current() || index >= listeners_.size() || listeners_[index] == nullptr ||
        !listeners_[index]->admitting_ || !http_server_worker_running(state_)) {
        throw std::logic_error("connection service is not accepting on this worker");
    }
    std::error_code error;
    socket.assign(listeners_[index]->endpoint_.protocol(), native, error);
    if (error) {
        throw std::system_error(error, "failed to adopt an accepted connection");
    }
    owner.socket_ = invalid_native_socket;
    (void)socket.remote_endpoint(error);
    if (error) {
        throw std::invalid_argument("adopted connection must be a connected TCP socket");
    }
    accept_socket(index, std::move(socket), options);
}

bool worker_connections::close_connection(std::uint64_t id) noexcept {
    assert(worker_.is_current());
    return registry_.close_connection(id);
}

void worker_connections::accept_socket(std::size_t index, tcp_socket_type socket, adopt_options options) {
    if (options.prefix_.size() > options_.server_.max_inbound_buffer_bytes_per_connection_) {
        throw std::length_error("adopted prefix exceeds the connection input budget");
    }
    std::vector<std::byte> prefix(options.prefix_.begin(), options.prefix_.end());
    if (listeners_[index]->session_.proxy_protocol_) {
        listeners_[index]->tasks_.spawn(prepare_connection(index, std::move(socket), std::move(prefix), options.remote_));
    } else {
        const auto bytes_read = prefix.size();
        admit_socket(index, std::move(socket), options.remote_, std::move(prefix), bytes_read);
    }
}

task<void> worker_connections::prepare_connection(std::size_t index, tcp_socket_type socket,
    std::vector<std::byte> prefix, std::optional<ip_endpoint> remote) {
    try {
        operation_interrupt_source source;
        auto limit = options_.server_.header_completion_timeout_;
        if (const auto request = options_.server_.request_header_timeout_; request && (!limit || *request < *limit)) {
            limit = request;
        }
        operation_interrupt interrupt(source, worker_, {.timeout_ = operation_timeout(limit.value_or(std::chrono::seconds(30))), .stop_token_ = stop_token_}, &socket, [](void* raw) noexcept { ruvia::close_socket(*static_cast<tcp_socket_type*>(raw)); }, "PROXY protocol header");
        proxy_protocol_parser parser;
        std::array<std::byte, 1024> buffer;
        std::span<const std::byte> input = prefix;
        auto bytes_read = prefix.size();
        for (;;) {
            const auto parsed = parser.parse(input);
            if (parsed.complete_) {
                const auto& header = parser.header();
                if (header.transport() == proxy_protocol_transport::datagram) {
                    throw proxy_protocol_error("a TCP listener requires a stream PROXY transport");
                }
                if (header.source()) {
                    remote = header.source();
                }
                std::vector<std::byte> remaining(input.begin() + parsed.consumed_bytes_, input.end());
                interrupt.throw_if_interrupted();
                admit_socket(index, std::move(socket), remote, std::move(remaining), bytes_read);
                co_return;
            }
            const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                socket.async_read_some(asio::buffer(buffer), std::move(handler));
            });
            interrupt.throw_if_interrupted();
            if (completion.error_code() || completion.result() == 0) {
                co_return;
            }
            bytes_read += completion.result();
            input = std::span<const std::byte>(buffer.data(), completion.result());
        }
    } catch (const proxy_protocol_error&) {
        // Malformed mandatory prefaces are untrusted input, not host failures.
    } catch (const std::system_error&) {
        // EOF, timeout and shutdown close only this pending connection.
    } catch (...) {
        report_session_failure(socket, {}, std::current_exception());
    }
}

void worker_connections::admit_socket(std::size_t listener_index, tcp_socket_type socket,
    std::optional<ip_endpoint> effective_remote, std::vector<std::byte> prefix, std::size_t bytes_read) {
    assert(worker_.is_current());
    if (!http_server_worker_running(state_)) {
        return;
    }

    // Read the peer before the socket moves into the lease: a failure to
    // start the session destroys the lease, and its socket, with it.
    std::optional<asio::ip::tcp::endpoint> remote;
    std::error_code remote_error;
    if (auto endpoint = socket.remote_endpoint(remote_error); !remote_error) {
        remote = endpoint;
    }
    if (effective_remote) {
        remote.emplace(to_asio_address(effective_remote->address()), effective_remote->port());
    }
    try {
        configure_accepted_socket(socket);
        std::error_code local_error;
        const auto local = socket.local_endpoint(local_error);
        if (local_error || !remote.has_value()) {
            return;
        }
        const auto connection_id = connection_ids_.next();
        connection_event event(connection_id, {to_ip_address(local.address()), local.port()},
            {to_ip_address(remote->address()), remote->port()}, false);
        if (options_.connection_admission_ && !options_.connection_admission_(event)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto& max_connections = options_.server_.max_connections_per_worker_;
        // Rejected peers must not evict an existing reusable connection.
        if (max_connections.has_value() && connection_count_.active() >= *max_connections &&
            !scanner_.evict_oldest_reusable()) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        connection_lifecycle lifecycle(registry_, &listeners_[listener_index]->session_, std::move(event), options_.connection_open_, options_.connection_close_,
            0, connection_transport::tcp(socket));
        lifecycle.note_read(bytes_read);
        accepted_connection_lease connection(std::move(socket), gauges_[listener_index]->count_, std::move(lifecycle));
        // The session borrows its listener, whose scope joins it before the
        // listener retires.
        auto& configured = *listeners_[listener_index];
        auto& listener_value = configured.session_;
        configured.tasks_.spawn(listener_value.tls() != nullptr
                                    ? run_tls_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix))
                                    : run_session(listener_value, std::move(connection), remote, connection_id, std::move(prefix)));
    } catch (...) {
        auto failure = std::current_exception();
        std::string remote_address;
        std::uint16_t remote_port = 0;
        try {
            name_remote_endpoint(remote, remote_address, remote_port);
        } catch (...) {
            remote_address.clear();
        }
        accept_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.connection_failure_.invoke(remote_address, std::move(failure));
    }
}

}  // namespace ruvia::detail

#if defined(_WIN32)

namespace ruvia::detail {
namespace {

// AcceptEx operations kept outstanding per listener: a burst of connections
// is taken without waiting for each completion to reissue the next; the
// listen backlog holds the rest. Product limit.
constexpr std::size_t accepts_per_listener = 8;
// AcceptEx reserves each address with 16 bytes of slack.
constexpr DWORD accept_address_bytes = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
// The pause after a failed accept, so a persistent resource error does not
// spin the acceptor.
constexpr std::chrono::milliseconds accept_retry_delay{25};
// The acceptor's queue holds the datagrams workers send; each worker keeps
// at most http3_max_datagrams_in_flight in it.
constexpr std::size_t minimum_queue_capacity = 64;

[[nodiscard]] std::error_code last_socket_error() noexcept {
    return {::WSAGetLastError(), asio::system_category()};
}

// No other socket, of this process or another, may bind the endpoint while
// the acceptor holds it.
template <typename socket_type>
void claim_exclusive_address(socket_type& socket) {
    const BOOL enabled = TRUE;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled))) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to enable listener SO_EXCLUSIVEADDRUSE");
    }
}

[[nodiscard]] LPFN_ACCEPTEX resolve_accept_ex(SOCKET listener) {
    GUID identifier = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX accept_ex = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &accept_ex, static_cast<DWORD>(sizeof(accept_ex)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve listener AcceptEx");
    }
    return accept_ex;
}

#if defined(RUVIA_ENABLE_HTTP3)
template <typename function_type>
[[nodiscard]] function_type resolve_message_extension(SOCKET socket, GUID identifier) {
    function_type function = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &function, static_cast<DWORD>(sizeof(function)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve UDP message extension");
    }
    return function;
}

void enable_udp_packet_info(asio::ip::udp::socket& socket) {
    const DWORD enabled = 1;
    const auto native = socket.native_handle();
    const auto enable = [&](int level, int option) {
        return ::setsockopt(native, level, option, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled)));
    };
    if (socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
        if (enable(IPPROTO_IPV6, IPV6_PKTINFO) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to enable IPv6 UDP packet information");
        }
        DWORD only_v6 = 0;
        int size = sizeof(only_v6);
        if (::getsockopt(native, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&only_v6), &size) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to inspect UDP dual-stack policy");
        }
        if (only_v6 != 0) {
            return;
        }
    }
    if (enable(IPPROTO_IP, IP_PKTINFO) == SOCKET_ERROR) {
        const auto error = last_socket_error();
        // Winsock documents WSAEINVAL on dual-stack sockets when IPv4 is disabled.
        if (error.value() == WSAEINVAL && socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
            const auto probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (probe == INVALID_SOCKET && ::WSAGetLastError() == WSAEAFNOSUPPORT) {
                return;
            }
            if (probe != INVALID_SOCKET) {
                (void)::closesocket(probe);
            }
        }
        throw std::system_error(error, "failed to enable IPv4 UDP packet information");
    }
}

struct udp_packet_destination final {
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_;
};

[[nodiscard]] std::optional<udp_packet_destination> packet_destination(
    WSAMSG& message, const asio::ip::udp::endpoint& remote, std::uint16_t port) {
    if ((message.dwFlags & MSG_CTRUNC) != 0) {
        return std::nullopt;
    }
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr; header = WSA_CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
            IN_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
            asio::ip::address address{asio::ip::address_v4(bytes)};
            if (remote.address().is_v6()) {
                address = asio::ip::make_address_v6(asio::ip::v4_mapped, address.to_v4());
            }
            return udp_packet_destination{{address, port}, info.ipi_ifindex};
        }
        if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
            IN6_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
            asio::ip::address_v6 address(bytes);
            if (address.is_link_local() || address.is_multicast_link_local()) {
                address.scope_id(info.ipi6_ifindex);
            }
            return udp_packet_destination{{address, port}, info.ipi6_ifindex};
        }
    }
    // Never invent the destination from a wildcard socket's bind address.
    return std::nullopt;
}
#endif

[[nodiscard]] std::size_t acceptor_queue_capacity(std::size_t worker_count) {
#if defined(RUVIA_ENABLE_HTTP3)
    constexpr auto max_workers =
        ((std::numeric_limits<std::size_t>::max)() - minimum_queue_capacity) / http3_max_datagrams_in_flight;
    if (worker_count > max_workers) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return minimum_queue_capacity + worker_count * http3_max_datagrams_in_flight;
#else
    (void)worker_count;
    return minimum_queue_capacity;
#endif
}

}  // namespace

windows_accepted_socket::~windows_accepted_socket() {
    if (handle_ != INVALID_SOCKET) {
        (void)::closesocket(handle_);
    }
}

struct windows_acceptor::worker_slot final {
    business_worker target_;
#if defined(RUVIA_ENABLE_HTTP3)
    // Datagrams this worker queued for sending that the acceptor has not
    // sent yet.
    std::atomic<std::size_t> egress_in_flight_{0};
#endif
};

struct windows_acceptor::tcp_listener final {
    tcp_listener(asio::io_context& io, asio::ip::tcp::endpoint endpoint, std::size_t index,
        std::optional<std::size_t> http3_index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : socket_(io),
          endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          index_(index),
          http3_index_(http3_index),
          inherited_(std::move(inherited)) {}

    asio::ip::tcp::acceptor socket_;
    asio::ip::tcp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    // The listener's index in every business worker.
    std::size_t index_;
    // Its HTTP/3 service's index among udp_listeners_.
    std::optional<std::size_t> http3_index_;
    LPFN_ACCEPTEX accept_ex_{nullptr};
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // run_accepts() operations of this listener; a removal holds its
    // receipt until none is left.
    std::size_t accepts_{0};
    std::shared_ptr<void> removal_receipt_;
    std::optional<worker_signal> activated_;
    // From opening until removal or the acceptor's stop.
    bool admitting_{false};
};

#if defined(RUVIA_ENABLE_HTTP3)
struct windows_acceptor::udp_transport final {
    explicit udp_transport(asio::io_context& io)
        : socket_(io) {}

    asio::ip::udp::socket socket_;
    std::size_t listeners_{0};
    std::vector<std::byte> receive_buffer_{http3_max_udp_payload_bytes};
    bool receiving_{false};
    LPFN_WSARECVMSG receive_message_{nullptr};
    LPFN_WSASENDMSG send_message_{nullptr};
};

struct windows_acceptor::udp_listener final {
    udp_listener([[maybe_unused]] asio::io_context& io, asio::ip::udp::endpoint endpoint, std::shared_ptr<const http3_secret> secret,
        std::size_t index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          secret_(std::move(secret)),
          index_(index),
          inherited_(std::move(inherited)) {}

    ~udp_listener() {
        release_transport();
    }

    void release_transport() noexcept {
        if (transport_ != nullptr && !std::exchange(transport_released_, true) && --transport_->listeners_ == 0) {
            std::error_code ignored;
            transport_->socket_.close(ignored);
        }
    }

    std::shared_ptr<udp_transport> transport_;
    asio::ip::udp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    std::shared_ptr<const http3_secret> secret_;
    // The HTTP/3 endpoint's index in every business worker.
    std::size_t index_;
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // The last worker releases this logical listener's transport claim.
    // A re-added listener may still need the same exclusive UDP socket.
    std::size_t released_{0};
    bool transport_released_{false};
    std::optional<worker_signal> activated_;
    std::shared_ptr<void> removal_receipt_;
    bool receiving_{false};
};
#endif

windows_acceptor::windows_acceptor(std::span<const http_server_listener_definition> listeners,
    std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory,
#endif
    worker_failure_sink failure)
    : runtime_({.queue_capacity_ = acceptor_queue_capacity(worker_count),
          .io_policy_ = worker_io_policy::single_owner}),
      io_(runtime_.io_context()),
      failure_sink_(failure),
      finalize_signal_(runtime_.handle()),
      tasks_(runtime_.handle()),
      workers_(),
      tcp_listeners_()
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      directory_(directory),
      udp_listeners_()
#endif
{
    if (worker_count == 0) {
        throw std::invalid_argument("the Windows acceptor requires business workers");
    }
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.push_back(std::make_unique<worker_slot>());
    }
    tcp_listeners_.reserve(listeners.size());
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        const auto& definition = listeners[index];
        std::optional<std::size_t> http3_index;
#if defined(RUVIA_ENABLE_HTTP3)
        if (definition.http3_.has_value()) {
            if (directory_ == nullptr || definition.http3_->secret_ == nullptr) {
                throw std::logic_error("Windows HTTP/3 acceptor requires the worker directory and a secret");
            }
            http3_index = udp_listeners_.size();
            udp_listeners_.push_back(std::make_unique<udp_listener>(io_, asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_), definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_));
        }
#endif
        tcp_listeners_.push_back(std::make_unique<tcp_listener>(io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_));
    }
    runtime_.configure({
        .startup_ = [this] {
            // Startup runs before the context does; the task starts in its first turn.
            asio::post(io_, [this] {
                ruvia::async_start(run(),
                    asio::bind_executor(io_, [this](std::exception_ptr failure) noexcept {
                                                     if (failure) {
                                                         (void)runtime_.fail(failure);
                                                     }
                                                 }));
            }); },
        .serve_ = [this] { serve_on_context(); complete_phase(false); },
        .stop_admission_ = [this] {
            stopping_ = true;
            close_admission();
#if defined(RUVIA_ENABLE_HTTP3)
            for (auto& listener : udp_listeners_) {
                if (listener != nullptr && listener->transport_ != nullptr) {
                    std::error_code ignored;
                    listener->transport_->socket_.close(ignored);
                }
            }
#endif
            runtime_.finalize(); },
        .finalize_ = [this] { finalize_signal_.notify(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            complete_phase(true, failure);
            complete_phase(false, failure);
            failure_sink_.notify(failure); },
        .shutdown_ = [this] {
            try { throw std::logic_error("HTTP server acceptor stopped before its phase completed"); }
            catch (...) {
                const auto failure = std::current_exception();
                complete_phase(true, failure);
                complete_phase(false, failure);
            } },
    });
}

windows_acceptor::~windows_acceptor() {
    runtime_.request_stop();
    runtime_.finalize();
    runtime_.join();
}

void windows_acceptor::bind_worker(std::size_t index, business_worker target) {
    if (index >= workers_.size() || !target.worker_.valid() || target.load_ == nullptr || target.target_ == nullptr ||
        target.adopt_ == nullptr) {
        throw std::invalid_argument("invalid Windows acceptor worker");
    }
    workers_[index]->target_ = target;
}

bool windows_acceptor::start() {
    for (const auto& slot : workers_) {
        if (slot->target_.adopt_ == nullptr) {
            throw std::logic_error("Windows acceptor worker was not bound");
        }
    }
    return runtime_.start() && runtime_.wait_until_ready();
}

bool windows_acceptor::serve() {
    runtime_.request_serve();
    return runtime_.wait_until_serving();
}

void windows_acceptor::complete_phase(bool startup, std::exception_ptr failure) noexcept {
    std::optional<runtime_control_completion::receipt> receipt;
    {
        std::lock_guard lock(phase_mutex_);
        auto& pending = startup ? startup_receipt_ : serve_receipt_;
        receipt.swap(pending);
    }
    if (receipt) {
        receipt->complete(std::move(failure));
    }
}

task<void> windows_acceptor::start(worker_ref caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (startup_receipt_) {
            throw std::logic_error("HTTP server acceptor is already starting");
        }
        startup_receipt_.emplace(completion.make_receipt());
    }
    try {
        for (const auto& slot : workers_) {
            if (!slot->target_.adopt_) {
                throw std::logic_error("Windows acceptor worker was not bound");
            }
        }
        if (!runtime_.start()) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(true, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::serve(worker_ref caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (serve_receipt_) {
            throw std::logic_error("HTTP server acceptor is already opening");
        }
        serve_receipt_.emplace(completion.make_receipt());
    }
    try {
        runtime_.request_serve();
        if (runtime_.state() != worker_runtime_state::running) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(false, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::join(worker_ref caller, asio::io_context& caller_io) {
    if (!caller.is_current()) {
        throw std::logic_error("acceptor join must run on its caller worker");
    }
    const auto native = co_await runtime_.prepare_thread_join(caller);
    if (native != 0) {
        std::unique_ptr<void, decltype(&::CloseHandle)> native_owner(reinterpret_cast<HANDLE>(native), &::CloseHandle);
        asio::windows::object_handle thread(caller_io);
        std::error_code error;
        thread.assign(native_owner.get(), error);
        if (error) {
            throw std::system_error(error, "failed to assign acceptor thread wait handle");
        }
        (void)native_owner.release();
        const auto waited = co_await ruvia::async_asio([&thread](auto handler) { thread.async_wait(std::move(handler)); });
        if (waited.failed()) {
            throw std::system_error(waited.error_code(), "failed to await acceptor thread exit");
        }
    }
    runtime_.join();
}

void windows_acceptor::stop_admission() noexcept {
    // A refused control means the acceptor is already stopping, which closes
    // admission itself.
    (void)runtime_.post_control([this] { close_admission(); });
}

void windows_acceptor::request_stop() noexcept {
    runtime_.request_stop();
}

void windows_acceptor::join() {
    runtime_.join();
}

std::exception_ptr windows_acceptor::failure() noexcept {
    return runtime_.take_failure();
}

task<void> windows_acceptor::run() {
    try {
        // A stop applied before this task first runs binds nothing.
        if (!stopping_) {
            open_listeners();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
            complete_phase(true);
        }
    } catch (...) {
        if (!stopping_) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    co_await finalize_signal_.wait();
    try {
        co_await tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void windows_acceptor::open_listeners() {
    for (auto& listener : tcp_listeners_) {
        open_tcp_listener(*listener);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    for (auto& listener : udp_listeners_) {
        open_udp_listener(*listener);
    }
#endif
    for (auto& listener : tcp_listeners_) {
        prepare_listener_tasks(*listener);
    }
}

void windows_acceptor::open_tcp_listener(tcp_listener& listener) {
    auto& socket = listener.socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_tcp(0) : invalid_native_socket;
    listener.inherited_.reset();
    if (inherited != invalid_native_socket) {
        // Checked to listen on this endpoint when it was handed over; its
        // first association with an I/O completion port is this one.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited), error);
        if (error) {
            close_native_socket(inherited);
            throw std::system_error(error, "failed to adopt an inherited TCP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
        socket.listen(asio::socket_base::max_listen_connections);
    }
    listener.accept_ex_ = resolve_accept_ex(socket.native_handle());
}

#if defined(RUVIA_ENABLE_HTTP3)
void windows_acceptor::open_udp_listener(udp_listener& listener) {
    if (listener.inherited_ == nullptr || listener.inherited_->udp().empty()) {
        for (const auto& existing : udp_listeners_) {
            if (existing != nullptr && existing->endpoint_ == listener.endpoint_ &&
                existing->transport_ != nullptr && existing->transport_->socket_.is_open()) {
                if (listener.ipv6_only_ && existing->transport_->socket_.local_endpoint().address().is_v6()) {
                    asio::ip::v6_only value;
                    existing->transport_->socket_.get_option(value);
                    if (value.value() != *listener.ipv6_only_) {
                        throw std::invalid_argument("shared HTTP/3 socket IPV6_V6ONLY does not match");
                    }
                }
                listener.transport_ = existing->transport_;
                ++listener.transport_->listeners_;
                listener.inherited_.reset();
                return;
            }
        }
    }
    auto transport = std::make_shared<udp_transport>(io_);
    auto& socket = transport->socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_udp()
                                                          : std::vector<native_socket>();
    listener.inherited_.reset();
    if (!inherited.empty()) {
        // listen() admitted exactly one, bound to this endpoint.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited.front()), error);
        if (error) {
            close_native_socket(inherited.front());
            throw std::system_error(error, "failed to adopt an inherited UDP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
    }
    enable_udp_packet_info(socket);
    transport->receive_message_ = resolve_message_extension<LPFN_WSARECVMSG>(socket.native_handle(), WSAID_WSARECVMSG);
    transport->send_message_ = resolve_message_extension<LPFN_WSASENDMSG>(socket.native_handle(), WSAID_WSASENDMSG);
    // Sending runs on this thread for every worker: a full send buffer
    // loses the datagram instead of blocking the acceptor.
    socket.non_blocking(true);
    ++transport->listeners_;
    listener.transport_ = std::move(transport);
}
#endif

void windows_acceptor::serve_on_context() {
    if (stopping_) {
        return;
    }
    admitting_ = true;
    for (auto& listener : tcp_listeners_) {
        start_listener(*listener);
    }
}

void windows_acceptor::prepare_listener_tasks(tcp_listener& listener) {
    listener.activated_.emplace(runtime_.handle());
    for (std::size_t operation = 0; operation < accepts_per_listener; ++operation) {
        tasks_.spawn(run_accepts(listener));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_.has_value()) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (!udp.transport_->receiving_) {
            udp.activated_.emplace(runtime_.handle());
            tasks_.spawn(run_receives(udp));
            udp.transport_->receiving_ = true;
            udp.receiving_ = true;
        }
    }
#endif
}

void windows_acceptor::start_listener(tcp_listener& listener) noexcept {
    listener.admitting_ = true;
    listener.activated_->notify();
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    if (stopping_) {
        throw std::logic_error("the Windows acceptor is stopping");
    }
    if (tcp_listeners_.size() <= index) {
        tcp_listeners_.resize(index + 1);
    }
    auto tcp = std::make_unique<tcp_listener>(io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_);
#if defined(RUVIA_ENABLE_HTTP3)
    std::unique_ptr<udp_listener> udp(
        nullptr, pmr_object_deleter<udp_listener>{});
    if (http3_index.has_value()) {
        if (udp_listeners_.size() <= *http3_index) {
            udp_listeners_.resize(*http3_index + 1);
        }
        // The application creates the directory with its first HTTP/3 listener.
        if (directory_ == nullptr) {
            directory_ = directory;
        }
        udp = std::make_unique<udp_listener>(io_, asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_), definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_);
    }
#endif
    open_tcp_listener(*tcp);
#if defined(RUVIA_ENABLE_HTTP3)
    if (udp != nullptr) {
        open_udp_listener(*udp);
        udp_listeners_[*http3_index] = std::move(udp);
    }
#endif
    tcp_listeners_[index] = std::move(tcp);
    try {
        prepare_listener_tasks(*tcp_listeners_[index]);
    } catch (...) {
        discard_listener(index);
        throw;
    }
}

void windows_acceptor::open_listener(std::size_t index) noexcept {
    if (!admitting_ || index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    start_listener(*tcp_listeners_[index]);
}

void windows_acceptor::discard_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    remove_listener(index, receipt);
#if defined(RUVIA_ENABLE_HTTP3)
    if (const auto http3_index = tcp_listeners_[index]->http3_index_; http3_index.has_value()) {
        auto& udp = *udp_listeners_[*http3_index];
        if (udp.receiving_) {
            udp.removal_receipt_ = std::move(receipt);
        }
        udp.release_transport();
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    auto& listener = *tcp_listeners_[index];
    listener.admitting_ = false;
    if (listener.activated_) {
        listener.activated_->notify();
    }
    // Completes its pending AcceptEx operations as aborted.
    std::error_code ignored;
    listener.socket_.close(ignored);
    if (listener.accepts_ != 0) {
        listener.removal_receipt_ = std::move(receipt);
    }
}

void windows_acceptor::close_admission() noexcept {
    admitting_ = false;
    // Closing a listener completes its pending AcceptEx operations as aborted.
    for (auto& listener : tcp_listeners_) {
        if (listener != nullptr) {
            listener->admitting_ = false;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            std::error_code ignored;
            listener->socket_.close(ignored);
        }
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // The sockets stay open for sending until request_stop().
    for (auto& listener : udp_listeners_) {
        if (listener != nullptr && listener->transport_ != nullptr) {
            std::error_code ignored;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            listener->transport_->socket_.cancel(ignored);
        }
    }
#endif
}

task<void> windows_acceptor::run_accepts(tcp_listener& listener) {
    ++listener.accepts_;
    co_await listener.activated_->wait();
    // AcceptEx writes both addresses here; the frame outlives the operation.
    std::array<std::byte, 2 * accept_address_bytes> addresses{};
    const auto protocol = listener.endpoint_.protocol();
    try {
        while (admitting_ && listener.admitting_) {
            // Not associated with any completion port: the business worker
            // that adopts it binds it to its own.
            windows_accepted_socket accepted(::WSASocketW(protocol.family(), protocol.type(), protocol.protocol(),
                nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
            std::error_code error;
            if (accepted.get() == INVALID_SOCKET) {
                error = last_socket_error();
            } else {
                const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::windows::overlapped_ptr native(io_, std::move(handler));
                    DWORD bytes = 0;
                    const BOOL accepted_now = listener.accept_ex_(listener.socket_.native_handle(), accepted.get(),
                        addresses.data(), 0, accept_address_bytes, accept_address_bytes, &bytes, native.get());
                    const int accept_error = accepted_now ? 0 : ::WSAGetLastError();
                    if (accepted_now || accept_error == ERROR_IO_PENDING) {
                        // Even a synchronous success posts its completion packet.
                        (void)native.release();
                    } else {
                        native.complete(asio::error_code(accept_error, asio::system_category()), 0);
                    }
                });
                error = completion.error_code();
            }
            // A stop, or this listener's removal, aborted the accept.
            if (!admitting_ || !listener.admitting_) {
                break;
            }
            if (!error) {
                // Gives the socket the listener's properties (getpeername,
                // shutdown); no completion port is involved.
                const SOCKET listening = listener.socket_.native_handle();
                if (::setsockopt(accepted.get(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<const char*>(&listening), static_cast<int>(sizeof(listening))) == SOCKET_ERROR) {
                    error = last_socket_error();
                } else {
                    dispatch(listener.index_, std::move(accepted));
                    continue;
                }
            }
            report_accept_error(error);
            if (co_await ruvia::sleep_for(runtime_.handle(), accept_retry_delay) == timer_sleep_result::stop_requested) {
                break;
            }
        }
    } catch (...) {
        // A fatal listener failure stops the application.
        (void)runtime_.fail(std::current_exception());
    }
    // A removal waits until every accept of the listener ended.
    if (--listener.accepts_ == 0) {
        listener.removal_receipt_.reset();
    }
}

void windows_acceptor::report_accept_error(const std::error_code& error) {
    const auto action = classify_accept_error(error);
    if (action == accept_error_action::retry) {
        return;
    }
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (action == accept_error_action::fail) {
        throw std::system_error(error, "fatal TCP AcceptEx error");
    }
    try {
        throw std::system_error(error, "recoverable TCP AcceptEx error");
    } catch (...) {
        ruvia::report_unhandled_failure("web accept", std::current_exception());
    }
}

void windows_acceptor::dispatch(std::size_t listener_index, windows_accepted_socket accepted) {
    const auto& target = workers_[select_worker()]->target_;
    // Admission is reserved before the callable takes the socket; a refused
    // socket stays here and closes with `accepted`.
    const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
        return [adopt = target.adopt_, owner = target.target_, listener_index, owned = std::move(accepted)]() mutable {
            adopt(owner, listener_index, std::move(owned));
        };
    });
    if (status == post_status::queue_full) {
        refused_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t windows_acceptor::select_worker() noexcept {
    // The worker holding the fewest connections; scanning starts after the
    // last choice, so ties (a burst) rotate across workers.
    const auto count = workers_.size();
    auto chosen = next_worker_;
    auto least = workers_[chosen]->target_.load_->active();
    for (std::size_t step = 1; step < count; ++step) {
        const auto index = (next_worker_ + step) % count;
        const auto load = workers_[index]->target_.load_->active();
        if (load < least) {
            least = load;
            chosen = index;
        }
    }
    next_worker_ = (chosen + 1) % count;
    return chosen;
}

#if defined(RUVIA_ENABLE_HTTP3)
task<void> windows_acceptor::run_receives(udp_listener& listener) {
    // A removed logical listener can release its claim while another listener
    // at this address continues receiving on the same exclusive socket.
    const auto transport = listener.transport_;
    auto& socket = transport->socket_;
    auto& buffer = transport->receive_buffer_;
    struct receipt_release final {
        udp_listener& listener_;
        ~receipt_release() {
            listener_.receiving_ = false;
            listener_.removal_receipt_.reset();
        }
    } release{listener};
    co_await listener.activated_->wait();
    try {
        while (admitting_ && socket.is_open()) {
            asio::ip::udp::endpoint remote;
            alignas(WSACMSGHDR) std::array<char,
                WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(IN_PKTINFO))>
                control{};
            WSABUF payload{};
            payload.buf = reinterpret_cast<char*>(buffer.data());
            payload.len = static_cast<ULONG>(buffer.size());
            WSAMSG message{};
            message.name = remote.data();
            message.namelen = static_cast<int>(remote.capacity());
            message.lpBuffers = &payload;
            message.dwBufferCount = 1;
            message.Control.buf = control.data();
            message.Control.len = static_cast<ULONG>(control.size());
            const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::windows::overlapped_ptr native(io_, std::move(handler));
                DWORD received = 0;
                const int result = transport->receive_message_(
                    socket.native_handle(), &message, &received, native.get(), nullptr);
                const int error = result == 0 ? 0 : ::WSAGetLastError();
                if (result == 0 || error == WSA_IO_PENDING) {
                    (void)native.release();
                } else {
                    native.complete(asio::error_code(error, asio::system_category()), 0);
                }
            });
            // A stop, or the last worker's release of a removed listener.
            if (!admitting_ || !socket.is_open()) {
                co_return;
            }
            if (const auto error = completion.error_code()) {
                if (http3_ignorable_receive_error(error)) {
                    continue;
                }
                throw std::system_error(error, "HTTP/3 UDP receive failed");
            }
            remote.resize(static_cast<std::size_t>(message.namelen));
            const auto destination = packet_destination(message, remote, listener.endpoint_.port());
            if (destination) {
                steer(listener, std::span<const std::byte>(buffer.data(), completion.result()),
                    remote, destination->local_, destination->interface_index_);
            }
        }
    } catch (...) {
        // The listener cannot receive any more: its connections time out and
        // the failure is reported once.
        ruvia::report_unhandled_failure("HTTP/3 receive", std::current_exception());
    }
}

void windows_acceptor::steer(
    const udp_listener& listener, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(
        &header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), http3_connection_id_length);
    if (decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    std::optional<std::size_t> owner;
    std::size_t listener_index = listener.index_;
    const udp_listener* initial_listener = &listener;
    for (const auto& candidate : udp_listeners_) {
        if (candidate != nullptr && candidate->transport_ == listener.transport_) {
            if (!candidate->transport_released_) {
                initial_listener = candidate.get();
            }
            if (const auto identified = http3_connection_id_owner(*candidate->secret_, destination, workers_.size())) {
                owner = identified;
                listener_index = candidate->index_;
                break;
            }
        }
    }
    if (!owner.has_value()) {
        // Only a long header can open a connection or earn Version
        // Negotiation; a short header naming no worker is dropped.
        if (header.version == 0) {
            return;
        }
        listener_index = initial_listener->index_;
        owner = http3_initial_worker(*initial_listener->secret_, destination, workers_.size());
    }
    (void)directory_->hand_off(*owner, listener_index, bytes, remote, local, interface_index);
}

bool windows_acceptor::transmit(std::size_t worker_index, std::size_t http3_listener_index,
    const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    auto token = http3_in_flight_token::reserve(workers_[worker_index]->egress_in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = runtime_.handle().post_factory([&]() -> move_only_function<void()> {
            std::vector<std::byte> owned(bytes.begin(), bytes.end());
            return [this, http3_listener_index, remote, local, interface_index, owned = std::move(owned),
                       token = std::move(token)]() mutable noexcept {
                send_datagram(http3_listener_index, remote, owned, local, interface_index);
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

void windows_acceptor::send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local,
    std::uint32_t interface_index) noexcept {
    // Errors (a full send buffer, a closed socket, an unreachable peer) lose
    // the datagram, as the network may; QUIC recovery handles the peer.
    if (const auto& listener = udp_listeners_[http3_listener_index]; listener != nullptr && listener->transport_ != nullptr) {
        auto& transport = *listener->transport_;
        if (!transport.socket_.is_open() || local.address().is_unspecified()) {
            return;
        }
        auto destination = remote;
        WSABUF payload{};
        payload.buf = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        payload.len = static_cast<ULONG>(bytes.size());
        alignas(WSACMSGHDR) std::array<char, WSA_CMSG_SPACE(sizeof(IN6_PKTINFO))> control{};
        WSAMSG message{};
        message.name = destination.data();
        message.namelen = static_cast<int>(destination.size());
        message.lpBuffers = &payload;
        message.dwBufferCount = 1;
        message.Control.buf = control.data();
        const auto source = local.address();
        const bool ipv4 = remote.address().is_v4() ||
                          (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped());
        auto* header = reinterpret_cast<WSACMSGHDR*>(control.data());
        if (ipv4) {
            asio::ip::address_v4::bytes_type address{};
            if (source.is_v4()) {
                address = source.to_v4().to_bytes();
            } else if (source.to_v6().is_v4_mapped()) {
                const auto mapped = source.to_v6().to_bytes();
                std::memcpy(address.data(), mapped.data() + 12, address.size());
            } else {
                return;
            }
            IN_PKTINFO info{};
            std::memcpy(&info.ipi_addr, address.data(), address.size());
            info.ipi_ifindex = interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IP;
            header->cmsg_type = IP_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        } else {
            if (!source.is_v6()) {
                return;
            }
            IN6_PKTINFO info{};
            const auto address = source.to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, address.data(), address.size());
            info.ipi6_ifindex = interface_index == 0
                                    ? static_cast<ULONG>(source.to_v6().scope_id())
                                    : interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IPV6;
            header->cmsg_type = IPV6_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        }
        DWORD sent = 0;
        (void)transport.send_message_(transport.socket_.native_handle(), &message, 0, &sent, nullptr, nullptr);
    }
}

void windows_acceptor::release_http3_listener(std::size_t http3_listener_index) noexcept {
    // A refused control means the acceptor is stopping, which closes every
    // socket itself.
    (void)runtime_.post_control([this, http3_listener_index] {
        auto& listener = *udp_listeners_[http3_listener_index];
        if (++listener.released_ == workers_.size()) {
            listener.release_transport();
        }
    });
}
#endif

}  // namespace ruvia::detail

#endif

namespace ruvia::detail {
namespace {

// Reclaim read storage beyond the header limit once the pending bytes fit the
// initial size. Amortized growth and handed-over pipelines can both exceed it.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Deferred access-log entries retained across coalesced writes.
constexpr std::size_t retained_deferred_access_entries = 64;

}  // namespace

// Reserve the initial capacity before exposing the active work set.
connection_read_buffer::connection_read_buffer() noexcept {
    storage_.reserve(initial_read_buffer_bytes);
}

std::span<char> connection_read_buffer::prepare_read() & {
    if (storage_.size() >= max_http_header_bytes) {
        return {};
    }
    const auto remaining = max_http_header_bytes - storage_.size();
    auto space = storage_.prepare_spare();
    if (space.empty()) {
        // Storage may outgrow the protocol window to amortize a short consumed
        // prefix; only the bytes still allowed in that window are writable.
        (void)storage_.prepare(std::min(initial_read_buffer_bytes, remaining));
        space = storage_.prepare_spare();
    }
    return space.first(std::min(space.size(), remaining));
}

void connection_read_buffer::consume(std::size_t bytes) noexcept {
    if (bytes > storage_.size()) {
        std::terminate();
    }
    storage_.consume(bytes);
}

void connection_read_buffer::assign(std::string_view bytes) {
    storage_.clear();
    storage_.append(bytes);
}

void connection_read_buffer::complete_request(const http1_request_buffer_completion& completion) {
    if (const auto* compaction = completion.compaction()) {
        consume(compaction->consumed_bytes());
        return;
    }
    if (const auto* restore = completion.pipeline_restore()) {
        assign(restore->pipeline());
        return;
    }
    // A discarded buffer is valid only when the connection plan closes. The
    // session must never reach reusable cleanup with that alternative.
    std::terminate();
}

void connection_read_buffer::trim() {
    if (storage_.capacity() > read_buffer_shrink_capacity_bytes && storage_.size() <= initial_read_buffer_bytes) {
        storage_.shrink_to(initial_read_buffer_bytes);
    }
}

http1_deferred_access_log::http1_deferred_access_log() noexcept = default;

void http1_deferred_access_log::defer(std::size_t head_end, const http_request& request,
    ip_address client_address, std::chrono::steady_clock::time_point start,
    http_status_code status, const request_io_metrics& io_metrics) {
    const auto method = request.method();
    const auto path = request.path();
    const auto text_begin = text_.size();
    const auto required_text = text_begin + method.size() + path.size();
    if (required_text > text_.capacity()) {
        text_.reserve(
            std::max<std::size_t>(required_text, std::max<std::size_t>(256, text_.capacity() * 2)));
    }
    text_.append(method);
    text_.append(path);
    if (entries_.size() == entries_.capacity()) {
        entries_.reserve(std::max<std::size_t>(8, entries_.capacity() * 2));
    }
    entries_.push_back({
        .head_end_ = head_end,
        .text_begin_ = text_begin,
        .method_bytes_ = method.size(),
        .path_bytes_ = path.size(),
        .client_address_ = client_address,
        .start_ = start,
        .io_metrics_ = io_metrics,
        .known_method_ = request.known_method(),
        .protocol_version_ = request.protocol_version(),
        .status_ = status,
    });
}

void http1_deferred_access_log::complete(
    const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept {
    const std::string_view text = text_;
    for (const auto& entry : entries_) {
        if (entry.head_end_ > written) {
            break;
        }
        const auto fields = text.substr(entry.text_begin_);
        access_log.invoke(access_log_record_access::make({
            .method_ = fields.substr(0, entry.method_bytes_),
            .known_method_ = entry.known_method_,
            .path_ = fields.substr(entry.method_bytes_, entry.path_bytes_),
            .protocol_version_ = entry.protocol_version_,
            .remote_address_ = remote_address,
            .client_address_ = entry.client_address_,
            .status_ = entry.status_,
            .duration_micros_ = access_log_record_access::duration_since(entry.start_),
            .db_count_ = entry.io_metrics_.db_count_,
            .db_duration_micros_ = entry.io_metrics_.db_duration_micros_,
            .redis_count_ = entry.io_metrics_.redis_count_,
            .redis_duration_micros_ = entry.io_metrics_.redis_duration_micros_,
        }));
    }
    reset();
}

void http1_deferred_access_log::reset() noexcept {
    entries_.clear();
    if (entries_.capacity() > retained_deferred_access_entries) {
        std::vector<entry_type>(entries_.get_allocator()).swap(entries_);
    }
    http_clear_string_retaining_small(text_);
}

connection_work_set::connection_work_set() noexcept = default;

std::unique_ptr<connection_work_set> make_connection_work_set() {
    return std::make_unique<connection_work_set>();
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// Total time allowed for a graceful close: flushing TLS close_notify and
// lingering for the peer's end of input together.
constexpr auto graceful_close_time = std::chrono::seconds(2);
// Peer input a lingering close discards before it stops waiting for the end.
constexpr std::size_t linger_input_bytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t linger_read_bytes = 4096;

// Keep the closing transport reachable by stop and bound the whole sequence.
// Expiry closes the socket, which ends a blocked write or lingering read; the
// owner closes TCP after the sequence completes either way.
class graceful_close_bounds final {
public:
    graceful_close_bounds(asio::ip::tcp::socket& socket, connection_scanner& scanner,
        const worker_ref& worker_value)
        : guard_(scanner, entry_, connection_transport::tcp(socket)) {
        entry_.set_phase(connection_scanner::phase::writing);
        armed_ = worker_value.schedule_timer(timer_, deadline_after(graceful_close_time),
                     [target = &socket](worker_timer_outcome outcome) {
                         if (outcome == worker_timer_outcome::expired) {
                             close_socket(*target);
                         }
                     }) == worker_timer_schedule_status::scheduled;
    }

    graceful_close_bounds(const graceful_close_bounds&) = delete;
    graceful_close_bounds& operator=(const graceful_close_bounds&) = delete;

    // False when the worker timer queue is already stopping; the caller then
    // closes immediately instead of running an unbounded close.
    [[nodiscard]] bool armed() const noexcept {
        return armed_;
    }

private:
    connection_scanner::entry entry_;
    connection_scanner::guard guard_;
    worker_timer_registration timer_;
    bool armed_{false};
};

// RFC 9112 §9.6: half-close, then read until the peer's end of input. Any
// error -- including the bounds closing the socket -- ends the wait.
task<void> close_gracefully(asio::ip::tcp::socket& socket, server_tls_stream* tls,
    http_connection_close mode, connection_scanner& scanner, const worker_ref& worker_value) {
    const graceful_close_bounds bounds(socket, scanner, worker_value);
    if (!bounds.armed() || !socket.is_open()) {
        co_return;
    }
    if (tls != nullptr) {
        // RFC 9846 §6.1: send our alert, without waiting for the peer's.
        // Recording its alert as received makes SSL_shutdown finish after our
        // write; this terminal session never reads the TLS stream afterward.
        SSL* const ssl = tls->native_handle();
        const int state = SSL_get_shutdown(ssl);
        if ((state & SSL_SENT_SHUTDOWN) == 0) {
            SSL_set_shutdown(ssl, state | SSL_RECEIVED_SHUTDOWN);
            const auto completion = co_await async_asio([tls](auto handler) {
                tls->async_shutdown(std::move(handler));
            });
            if (completion.error_code()) {
                co_return;
            }
        }
    }
    if (mode != http_connection_close::linger) {
        co_return;
    }
    shutdown_http_connection_send(socket);
    // Remaining input is discarded below TLS: the session never decrypts again.
    std::array<char, linger_read_bytes> discarded;
    for (std::size_t total = 0; total < linger_input_bytes;) {
        const auto completion = co_await async_asio<std::size_t>([&socket, &discarded](auto handler) {
            socket.async_read_some(asio::buffer(discarded), std::move(handler));
        });
        if (completion.error_code()) {
            co_return;
        }
        total += completion.result();
    }
}

}  // namespace

task<void> linger_http_connection(
    asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_ref& worker_value) {
    return close_gracefully(socket, nullptr, http_connection_close::linger, scanner, worker_value);
}

task<void> close_http_connection_gracefully(server_tls_stream& stream, http_connection_close mode,
    connection_scanner& scanner, const worker_ref& worker_value) {
    return close_gracefully(stream.lowest_layer(), &stream, mode, scanner, worker_value);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// A file removed, replaced or rewritten since its response was framed: a
// deployment race the client sees as an error response, not a server fault.
[[nodiscard]] bool is_file_changed_since_framing(std::error_code error) noexcept {
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
           error == std::errc::state_not_recoverable;
}

}  // namespace

response_file_failure response_file_failure::read_failed(std::error_code error) {
    if (is_file_changed_since_framing(error)) {
        return response_file_failure(false, nullptr);
    }
    return response_file_failure(false, std::make_exception_ptr(std::system_error(error, "file response body read failed")));
}

response_file_failure response_file_failure::read_not_completed(blocking_status status) {
    // A worker without a pool is a configuration fault, and an open or read
    // still unfinished after the write timeout is a stalled filesystem.
    // Overload and shutdown are not faults of this response.
    std::exception_ptr fault;
    if (status == blocking_status::unavailable) {
        fault = std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::operation_not_supported),
            "file response needs a blocking pool"));
    } else if (status == blocking_status::timed_out) {
        fault = std::make_exception_ptr(
            std::system_error(std::make_error_code(std::errc::timed_out), "file response I/O timed out"));
    }
    return response_file_failure(true, std::move(fault));
}

response_file_failure response_file_failure::read_threw(std::exception_ptr exception) noexcept {
    return response_file_failure(false, std::move(exception));
}

std::exception_ptr response_file_failure::response_error() const {
    if (unavailable_) {
        return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::service_unavailable,
            .code_ = "file_read_unavailable",
            .message_ = "file could not be read in time"}));
    }
    return std::make_exception_ptr(http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "file_read_failed",
        .message_ = "file could not be read"}));
}

char* response_file_source::read_buffer::data() & {
    if (data_ == nullptr) {
        data_.reset(static_cast<char*>(
            ::operator new(response_file_read_ahead_bytes, std::align_val_t{alignof(std::max_align_t)})));
    }
    return data_.get();
}

void response_file_source::read_buffer::reset() noexcept {
    data_.reset();
}

void response_file_source::read_buffer::buffer_deleter::operator()(char* bytes) const noexcept {
    ::operator delete(bytes, std::align_val_t{alignof(std::max_align_t)});
}

// The path travels with the state to the pool thread, so it lives in the
// synchronized process resource.
response_file_source::read_state::read_state() = default;

response_file_source::response_file_source(const http_response& response, response_file_io io)
    : response_(response),
      io_(std::move(io)),
      segment_count_(response.body_segment_count()) {}

void response_file_source::select_file(http_response_file_view file) {
    // Multipart byte ranges name the same file view: keep its validated
    // handle and only reposition.
    if (current_path_ != file.native_path_c_str() || state_.identity_ != file.identity() ||
        state_.size_ != file.size()) {
        current_path_ = file.native_path_c_str();
        state_.path_.assign(file.native_path_c_str());
        state_.size_ = file.size();
        state_.identity_ = file.identity();
        state_.validated_ = false;
        state_.input_.reset();
        if (const auto* opened = file.opened_file()) {
            // An independent handle travels with the read state: a cancelled
            // response may be destroyed before a late read finishes.
            state_.input_ = duplicate_native_file(*opened, state_.error_);
        }
    }
    state_.position_ = file.offset();
}

void response_file_source::read_ahead(read_state& state, std::size_t bytes, bool completes_range) {
    if (!state.validated_) {
        if (!state.input_) {
            state.input_ = open_native_file(state.path_.c_str(), state.error_);
            if (state.error_) {
                return;
            }
        }
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
        if (state.error_) {
            return;
        }
        state.validated_ = true;
    }
    const auto remaining = bytes - state.filled_;
    if (remaining != 0) {
        state.error_ =
            read_native_file_at(state.input_.get(), state.position_, state.buffer_.data() + state.filled_, remaining);
        if (state.error_) {
            return;
        }
    }
    state.filled_ = bytes;
    state.position_ += remaining;
    if (completes_range) {
        state.error_ = validate_native_file(state.input_.get(), state.identity_, state.size_);
    }
}

bool response_file_source::settle_read() {
    if (state_.error_ && !failure_) {
        failure_.emplace(response_file_failure::read_failed(state_.error_));
    }
    return !failure_;
}

task<bool> response_file_source::fill(std::size_t bytes, bool completes_range) {
    if (!settle_read()) {
        co_return false;
    }
    state_.filled_ = 0;
#if defined(__linux__)
    // A validated handle first takes what the page cache already holds; only
    // a remainder waits on the pool.
    if (bytes != 0 && state_.validated_) {
        state_.filled_ = read_native_file_cached(
            state_.input_.get(), state_.position_, state_.buffer_.data(), bytes, state_.cached_read_supported_);
        state_.position_ += state_.filled_;
        if (state_.filled_ == bytes) {
            if (completes_range) {
                state_.error_ = validate_native_file(state_.input_.get(), state_.identity_, state_.size_);
            }
            co_return settle_read();
        }
    }
#endif
    auto result = co_await run_blocking(io_.worker_, {.timeout_ = io_.timeout_, .stop_token_ = io_.stop_},
        [state = std::move(state_), bytes, completes_range]() mutable {
            read_ahead(state, bytes, completes_range);
            return std::move(state);
        });
    if (result.status() != blocking_status::completed) {
        // The state (handle and buffer) stayed with the abandoned operation.
        failure_.emplace(response_file_failure::read_not_completed(result.status()));
        co_return false;
    }
    if (result.failed()) {
        // A throwing read retired its owned state on the pool thread.
        failure_.emplace(response_file_failure::read_threw(result.error()));
        co_return false;
    }
    state_ = std::move(result).take_value();
    co_return settle_read();
}

task<bool> response_file_source::open_first() {
    if (first_file_opened_) {
        co_return !failure_;
    }
    first_file_opened_ = true;
    for (auto index = next_segment_; index < segment_count_; ++index) {
        if (const auto segment = response_.body_segment(index); segment.file_) {
            select_file(*segment.file_);
            co_return co_await fill(0, segment.file_->length() == 0);
        }
    }
    co_return true;
}

task<response_body_chunk> response_file_source::next(std::size_t max_bytes) {
    // A body that opens with in-memory bytes (a multipart boundary) still has
    // its first file range validated before anything is yielded. A body that
    // opens with a file validates it on the read that yields its first chunk.
    if (!first_file_opened_ && next_segment_ < segment_count_ && !response_.body_segment(next_segment_).file_ &&
        !(co_await open_first())) {
        co_return response_body_chunk::failed();
    }
    first_file_opened_ = true;
    for (;;) {
        if (range_remaining_ != 0) {
            const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                std::min(max_bytes, response_file_read_ahead_bytes), range_remaining_));
            if (!(co_await fill(bytes, bytes == range_remaining_))) {
                co_return response_body_chunk::failed();
            }
            range_remaining_ -= bytes;
            co_return response_body_chunk::data(std::string_view(state_.buffer_.data(), bytes));
        }
        if (next_segment_ >= segment_count_) {
            co_return response_body_chunk::end();
        }
        const auto segment = response_.body_segment(next_segment_++);
        if (segment.file_) {
            select_file(*segment.file_);
            range_remaining_ = segment.file_->length();
            // Empty files still complete a range: validate their snapshot
            // through the same owned I/O state before releasing the head.
            if (range_remaining_ == 0 && !(co_await fill(0, true))) {
                co_return response_body_chunk::failed();
            }
            continue;
        }
        if (!segment.bytes_.empty()) {
            co_return response_body_chunk::data(segment.bytes_);
        }
    }
}

void response_file_source::release_buffer() noexcept {
    state_.buffer_.reset();
}

bool response_file_source::file_bytes_pending() const noexcept {
    if (range_remaining_ != 0) {
        return true;
    }
    // next() validates an empty file range without reading it and skips
    // empty in-memory segments.
    for (auto index = next_segment_; index < segment_count_; ++index) {
        const auto segment = response_.body_segment(index);
        if (segment.file_) {
            if (segment.file_->length() != 0) {
                return true;
            }
        } else if (!segment.bytes_.empty()) {
            return false;
        }
    }
    return false;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The byte stream behind one server_connection: the accepted TCP socket, or
// the TLS stream whose writes leave through `tls_output`. Reads land in one
// buffer charged to the connection's inbound budget and reused for every
// read; each read and write that completes renews the scanner entry's
// activity, so idle_timeout_ measures time without I/O.
template <typename stream_type>
class server_connection_session final {
public:
    server_connection_session(stream_type& stream, tls_tunnel_output* tls_output, connection_scanner::entry& entry,
        inbound_buffer_account& inbound, const worker_ref& worker_value, const stop_token& stop_token_value,
        const server_connection_fields& fields)
        : transport_(stream, tls_output, &entry),
          entry_(entry),
          lifecycle_(fields.lifecycle_),
          worker_(worker_value),
          read_reservation_(&inbound),
          facade_(fields, stop_token_value, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk) {}
    server_connection_session(const server_connection_session&) = delete;
    server_connection_session& operator=(const server_connection_session&) = delete;

    [[nodiscard]] server_connection& connection() noexcept {
        return facade_;
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    void abort() noexcept {
        aborted_ = true;
        if (lifecycle_ != nullptr) {
            lifecycle_->set_reason(connection_close_reason::aborted);
        }
        transport_.abort();
    }
    // Expires the facade once the handler returned. An operation the handler
    // left running is aborted first, so the join cannot wait on the peer.
    [[nodiscard]] task<void> join() {
        if (facade_.read_active_ || facade_.write_active_) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }
    // Ends the send direction the handler left open: close_notify for TLS.
    [[nodiscard]] task<void> finish() {
        if (!facade_.send_ended_) {
            facade_.send_ended_ = true;
            co_await end_send({});
        }
    }

private:
    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }
    [[nodiscard]] task<void> end_send(operation_options operation) {
        operation_interrupt interrupt(interrupts_, worker_, operation, this, &abort_thunk, "server connection finish");
        const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection finish");
        }
    }
    static task<std::optional<std::span<const std::byte>>> read_thunk(void* raw, operation_options operation) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, &owner.read_cancellation_,
            &wake_read, "server connection read");
        if (owner.receive_ended_) {
            co_return std::nullopt;
        }
        owner.read_buffer_.clear();
        if (!(co_await owner.transport_.read_more(
                owner.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); }, owner.read_cancellation_.slot(), &owner.read_reservation_))) {
            owner.receive_ended_ = true;
            if (owner.lifecycle_ != nullptr) {
                owner.lifecycle_->set_reason(connection_close_reason::peer_eof);
            }
            co_return std::nullopt;
        }
        owner.entry_.touch();
        co_return std::as_bytes(std::span(owner.read_buffer_.data(), owner.read_buffer_.size()));
    }
    static task<void> write_thunk(void* raw, std::string_view bytes, operation_options operation) {
        auto& owner = *static_cast<server_connection_session*>(raw);
        operation_interrupt interrupt(owner.interrupts_, owner.worker_, operation, raw, &abort_thunk, "server connection write");
        const auto error = co_await owner.transport_.write_bytes(bytes, http_stream_end::keep_open);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "server connection write");
        }
    }
    static task<void> finish_thunk(void* raw, operation_options operation) {
        return static_cast<server_connection_session*>(raw)->end_send(std::move(operation));
    }
    static void abort_thunk(void* raw) noexcept {
        static_cast<server_connection_session*>(raw)->abort();
    }

    http_socket_tunnel_transport<stream_type> transport_;
    connection_scanner::entry& entry_;
    connection_lifecycle* lifecycle_;
    const worker_ref& worker_;
    std::string read_buffer_;
    inbound_buffer_reservation read_reservation_;
    asio::cancellation_signal read_cancellation_;
    bool receive_ended_{false};
    bool aborted_{false};
    operation_interrupt_source interrupts_;
    server_connection facade_;
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr const char* connection_lane_in_progress = "server connection operation already in progress on this direction";

task<std::optional<std::span<const std::byte>>> read_connection(void* target,
    task<std::optional<std::span<const std::byte>>> (*read)(void*, operation_options), bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, connection_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}

task<void> write_connection(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::string bytes, bool& write_active, const bool& send_ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (send_ended) {
        throw std::logic_error("server connection send direction has ended");
    }
    co_await write(target, bytes, std::move(operation));
}

task<void> finish_connection(void* target, task<void> (*finish_value)(void*, operation_options), bool& send_ended,
    bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (!send_ended) {
        co_await finish_value(target, std::move(operation));
        send_ended = true;
    }
}

}  // namespace

server_connection::server_connection(const detail::server_connection_fields& fields,
    const ::ruvia::stop_token& stop_token_value, const worker_ref& worker_value, void* target, read_type read,
    write_type write, finish_type finish_value, abort_type abort_value) noexcept
    : worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort_value),
      id_(fields.id_),
      local_(fields.lifecycle_ != nullptr ? fields.lifecycle_->local_endpoint()
                                          : ip_endpoint(ip_address::parse(fields.local_address_).value_or(ip_address{}), fields.local_port_)),
      remote_(fields.lifecycle_ != nullptr ? fields.lifecycle_->remote_endpoint()
                                           : ip_endpoint(ip_address::parse(fields.remote_address_).value_or(ip_address{}), fields.remote_port_)),
      stop_token_(stop_token_value),
      state_(fields.state_) {
    if (fields.tls_.has_value()) {
        tls_.emplace(tls_connection_transport(
            fields.tls_->client_certificate_subject_, fields.tls_->alpn_, fields.tls_->server_name_));
    }
    operations_.bind_worker(worker_);
}

void server_connection::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("server connection has expired");
    }
}

task<std::optional<std::span<const std::byte>>> server_connection::read(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_connection(target_, read_, read_active_, std::move(operation)));
}

task<void> server_connection::write(std::string_view bytes, operation_options operation) & {
    require_active();
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    return write_owned(std::string(bytes), std::move(operation));
}

task<void> server_connection::write(std::span<const std::byte> bytes, operation_options operation) & {
    return write(as_chars(bytes), std::move(operation));
}

task<void> server_connection::write_owned(std::string&& bytes, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    auto owned = std::move(bytes);
    return ::ruvia::make_scoped_operation(operations_, write_connection(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}

task<void> server_connection::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, finish_connection(target_, finish_, send_ended_, write_active_, std::move(operation)));
}

void server_connection::abort() & noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

// The handler owns the connection until its task completes; the connection
// then ends here, so a handler never has to close it itself. The TLS ciphertext
// writer outlives the session that writes through it and is joined on every
// exit.
template <typename stream_type>
task<void> worker_connections::serve_connection(
    stream_type& stream, tcp_socket_type& socket, server_connection_fields fields) {
    // A stopping worker has already closed the connection.
    if (!http_server_worker_running(state_) || !socket.is_open()) {
        co_return;
    }
    std::string local_address;
    std::error_code local_error;
    if (const auto local = socket.local_endpoint(local_error); !local_error) {
        assign_peer_address(local_address, local.address());
        fields.local_address_ = local_address;
        fields.local_port_ = local.port();
    }
    connection_scanner::entry scanner_entry;
    scanner_entry.bind_lifecycle(fields.lifecycle_);
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    inbound_buffer_account connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    std::optional<tls_tunnel_output> tls_output;
    if constexpr (requires { stream.next_layer(); }) {
        tls_output.emplace(*stream.native_handle(), socket, worker_, fields.lifecycle_);
        tls_output->start();
    }
    std::exception_ptr failure;
    {
        server_connection_session<stream_type> session(stream, tls_output.has_value() ? &*tls_output : nullptr,
            scanner_entry, connection_buffers, worker_, stop_token_, fields);
        try {
            co_await options_.connection_(session.connection());
        } catch (...) {
            failure = std::current_exception();
            if (fields.lifecycle_ != nullptr) {
                fields.lifecycle_->note_exception(failure);
            }
        }
        if (failure != nullptr) {
            session.abort();
        }
        co_await session.join();
        // A worker stop keeps the immediate close.
        if (failure == nullptr && !session.aborted() && !stop_token_.stop_requested()) {
            try {
                co_await session.finish();
            } catch (...) {
                failure = std::current_exception();
                if (fields.lifecycle_ != nullptr) {
                    fields.lifecycle_->note_exception(failure);
                }
            }
        }
    }
    if (tls_output.has_value()) {
        if (failure != nullptr) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template task<void> worker_connections::serve_connection<inbound_socket_stream>(
    inbound_socket_stream&, tcp_socket_type&, server_connection_fields);
template task<void> worker_connections::serve_connection<server_tls_stream>(
    server_tls_stream&, tcp_socket_type&, server_connection_fields);

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_stream_access final {
    [[nodiscard]] static request_stream make(context& context_value, response_stream_writer& response,
        const request_stream_tunnel_accept* accept_tunnel,
        const request_stream_websocket_accept* accept_websocket = nullptr) noexcept {
        return request_stream(context_value, response, accept_tunnel, accept_websocket);
    }

    [[nodiscard]] static task<void> close(request_stream& stream) {
        return stream.operation_scope_.close_and_join();
    }
};

namespace {

// Runs the connection's callback; returns its failure, if any.
task<std::exception_ptr> run_request_stream_callback(
    context& context_value, response_stream_writer& response, const request_stream_tunnel_accept* accept_tunnel,
    const request_stream_websocket_accept* accept_websocket = nullptr) {
    const auto callback = context_access::services(context_value).request_stream();
    auto stream = request_stream_access::make(context_value, response, accept_tunnel, accept_websocket);
    std::exception_ptr failure;
    try {
        co_await callback(stream);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await request_stream_access::close(stream);
    co_return failure;
}

// After the callback: a committed response or tunnel stands, and a failure
// past it propagates to the transport, which aborts the stream and reports it.
// Otherwise nothing reached the peer: an explicit connection abort is terminal;
// without it the request gets the framework's answer to the failure, and a
// callback that selected nothing is a fault. Unexpected failures reach the sink.
// No application error handler takes part.
void complete_request_stream(context& context_value, std::exception_ptr failure) {
    if (context_access::services(context_value).connection_aborted() ||
        context_access::services(context_value).request_reset()) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (context_access::output_committed(context_value)) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (failure == nullptr) {
        if (context_access::has_response(context_value)) {
            return;
        }
        failure = std::make_exception_ptr(std::logic_error("request stream callback completed without a response"));
    }
    auto answer = make_default_exception_response(failure);
    if (answer.unexpected_) {
        context_access::services(context_value)
            .connection_failure()
            .invoke(context_access::services(context_value).remote_address(), failure);
    }
    context_access::set_response(context_value, std::move(answer.response_));
}

// The response route of a request stream connection: the callback writes
// through the transport's response stream, bound to the context.
task<void> serve_request_stream(void*, context& context_value) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), nullptr);
    complete_request_stream(context_value, std::move(failure));
}

[[nodiscard]] const route_entry& request_stream_response_route() noexcept {
    // Process-invariant and allocation-free: no route path, method token or
    // protocol to own.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::response_stream(&serve_request_stream, request_body_mode::stream)});
    return route;
}

[[nodiscard]] const route_entry& request_stream_tunnel_route() noexcept {
    // The tunnel upgrade runs serve_request_stream_tunnel() for this route
    // instead of its handler.
    static const route_entry route({.method_ = http_known_method::unknown,
        .path_ = {},
        .endpoint_ = route_endpoint::tunnel(&serve_request_stream, {}, {})});
    return route;
}

}  // namespace

route_resolution resolve_request_stream_route(bool tunnel) noexcept {
    return route_resolution::resolved(tunnel ? request_stream_tunnel_route() : request_stream_response_route(), nullptr);
}

bool is_request_stream_tunnel_route(const route_entry& route) noexcept {
    return &route == &request_stream_tunnel_route();
}

bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept {
    return request.known_method() == http_known_method::connect || (!content && http1_request_offers_upgrade(request));
}

task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept,
    const request_stream_websocket_accept* accept_websocket) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), &accept, accept_websocket);
    complete_request_stream(context_value, std::move(failure));
}

}  // namespace ruvia::detail

namespace ruvia {

const http_request& request_stream::request() const& noexcept {
    return detail::context_access::request(context_);
}

const conn_info& request_stream::conn() const& noexcept {
    return context_.conn();
}

request_body_state request_stream::body_state() const noexcept {
    return detail::context_access::services(context_).body_state();
}

void request_stream::reset() {
    detail::context_access::services(context_).reset_request({});
}

void request_stream::reset(http2_error_code error) {
    if (request().protocol_version() != http_protocol_version::http2) {
        throw std::invalid_argument("HTTP/2 reset code requires HTTP/2");
    }
    detail::context_access::services(context_).reset_request(error);
}

void request_stream::reset(http3_error_code error) {
    if (request().protocol_version() != http_protocol_version::http3) {
        throw std::invalid_argument("HTTP/3 reset code requires HTTP/3");
    }
    detail::context_access::services(context_).reset_request(error);
}

body_reader& request_stream::body() & {
    if (accept_tunnel_ != nullptr) {
        throw std::logic_error("a tunnel request carries no content: its bytes belong to accept_tunnel()");
    }
    return context_.req().body_reader();
}

std::span<const http_header> request_stream::trailers() const noexcept {
    return context_.req().trailers();
}

std::optional<std::string_view> request_stream::trailer(std::string_view name) const noexcept {
    return context_.req().trailer(name);
}

task<void> request_stream::inform(const http_interim_response_head& response, operation_options operation) & {
    return context_.inform(response, std::move(operation));
}

bool request_stream::early_data() const noexcept {
    return context_.early_data();
}

bool request_stream::push(http_push_request_view request) {
    return context_.push(request);
}

void request_stream::advertise_origins(std::span<const std::string_view> origins) {
    context_.advertise_origins(origins);
}

void request_stream::advertise_alternative_service(std::string_view value) {
    context_.advertise_alternative_service(value);
}

http_client_ref request_stream::client() const {
    return context_.client();
}

http_client_ref request_stream::client(std::string_view alias) const {
    return context_.client(alias);
}

#ifdef RUVIA_ENABLE_DATABASE
db_session request_stream::db() const {
    return context_.db();
}

db_session request_stream::db(std::string_view alias) const {
    return context_.db(alias);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session request_stream::redis() const {
    return context_.redis();
}

redis_session request_stream::redis(std::string_view alias) const {
    return context_.redis(alias);
}
#endif

context_response_editor request_stream::draft() & noexcept {
    return context_.draft();
}

task<http_response> request_stream::file(file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.file(options));
}

task<http_response> request_stream::static_file(const static_root& root, static_file_response_options options) {
    return make_scoped_operation(operation_scope_, context_.static_file(root, options));
}

void request_stream::respond(http_response response) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    require_unselected_response();
    if (tunnel_requested() && request().known_method() == http_known_method::connect &&
        response.status().is_successful()) {
        throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
    }
    if (response.status() == http_status::switching_protocols) {
        throw std::logic_error("a 101 response requires accept_tunnel() or accept_websocket()");
    }
    context_.respond(std::move(response));
}

response_stream_writer& request_stream::response() & noexcept {
    return response_;
}

bool request_stream::tunnel_requested() const noexcept {
    return accept_tunnel_ != nullptr;
}

task<http_tunnel*> request_stream::accept_tunnel(http_status_code status, http_tunnel_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_tunnel_impl(status, config));
}

task<http_tunnel*> request_stream::accept_tunnel_impl(http_status_code status, http_tunnel_route_config config) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_tunnel_ == nullptr) {
        throw std::logic_error("accept_tunnel() requires a CONNECT or HTTP/1.1 Upgrade request");
    }
    require_unselected_response();
    if (request().known_method() == http_known_method::connect) {
        if (!status.is_successful()) {
            throw std::invalid_argument("a CONNECT tunnel is accepted with a 2xx status");
        }
        context_.draft().status(status);
    } else if (status != http_status::switching_protocols) {
        throw std::invalid_argument("an HTTP/1.1 Upgrade is accepted with 101 (Switching Protocols)");
    }
    co_return co_await (*accept_tunnel_)(status, config);
}

task<websocket_accept_result> request_stream::accept_websocket(websocket_route_config config) & {
    return make_scoped_operation(operation_scope_, accept_websocket_impl(std::move(config)));
}

task<websocket_accept_result> request_stream::accept_websocket_impl(websocket_route_config config) {
    // Validated before any state check so an invalid configuration selects nothing.
    const auto endpoint = detail::route_endpoint::websocket(nullptr, std::move(config));
    co_return co_await accept_websocket_endpoint(*endpoint.websocket());
}

task<websocket_accept_result> request_stream::accept_websocket_endpoint(const detail::websocket_route_endpoint& endpoint) {
    auto lease = detail::claim_operation_lane(detail::streaming_access::output_lane(response_),
        "response stream output operation is already in progress");
    if (accept_websocket_ == nullptr) {
        throw std::logic_error("accept_websocket() requires an Upgrade or extended CONNECT request");
    }
    require_unselected_response();
    co_return co_await (*accept_websocket_)(endpoint);
}

void request_stream::require_unselected_response() const {
    if (detail::context_access::services(context_).request_reset() ||
        detail::context_access::services(context_).connection_aborted() ||
        response_.committed() || detail::context_access::output_committed(context_) ||
        detail::context_access::has_response(context_)) {
        throw std::logic_error("the request's response is already selected or committed");
    }
}

::ruvia::stop_token request_stream::stop_token() const noexcept {
    return context_.stop_token();
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] protocol_byte_limit request_body_byte_limit(request_body_mode body_mode,
    const std::optional<std::size_t>& max_stream_body_bytes, std::size_t max_buffered_body_bytes,
    std::size_t route_limit) {
    if (body_mode != request_body_mode::stream) {
        const auto limit = route_limit != 0 && route_limit < max_buffered_body_bytes
                               ? route_limit
                               : max_buffered_body_bytes;
        return protocol_byte_limit::limited(limit);
    }
    if (!max_stream_body_bytes.has_value()) {
        return route_limit != 0 ? protocol_byte_limit::limited(route_limit)
                                : protocol_byte_limit::unlimited();
    }
    const auto limit =
        route_limit != 0 && route_limit < *max_stream_body_bytes ? route_limit : *max_stream_body_bytes;
    return protocol_byte_limit::limited(limit);
}

// RFC 9110 §7.4 against the transport connection's facts. A secured
// connection without a recorded certificate cannot vouch for any host.
[[nodiscard]] bool connection_carries_target(const http_request& request, const context_services& services) noexcept {
    const auto origin = decide_http_target_origin(request,
        {.secured_ = services.get_conn_info().tls() != nullptr, .trusted_gateway_ = services.peer_trusted()});
    switch (origin.action()) {
        case http_target_origin_action::accept:
            return true;
        case http_target_origin_action::verify_certificate: {
            auto* certificate = services.served_certificate();
            return certificate != nullptr && certificate->covers(origin);
        }
        case http_target_origin_action::misdirected:
            break;
    }
    return false;
}

}  // namespace
http_request_admission_policy http_request_admission_policy::server(const http_server_options& options) noexcept {
    return {
        .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
        .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
        .apply_rate_limit_ = true,
        .verify_target_origin_ = true,
    };
}

protocol_byte_limit http_request_body_limit(const route_resolution& resolution, request_body_mode mode,
    const http_request_admission_policy& policy) noexcept {
    const auto* resolved = resolution.resolved();
    return request_body_byte_limit(mode, policy.max_stream_body_bytes_, policy.max_buffered_body_bytes_,
        resolved != nullptr ? resolved->route().max_request_body_bytes() : 0);
}

http_request_rejection http_request_rejection::rate_limited(rate_limit_rejection rejection) noexcept {
    return http_request_rejection(rate_limit_rejection_error(), rejection);
}

task<http_response> respond_http_request_rejection(const route_table& routes, const http_request& request, http_request_rejection rejection, const context_services& services) {
    auto response = co_await routes.handle_error(request, rejection.error(), services);
    if (const auto* rate_limit = rejection.rate_limit()) {
        apply_rate_limit_rejection_headers(response, *rate_limit);
    }
    co_return response;
}

void http_response_tail::record_access(const access_log_sink& access_log, http_status_code status) const noexcept {
    if (!access_log.callback_) {
        return;
    }
    access_log.invoke(access_log_record_access::make({
        .method_ = request_->method(),
        .known_method_ = request_->known_method(),
        .path_ = request_->path(),
        .protocol_version_ = request_->protocol_version(),
        .remote_address_ = remote_address_,
        .client_address_ = client_address_,
        .status_ = status,
        .duration_micros_ = access_log_record_access::duration_since(start_),
        .db_count_ = io_metrics_.db_count_,
        .db_duration_micros_ = io_metrics_.db_duration_micros_,
        .redis_count_ = io_metrics_.redis_count_,
        .redis_duration_micros_ = io_metrics_.redis_duration_micros_,
    }));
}

void http_response_tail::defer_access(const access_log_sink& access_log, http1_deferred_access_log& deferred,
    std::size_t head_end, http_status_code status) const {
    if (!access_log.callback_) {
        return;
    }
    deferred.defer(head_end, *request_, client_address_, start_, status, io_metrics_);
}

http_request_admission admit_http_request(const http_request_admission_input& input,
    const http_request_admission_policy& policy, const context_services& services) {
    const auto& request = input.request_;
    // The host's compression and CORS policies cover request stream responses
    // too; the application-wide rate limit belongs to the routes alone, and a
    // router applies its own.
    const bool request_stream = static_cast<bool>(services.request_stream());
    const auto coding = input.coding_;
    // Negotiate against every coding. A document root or context::static_file
    // may serve an indexed precompressed sidecar even when this worker has no
    // runtime encoder; the encoder capability travels to representation
    // preparation instead of pruning the client's choices here. A failed
    // negotiation stays in the selection until the response status is known: a
    // buffered 204/205/304 carries no representation and must not become 406.
    auto coding_policy = http_response_coding_policy::selected(coding);
    // Keyed on the client, not the hop: behind a trusted proxy every request
    // would otherwise share the proxy's single key. Resolved once per request
    // and bound to the services every context of this request is created from.
    const auto* resolved = input.resolution_.resolved();
    http_request_admission admission(
        http_response_tail(request, coding_policy, {}, {}, input.start_), services);
    if (services.peer_trusted()) {
        admission.services_.bind_conn_info(services.resolve_conn_info(request));
    }
    const auto& connection = admission.services_.get_conn_info();
    const auto remote_address = connection.remote().address();
    const auto client_address = connection.client().address();
    admission.tail_.bind_addresses(remote_address, client_address);
    admission.tail_.bind_alternative_service(services.alternative_service());
    admission.services_.bind_response_coding(coding);
    if (input.pinned_arrival_.has_value()) {
        admission.services_.pin_arrival(*input.pinned_arrival_);
    }
    if (resolved != nullptr) {
        admission.services_.bind_route_body_limit(resolved->route().max_request_body_bytes());
    }
    admission.resolved_ = resolved;
    const auto* endpoint = resolved != nullptr ? &resolved->route().endpoint() : nullptr;
    admission.body_limit_ = http_request_body_limit(input.resolution_,
        endpoint != nullptr ? endpoint->request_body_mode() : request_body_mode::buffered, policy);

    const auto reject = [&](http_request_rejection rejection) {
        admission.endpoint_ = http_request_endpoint::rejection;
        admission.rejection_ = rejection;
        return std::move(admission);
    };

    // Before any expectation, limit or endpoint decision: a request this
    // connection cannot carry is not this server's to evaluate.
    if (policy.verify_target_origin_ && !connection_carries_target(request, services)) {
        return reject(http_request_rejection::error(http_error_info(
            {.status_ = http_status::misdirected_request, .message_ = "misdirected request"})));
    }

    // Expect extensions are valid HTTP syntax. The protocol reports the
    // semantic fact; this product implements only 100-continue and chooses the
    // RFC 9110-permitted 417 before reading request content.
    if (input.expectation_ != nullptr &&
        *input.expectation_ == http_server_expectation_plan::reject_unsupported) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_unsupported_expectation_error(); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (policy.apply_rate_limit_ && !request_stream) {
        if (const auto rejection = decide_request_rate_limit(
                admission.services_.rate_limiter(), client_address, input.pinned_arrival_)) {
            return reject(http_request_rejection::rate_limited(*rejection));
        }
    }
    // Content already in hand (buffered bodies and subrequests) is held
    // to the same ceiling a streaming protocol enforces while reading.
    if (admission.body_limit_.exceeds(request.body_bytes().size())) {
        return reject(http_request_rejection::error(
            [&] { const auto err = http_request_body_protocol_error(http_request_body_failure::too_large); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }()));
    }
    if (endpoint == nullptr) {
        admission.endpoint_ = http_request_endpoint::buffered;
        return admission;
    }
    if (const auto* tunnel = endpoint->tunnel()) {
        admission.udp_tunnel_ = tunnel->protocol() == "connect-udp" ||
                                (request_stream && (request.protocol() == "connect-udp" || is_http_connect_udp_upgrade_request(request)));
        if (admission.udp_tunnel_ && validate_http_connect_udp_request(request).has_value()) {
            return reject(http_request_rejection::error(http_error_info(
                {.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"})));
        }
        admission.endpoint_ = http_request_endpoint::tunnel;
        return admission;
    }
    if (endpoint->websocket() != nullptr) {
        // An upgrade selects no HTTP response representation.
        admission.endpoint_ = http_request_endpoint::websocket;
        return admission;
    }
    if (endpoint->response_stream() != nullptr) {
        admission.endpoint_ = http_request_endpoint::response_stream;
        return admission;
    }
    const auto* buffered = endpoint->buffered();
    admission.endpoint_ = buffered != nullptr && buffered->request_body_mode() == request_body_mode::stream
                              ? http_request_endpoint::stream_body
                              : http_request_endpoint::buffered;
    return admission;
}

task<http_request_admission> admit_and_dispatch_buffered_request(const route_table& routes,
    const http_request_admission_input& input, const http_request_admission_policy& policy,
    const context_services& services, http_response& response) {
    auto admission = admit_http_request(input, policy, services);
    if (const auto* rejection = admission.rejection()) {
        // As in the server: a rejection does not inherit the coding promise.
        admission.tail().disable_response_coding();
        response = co_await respond_http_request_rejection(
            routes, input.request_, *rejection, admission.services());
    } else {
        co_await routes.dispatch_buffered_response(input.request_, input.resolution_,
            document_root_binding::none(), admission.services(), admission.static_file_mode(), response);
    }
    co_return admission;
}
}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A coded representation is worth sending only when strictly smaller than
// the identity bytes; the encoder stops at this cap instead of finishing.
// When the client refuses identity (RFC 9110 §12.5.3), any acceptable coding
// beats a 406, so the cap is lifted.
[[nodiscard]] http_content_encode_options compressed_body_options(
    std::string_view plain, bool identity_accepted) noexcept {
    if (!identity_accepted) {
        return {.max_encoded_bytes_ = (std::numeric_limits<std::size_t>::max)()};
    }
    return {.max_encoded_bytes_ = plain.empty() ? 0 : plain.size() - 1};
}

// Publishes an encoder result as the response representation. The commit
// stages every affected header before publishing the owned body, so a
// request-resource failure leaves the identity response usable for the typed
// 500 path instead of exposing a mixed body/metadata state.
[[nodiscard]] http_response_compression_status commit_compressed_body(
    http_response& response, http_content_coding coding, std::optional<std::string>&& encoding) {
    if (!encoding.has_value()) {
        return http_response_compression_status::not_applicable;
    }
    response.replace_body_with_content_encoding(std::move(*encoding), http_content_coding_token(coding));
    return http_response_compression_status::compressed;
}

[[nodiscard]] bool response_headers_skip_compression(const http_response& response) noexcept {
    if (response.header("Content-Range").has_value()) {
        return true;
    }
    if (const auto content_type = response.header("Content-Type");
        !content_type.has_value() || !is_http_media_type_compressible(*content_type)) {
        return true;
    }
    const bool has_cache_control = response.header("Cache-Control").has_value();
    cache_control_field_parser cache_parser;
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        switch (name.size()) {
            case 11U:
                if (http_ascii_equals_ignore_case(name, "Repr-Digest") ||
                    http_ascii_equals_ignore_case(name, "Content-MD5")) {
                    return true;
                }
                break;
            case 13U:
                if (http_ascii_equals_ignore_case(name, "Cache-Control")) {
                    cache_parser.update(header.value());
                }
                break;
            case 14U:
                if ((name[8] | 0x20) == 'd' && http_ascii_equals_ignore_case(name, "Content-Digest")) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return has_cache_control && cache_parser.finish().has(cache_control_directive::no_transform);
}

}  // namespace

std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept {
    const auto field = response.header("Content-Length");
    if (!field.has_value()) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> declared;
    bool valid = true;
    http_visit_comma_separated_quoted_field_items(*field, [&](std::string_view item) noexcept {
        if (item.empty()) {
            valid = false;
            return false;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
        if (ec != std::errc{} || ptr != item.data() + item.size() ||
            (declared.has_value() && *declared != parsed)) {
            valid = false;
            return false;
        }
        declared = parsed;
        return true;
    });
    return valid ? declared : std::nullopt;
}

bool declared_length_permits_encoding(const http_response_coding_selection& selection,
    const compression_config& options, std::uint64_t declared) noexcept {
    // min_bytes_ only says when encoding stops paying off; a client that
    // refuses identity is served a coding at any size instead of a 406.
    return !(((declared == 0 || declared < options.min_bytes_) && selection.identity_accepted()) ||
             declared > options.max_bytes_);
}

namespace {

// An empty HEAD body is a HEAD handler describing the GET representation
// without producing it. Encoding those zero bytes would publish the coded
// length of nothing (RFC 9110 §8.6). Apply the coding GET would select, sized
// by the declared Content-Length, and drop that length: the coded size is
// unknown and §8.6 allows omitting it.
[[nodiscard]] http_buffered_compression_step apply_head_content_encoding(
    const http_response_coding_selection& selection, http_response& response,
    const compression_config& options, std::optional<std::uint64_t> declared) {
    const bool encode = declared.has_value() ? declared_length_permits_encoding(selection, options, *declared)
                                             : !selection.identity_accepted();
    if (!encode) {
        return {http_response_compression_status::not_applicable, false};
    }
    response.apply_content_encoding(http_content_coding_token(selection.coding()));
    return {http_response_compression_status::compressed, false};
}

}  // namespace

http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability) {
    // A file's bytes are served as the representation their validators and
    // HEAD response describe, whether the transport reads the file or the
    // context already carries them in memory.
    if (source_value == http_response_compression_source::buffered && response.has_file_representation()) {
        return http_response_compression_decision::fixed_representation;
    }
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return http_response_compression_decision::fixed_representation;
    }
    // An application-encoded body is kept, but whether it is sent (200) or
    // refused (406) still depends on Accept-Encoding, so caches must key on it
    // (RFC 9110 §12.5.5; Pingora depends_on_accept_encoding).
    if (response.header("Content-Encoding").has_value()) {
        response.add_vary_token("Accept-Encoding");
        return http_response_compression_decision::fixed_representation;
    }
    // Integrity fields describe the content or the selected representation,
    // both of which include the content coding (RFC 9530 §1, §2, §3; legacy
    // Content-MD5 likewise covered the sent content). Encoding here would
    // leave them describing bytes that are not sent.
    if (availability == http_response_coding_availability::identity_only ||
        response.status() == http_status::partial_content ||
        response_headers_skip_compression(response)) {
        return http_response_compression_decision::fixed_representation;
    }

    response.add_vary_token("Accept-Encoding");
    if (selection.coding() == http_content_coding::identity) {
        return http_response_compression_decision::negotiated_identity;
    }
    return http_response_compression_decision::encode;
}

http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection,
    http_known_method request_method, http_response& response, const compression_config& options,
    worker_codecs& codecs, bool offload_available) {
    constexpr http_buffered_compression_step not_applicable{http_response_compression_status::not_applicable, false};
    const auto body = response.body_bytes();

    const auto decision = prepare_response_compression(selection, request_method, response,
        http_response_compression_source::buffered,
        http_response_coding_availability::identity_and_compression);
    // Negotiation errors above still propagate; only encoding and publication
    // failures become a typed compression result.
    try {
        if (decision == http_response_compression_decision::encode && request_method == http_known_method::head &&
            body.empty() && (response.has_head_metadata_body() || response.header("Content-Length").has_value())) {
            const auto declared = declared_content_length(response);
            if (declared != std::uint64_t{0}) {
                return apply_head_content_encoding(selection, response, options, declared);
            }
        }
        if (decision != http_response_compression_decision::encode ||
            !declared_length_permits_encoding(selection, options, body.size())) {
            return not_applicable;
        }
        // sync_bytes_ is the inline ceiling. Larger bodies encode on the blocking
        // pool; without one they stay identity (or 406 when identity is refused)
        // rather than stall the event loop.
        if (body.size() > options.sync_bytes_) {
            return {http_response_compression_status::not_applicable, offload_available};
        }

        const auto coding = selection.coding();
        auto encoding = codecs.encoder(coding).encode(
            body, compressed_body_options(body, selection.identity_accepted()));
        return {commit_compressed_body(response, coding, std::move(encoding)), false};
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        return {http_response_compression_status::failed, false};
    }
}

task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection,
    http_response& response, const worker_ref& worker_value, const stop_token& stop_token_value) {
    try {
        const auto coding = selection.coding();
        const bool identity_accepted = selection.identity_accepted();
        std::string plain(response.body_bytes());
        auto result_value = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [coding, identity_accepted, plain = std::move(plain)]() {
                return encode_http_content(
                    coding, plain, compressed_body_options(plain, identity_accepted));
            });
        if (result_value.failed()) {
            ruvia::report_unhandled_failure("response compression", result_value.error());
            co_return http_response_compression_status::failed;
        }
        if (!result_value.completed()) {
            // Queue saturation and shutdown are overload/lifecycle outcomes,
            // not broken encoders. Preserve the identity representation when
            // identity is acceptable; otherwise report 503 Service Unavailable.
            co_return http_response_compression_status::unavailable;
        }
        co_return commit_compressed_body(response, coding, std::move(result_value).take_value());
    } catch (...) {
        ruvia::report_unhandled_failure("response compression", std::current_exception());
        // Input ownership, encoder execution and representation publication
        // leave the original identity body intact on failure.
        co_return http_response_compression_status::failed;
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

connection_transport connection_transport::tcp(asio::ip::tcp::socket& socket) noexcept {
    return connection_transport(
        &socket, [](void* target) noexcept { close_socket(*static_cast<asio::ip::tcp::socket*>(target)); },
        [](void* target) noexcept {
            std::error_code ignored;
            static_cast<asio::ip::tcp::socket*>(target)->cancel(ignored);
        });
}
namespace {

[[nodiscard]] worker_ref require_scanner_worker(worker_ref worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("connection scanner requires a valid worker");
    }
    return worker_value;
}

[[nodiscard]] bool timeout_expired(
    const std::optional<timeout_duration>& timeout, std::int64_t inactive_ms) noexcept {
    return timeout.has_value() && inactive_ms >= timeout->value().count();
}

[[nodiscard]] std::int64_t close_deadline_after(std::int64_t now, std::chrono::milliseconds delay) noexcept {
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    return now > maximum - delay.count() ? maximum : now + delay.count();
}

[[nodiscard]] std::int64_t activity_now_ms() noexcept {
    // 向上取整活动时间，避免毫秒截断让超时提前；扫描时钟仍向下取整。
    return std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

connection_scanner::periodic_check_registration::~periodic_check_registration() noexcept {
    reset();
}
void connection_scanner::periodic_check_registration::reset() noexcept {
    if (entry_ != nullptr) {
        entry_->remove_periodic_check(*this);
    }
}
connection_scanner::entry::~entry() noexcept {
    connection_scanner::detach_entry(*this);
    detach_periodic_checks();
}
void connection_scanner::entry::touch() noexcept {
    if (scanner_ != nullptr) {
        last_active_ms_ = activity_now_ms();
    }
}
void connection_scanner::entry::note_write_progress() noexcept {
    if (scanner_ != nullptr) {
        last_active_ms_ = activity_now_ms();
        write_progress_ms_ = last_active_ms_;
    }
}
void connection_scanner::entry::set_phase(phase next_phase) noexcept {
    if (scanner_ == nullptr) {
        return;
    }
    const bool write_started = next_phase == phase::writing && !write_pending();
    touch();
    if (phase_ != next_phase) {
        phase_started_ms_ = last_active_ms_;
    }
    if (write_started) {
        write_progress_ms_ = last_active_ms_;
    }
    phase_ = next_phase;
    // A head read that completed before its cancel took effect returns the
    // connection to its phase timeouts; the stale request has no deadline.
    if (reading_request_head_) {
        close_requested_ = false;
    }
    reading_request_head_ = false;
    sync_reusable();
}
void connection_scanner::entry::read_request_head(phase timeout_phase) noexcept {
    set_phase(timeout_phase);
    reading_request_head_ = scanner_ != nullptr;
    close_requested_ = false;
    sync_reusable();
}
connection_scanner::entry::phase_scope::~phase_scope() {
    if (entry_.scanner_ == nullptr) {
        return;
    }
    entry_.touch();
    entry_.phase_started_ms_ = phase_started_ms_;
    entry_.phase_ = phase_;
    entry_.reading_request_head_ = reading_request_head_;
    entry_.sync_reusable();
}
connection_scanner::entry::write_scope::write_scope(entry& entry_value) noexcept
    : entry_(entry_value) {
    if (!entry_.write_pending()) {
        entry_.note_write_progress();
    }
    ++entry_.write_scopes_;
}
connection_scanner::entry::write_scope::~write_scope() {
    --entry_.write_scopes_;
    entry_.touch();
}
void connection_scanner::entry::enter_long_lived() noexcept {
    set_phase(phase::long_lived);
}
std::int64_t connection_scanner::entry::last_active_ms() const noexcept {
    return last_active_ms_;
}
void connection_scanner::entry::register_periodic_check(periodic_check_registration& registration,
    void* target, periodic_check_type tick, drain_check_type drain) noexcept {
    registration.reset();
    registration.entry_ = this;
    registration.target_ = target;
    registration.tick_ = tick;
    registration.drain_ = drain;
    registration.next_ = periodic_checks_;
    if (periodic_checks_ != nullptr) {
        periodic_checks_->prev_ = &registration;
    }
    periodic_checks_ = &registration;
}
void connection_scanner::entry::remove_periodic_check(
    periodic_check_registration& registration) noexcept {
    if (registration.entry_ != this) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        periodic_checks_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (periodic_scan_next_ == &registration) {
        periodic_scan_next_ = registration.next_;
    }
    registration.entry_ = nullptr;
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.tick_ = nullptr;
    registration.drain_ = nullptr;
}
void connection_scanner::entry::detach_periodic_checks() noexcept {
    periodic_scan_next_ = nullptr;
    while (periodic_checks_ != nullptr) {
        periodic_checks_->reset();
    }
}
bool connection_scanner::entry::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}
void connection_scanner::entry::sync_reusable() noexcept {
    const bool listed = reusable_next_ != nullptr;
    if (scanner_ == nullptr || reusable() == listed) {
        return;
    }
    if (listed) {
        unlink_reusable();
        return;
    }
    auto& sentinel = scanner_->impl_->reusable_sentinel_;
    reusable_prev_ = sentinel.reusable_prev_;
    reusable_next_ = &sentinel;
    sentinel.reusable_prev_->reusable_next_ = this;
    sentinel.reusable_prev_ = this;
}
void connection_scanner::entry::unlink_reusable() noexcept {
    if (reusable_next_ == nullptr) {
        return;
    }
    reusable_prev_->reusable_next_ = reusable_next_;
    reusable_next_->reusable_prev_ = reusable_prev_;
    reusable_prev_ = reusable_next_ = nullptr;
}
bool connection_scanner::entry::has_drain_check() const noexcept {
    for (const auto* registration = periodic_checks_; registration != nullptr; registration = registration->next_) {
        if (registration->drain_ != nullptr) {
            return true;
        }
    }
    return false;
}
void connection_scanner::entry::notify_drain() noexcept {
    // A callback may detach its own registration; read the successor first.
    for (auto* registration = periodic_checks_; registration != nullptr;) {
        auto* const next = registration->next_;
        if (registration->drain_ != nullptr) {
            registration->drain_(registration->target_);
        }
        registration = next;
    }
}
connection_scanner::guard::guard(
    connection_scanner& scanner, entry& entry_value, connection_transport transport_value)
    : entry_(entry_value) {
    scanner.impl_->register_entry(entry_, transport_value);
}
connection_scanner::guard::~guard() {
    connection_scanner::detach_entry(entry_);
}
connection_scanner::impl_type::impl_type(
    connection_scanner* owner_value, worker_ref worker_value, connection_scanner_options options)
    : owner_(owner_value),
      worker_(require_scanner_worker(std::move(worker_value))),
      timer_state_(std::make_shared<timer_state_type>(this)),
      options_(std::move(options)),
      cached_now_ms_(steady_now_ms()) {
    sentinel_.prev_ = sentinel_.next_ = &sentinel_;
    reusable_sentinel_.reusable_prev_ = reusable_sentinel_.reusable_next_ = &reusable_sentinel_;
}
bool connection_scanner::impl_type::retire() noexcept {
    // scanning_ is worker-local; only the worker thread may observe it. On that
    // thread a set flag means this call is nested inside the scan callback,
    // which already holds the timer mutex.
    const bool on_worker = worker_.is_current();
    const bool nested_in_scan = on_worker && scanning_;
    if (nested_in_scan) {
        stop();
        timer_state_->owner_ = nullptr;
        release_after_scan_ = true;
    } else if (on_worker) {
        stop();
        timer_state_->owner_ = nullptr;
    } else {
        // Another thread may be scanning; wait for that callback to finish.
        std::lock_guard lock(timer_state_->mutex_);
        stop();
        timer_state_->owner_ = nullptr;
    }
    // Unregistration keeps the scan cursors consistent, so a nested scan ends
    // without visiting any detached node.
    while (sentinel_.next_ != &sentinel_) {
        unregister_entry(*sentinel_.next_);
    }
    return nested_in_scan;
}

connection_scanner::connection_scanner(worker_ref worker_value, connection_scanner_options options)
    : impl_(std::make_unique<impl_type>(this, std::move(worker_value), std::move(options))) {}
connection_scanner::~connection_scanner() noexcept {
    if (impl_->retire()) {
        // The scan running further up this stack frees the impl.
        static_cast<void>(impl_.release());
    }
}
void connection_scanner::start() {
    impl_->start();
}
void connection_scanner::stop() noexcept {
    impl_->stop();
}
void connection_scanner::close_all() noexcept {
    impl_->close_all();
}
bool connection_scanner::evict_oldest_reusable() noexcept {
    auto& sentinel = impl_->reusable_sentinel_;
    for (auto* current = sentinel.reusable_next_; current != &sentinel; current = current->reusable_next_) {
        // A write still under way belongs to an exchange that only just
        // ended; its connection is not idle yet.
        if (current->write_pending()) {
            continue;
        }
        // Closed at once rather than asked to close: the new connection takes
        // the slot now. The close request keeps the entry out of the list
        // while its owner unwinds.
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::aborted);
        }
        impl_type::record_close_request(*current, impl_->cached_now_ms_);
        current->transport_.close();
        return true;
    }
    return false;
}
void connection_scanner::detach_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != nullptr) {
        entry_value.scanner_->impl_->unregister_entry(entry_value);
    }
}

void connection_scanner::impl_type::start() {
    if (running_) {
        return;
    }
    running_ = true;
    cached_now_ms_ = steady_now_ms();
    try {
        schedule();
    } catch (...) {
        running_ = false;
        throw;
    }
}
void connection_scanner::impl_type::stop() noexcept {
    running_ = false;
    timer_.cancel();
}
void connection_scanner::impl_type::register_entry(entry& entry_value, connection_transport transport_value) {
    if (entry_value.scanner_ != nullptr) {
        throw std::logic_error("connection scanner entry is already registered");
    }
    // A parked scanner resumes with its first entry. Armed before linking, so
    // a failure leaves the entry unregistered.
    if (running_ && !timer_.registered()) {
        schedule();
    }
    entry_value.transport_ = transport_value;
    entry_value.scanner_ = owner_;
    entry_value.touch();
    entry_value.phase_started_ms_ = entry_value.last_active_ms_;
    entry_value.phase_ = phase::idle;
    entry_value.close_requested_ = false;
    entry_value.next_ = sentinel_.next_;
    entry_value.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry_value;
    sentinel_.next_ = &entry_value;
}
void connection_scanner::impl_type::unregister_entry(entry& entry_value) noexcept {
    if (entry_value.scanner_ != owner_ || !entry_value.linked()) {
        return;
    }
    if (entry_value.last_active_ms_ > retired_activity_ms_) {
        retired_activity_ms_ = entry_value.last_active_ms_;
    }
    if (scan_current_ == &entry_value) {
        scan_current_ = nullptr;
    }
    if (scan_next_ == &entry_value) {
        scan_next_ = entry_value.next_;
    }
    entry_value.prev_->next_ = entry_value.next_;
    entry_value.next_->prev_ = entry_value.prev_;
    entry_value.prev_ = entry_value.next_ = nullptr;
    entry_value.unlink_reusable();
    entry_value.transport_ = {};
    entry_value.detach_periodic_checks();
    entry_value.scanner_ = nullptr;
}
void connection_scanner::impl_type::close_all() noexcept {
    for (auto* current = sentinel_.next_; current != &sentinel_; current = current->next_) {
        if (current->lifecycle_ != nullptr) {
            current->lifecycle_->set_reason(connection_close_reason::cancelled);
        }
        current->transport_.close();
    }
}
void connection_scanner::impl_type::schedule() {
    if (!running_) {
        return;
    }
    const auto timer_state = timer_state_;
    // A stopped timer queue arms nothing: scanning ends with the worker, as it
    // does for a scan timer that stop_timers() cancels.
    (worker_).schedule_timer(timer_, ::ruvia::deadline_after(options_.scan_interval_.value()), [timer_state](worker_timer_outcome outcome) {
        if (outcome == worker_timer_outcome::cancelled) {
            return;
        }
        impl_type* destroy_after_lock = nullptr;
        {
            std::lock_guard lock(timer_state->mutex_);
            auto* scanner = timer_state->owner_;
            if (scanner == nullptr || !scanner->running_) {
                return;
            }
            // Expiry has consumed this timer. Release its token before callbacks,
            // which may stop, restart, or destroy the scanner.
            scanner->timer_.cancel();
            // 每轮采样一次比较时钟，供超时、协议检查和跨轮空闲判断共用。
            const auto interval_start = scanner->cached_now_ms_;
            scanner->cached_now_ms_ = steady_now_ms();
            scanner->scanning_ = true;
            const bool idle = scanner->scan(interval_start);
            scanner->scanning_ = false;
            if (scanner->release_after_scan_) {
                // A callback destroyed the scanner; its teardown already ran.
                destroy_after_lock = scanner;
            } else {
                // Never during an exchange or a burst: only after a whole interval
                // in which no entry carried a request exchange and none had activity
                // (I/O, registration or retirement), once per such quiet period. The
                // timer runs on the worker, whose thread heap this collects.
                if (idle && scanner->retired_activity_ms_ < interval_start) {
                    if (!scanner->quiet_trimmed_) {
                        collect_thread_heap();
                        scanner->quiet_trimmed_ = true;
                    }
                } else {
                    scanner->quiet_trimmed_ = false;
                }
                // With no entry left and the quiet period trimmed, nothing remains to
                // time: park until register_entry() re-arms the scan.
                const bool parked = scanner->quiet_trimmed_ && scanner->sentinel_.next_ == &scanner->sentinel_;
                if (!parked && !scanner->timer_.registered()) {
                    scanner->schedule();
                }
            }
        }
        if (destroy_after_lock != nullptr) {
            delete destroy_after_lock;
        }
    });
}
bool connection_scanner::impl_type::scan(std::int64_t interval_start) noexcept {
    const auto now = cached_now_ms_;
    bool idle = true;
    scan_next_ = sentinel_.next_;
    while (scan_next_ != &sentinel_) {
        scan_current_ = scan_next_;
        scan_next_ = scan_current_->next_;
        // Checked before this entry's callbacks, which may touch it at the new time.
        idle = idle && !active_since(*scan_current_, interval_start);
        scan_current_->periodic_scan_next_ = scan_current_->periodic_checks_;
        // A callback can retire the current entry or a later one. Unregistration
        // updates both scanner-owned cursors before the entry can be destroyed.
        while (scan_current_ != nullptr && scan_current_->periodic_scan_next_ != nullptr) {
            auto* registration = scan_current_->periodic_scan_next_;
            scan_current_->periodic_scan_next_ = registration->next_;
            registration->tick_(registration->target_, now);
        }
        if (scan_current_ != nullptr && is_timed_out(*scan_current_, now)) {
            expire(*scan_current_);
        }
        scan_current_ = nullptr;
    }
    scan_next_ = nullptr;
    return idle;
}
bool connection_scanner::impl_type::active_since(const entry& entry_value, std::int64_t interval_start) noexcept {
    // A connection waiting for, or still reading, a request head has no
    // exchange; neither has an upgraded one between messages, nor a
    // multiplexed session without a stream. Anything else (a handshake, an
    // admitted request, a response) is an exchange.
    return entry_value.last_active_ms_ >= interval_start || entry_value.write_pending() ||
           (!entry_value.reading_request_head_ && entry_value.phase_ != phase::long_lived &&
               entry_value.phase_ != phase::awaiting_stream);
}
void connection_scanner::impl_type::expire(entry& entry_value) noexcept {
    if (entry_value.lifecycle_ != nullptr) {
        entry_value.lifecycle_->set_reason(connection_close_reason::timeout);
    }
    if (!entry_value.close_requested_) {
        const auto now = cached_now_ms_;
        if (entry_value.reading_request_head_) {
            request_close(entry_value, close_deadline_after(now, options_.scan_interval_.value()));
            return;
        }
        // A stalled write cannot carry a graceful end (GOAWAY, close alert);
        // a write still progressing is followed by the drain's own output.
        if (entry_value.phase_ != phase::writing && !write_stalled(entry_value, now) &&
            entry_value.has_drain_check()) {
            // The callback may retire the entry; record the request first.
            record_close_request(entry_value, close_deadline_after(now, connection_close_grace));
            entry_value.notify_drain();
            return;
        }
    }
    entry_value.transport_.close();
}
void connection_scanner::impl_type::request_close(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // The pending read completes with operation_aborted; its owner then ends
    // the connection with its close alert instead of an abrupt close. A TLS
    // read can lose the cancel (the ssl operation resumes reading a partial
    // record), so the deadline closes the socket of one that never returns.
    record_close_request(entry_value, deadline_ms);
    entry_value.transport_.cancel();
}
void connection_scanner::impl_type::record_close_request(entry& entry_value, std::int64_t deadline_ms) noexcept {
    // Repeated requests (a timeout, then a drain) keep the earliest deadline.
    if (!entry_value.close_requested_ || deadline_ms < entry_value.close_deadline_ms_) {
        entry_value.close_deadline_ms_ = deadline_ms;
    }
    entry_value.close_requested_ = true;
    entry_value.sync_reusable();
}
bool connection_scanner::impl_type::is_timed_out(const entry& entry_value, std::int64_t now) const noexcept {
    // A graceful close request is bounded by its own deadline, which later
    // activity (the GOAWAY or alert write it asked for) does not extend.
    if (entry_value.close_requested_) {
        return now >= entry_value.close_deadline_ms_;
    }
    if (write_stalled(entry_value, now)) {
        return true;
    }
    // 活动时间独立采样，不能把配置的扫描间隔当成调度延迟的上界。
    const auto inactive_ms = now - entry_value.last_active_ms_;
    const auto phase_elapsed_ms = now - entry_value.phase_started_ms_;
    switch (entry_value.phase_) {
        case phase::reading_initial:
            return timeout_expired(options_.initial_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.initial_read_completion_timeout_, phase_elapsed_ms);
        case phase::reading_payload:
            return timeout_expired(options_.payload_read_timeout_, inactive_ms) ||
                   timeout_expired(options_.payload_read_completion_timeout_, phase_elapsed_ms);
        case phase::writing:
        case phase::long_lived:
            return false;
        case phase::awaiting_stream:
            return timeout_expired(options_.idle_timeout_, phase_elapsed_ms);
        case phase::idle:
        default:
            return timeout_expired(options_.idle_timeout_, inactive_ms);
    }
}
bool connection_scanner::impl_type::write_stalled(const entry& entry_value, std::int64_t now) const noexcept {
    return entry_value.write_pending() &&
           timeout_expired(options_.write_timeout_, now - entry_value.write_progress_ms_);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Connection-private ownership of the Web product's request-count policy.
// HTTP still owns persistence and response framing; this connection state keeps
// an optional saturating remaining-response budget, contributes one typed close policy
// before a streamed head is committed, and records each completed route once.
// A stopping worker (which closes every connection) never allows reuse.
// Once the peer has ended its request direction behind pipelined requests,
// the sequence is those already received: they are served, then the
// connection closes without reading the transport again.
class http1_request_sequence final {
public:
    http1_request_sequence(
        std::optional<std::size_t> max_requests, const http_server_worker_state& worker_state)
        : requests_until_close_(max_requests),
          worker_state_(worker_state) {
        if (max_requests.has_value() && *max_requests == 0) {
            throw std::invalid_argument(
                "configured requests-per-connection limit must be greater than zero");
        }
    }

    http1_request_sequence(std::size_t, const http_server_worker_state&) = delete;

    http1_request_sequence(const http1_request_sequence&) = delete;
    http1_request_sequence& operator=(const http1_request_sequence&) = delete;
    http1_request_sequence(http1_request_sequence&&) = delete;
    http1_request_sequence& operator=(http1_request_sequence&&) = delete;

    [[nodiscard]] http1_close_policy next_response_close_policy() const noexcept {
        return budget_ends_with_next_response() || !http_server_worker_running(worker_state_)
                   ? http1_close_policy::close_after_response
                   : http1_close_policy::allow_reuse;
    }

    // Buffered response bytes have not been committed yet, so the request
    // budget may still tighten the protocol plan before Connection fields are
    // finalized.
    [[nodiscard]] http1_request_connection_plan complete_uncommitted_response(
        http1_request_connection_plan connection_plan) noexcept {
        const auto close_policy = next_response_close_policy();
        record_completion();
        return close_policy == http1_close_policy::close_after_response ? connection_plan.require_close()
                                                                        : connection_plan;
    }

    // A streamed head already carries the pre-commit close policy. Once bytes
    // are committed the plan cannot be tightened; fail fast if a caller tries
    // to complete a limit-ending response whose wire plan still permits reuse.
    void complete_committed_response(http1_request_connection_plan connection_plan) noexcept {
        if (budget_ends_with_next_response() &&
            connection_plan.disposition() != http1_close_policy::close_after_response) {
            std::terminate();
        }
        record_completion();
    }

    // A streamed response observed the peer's request-direction end (RFC 9112
    // section 9.6) behind requests it retained for the session.
    void mark_request_input_ended() noexcept {
        request_input_ended_ = true;
    }

    [[nodiscard]] bool request_input_ended() const noexcept {
        return request_input_ended_;
    }

private:
    [[nodiscard]] bool budget_ends_with_next_response() const noexcept {
        return requests_until_close_.has_value() && *requests_until_close_ == 1;
    }

    void record_completion() noexcept {
        // One is deliberately saturated so an impossible extra completion
        // cannot reopen reuse; absence remains unlimited.
        if (requests_until_close_.has_value() && *requests_until_close_ > 1) {
            --*requests_until_close_;
        }
    }

    std::optional<std::size_t> requests_until_close_;
    const http_server_worker_state& worker_state_;
    bool request_input_ended_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t chunked_encoded_buffer_bytes = max_http_header_bytes;

[[noreturn]] inline void throw_request_body_too_large() {
    throw http_request_body_protocol_error(http_request_body_failure::too_large);
}

[[noreturn]] inline void throw_incomplete_request_body() {
    throw http_request_body_protocol_error(http_request_body_failure::incomplete);
}

// Classifies a decode result that produced no output: returns when the decoder
// completed or needs more input, throws for every failure.
inline void require_transfer_coding_progress(const http_transfer_coding_decode_result& result_value) {
    if (const auto* failure = result_value.failure()) {
        throw http_request_transfer_coding_error(failure->error());
    }
    if (result_value.decoder_failure() != nullptr) {
        throw std::runtime_error("transfer-coding decoder failure");
    }
    if (result_value.complete() == nullptr && result_value.need_input() == nullptr) {
        throw std::logic_error("unexpected transfer-coding decode result");
    }
}

inline void require_complete_transfer_coding(http_transfer_coding_stack_decoder& decoder) {
    const auto finish_result = decoder.finish_input();
    if (finish_result.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finish_result.failure()) {
        throw http_request_transfer_coding_error(failure->error());
    }
    if (finish_result.decoder_failure() != nullptr) {
        throw std::runtime_error("transfer-coding decoder failure");
    }
    throw std::logic_error("unexpected transfer-coding finish result");
}

// Reserves the next growth step of a known-length body whose first `filled`
// bytes have arrived: double them, at least known_length_min_read_bytes, never
// past the declared final size. Returns that final size, saturated at max_size().
[[nodiscard]] inline std::size_t reserve_known_length_growth(
    std::string& body, inbound_buffer_reservation& body_reservation, std::size_t filled, std::size_t remaining) {
    const auto final_size = remaining > body.max_size() - filled ? body.max_size() : filled + remaining;
    const auto doubled = filled > body.max_size() / 2 ? body.max_size() : filled * 2;
    const auto target_capacity = std::min(final_size, std::max(doubled, known_length_min_read_bytes));
    if (target_capacity > body.capacity()) {
        inbound_reserve(body, body_reservation, target_capacity);
    }
    return final_size;
}

// Where a body reader leaves the pipelined suffix -- the bytes of the next
// request that arrived together with this one. The suffix normally still sits
// at the tail of the connection read buffer, where the session compacts it in
// place once the views borrowing that buffer are dead. Only bytes the reader
// over-read into its own buffer, which dies with the route, are copied into
// request-scoped storage.
struct body_pipeline_handover final {
    // Length of the suffix still at the tail of the connection read buffer;
    // zero when `stashed_` holds the pipeline.
    std::size_t read_buffer_suffix_bytes_{};
    std::string_view stashed_;
};

// Advances `decoder` over the chunked framing in `input` from `cursor`,
// skipping payload without decoding it. True once the body ended there,
// trailers included; false when it needs more input or the framing failed.
[[nodiscard]] inline bool skip_buffered_chunked_body(
    http1_chunked_body_decoder& decoder, std::string_view input, std::size_t& cursor) {
    for (;;) {
        const auto result_value = decoder.decode(input.substr(cursor));
        cursor += result_value.consumed_bytes();
        if (result_value.complete() != nullptr) {
            return true;
        }
        if (result_value.body_chunk() == nullptr) {
            return false;
        }
    }
}

// Length of the framed request body when all of it is already in
// `body_and_pipeline`, the input that followed its head, so it is consumed
// without I/O and the connection stays reusable (RFC 9112 section 9.3);
// nullopt when it still needs input or its chunked framing is invalid.
[[nodiscard]] inline std::optional<std::size_t> buffered_request_body_bytes(
    const http1_request_body_plan& body_plan, std::string_view body_and_pipeline,
    protocol_byte_limit body_limit = protocol_byte_limit::unlimited()) {
    if (const auto* known_length = body_plan.known_length()) {
        const auto content_length = known_length->content_length();
        return !body_limit.exceeds(content_length) && body_and_pipeline.size() >= content_length
                   ? std::optional<std::size_t>(content_length)
                   : std::nullopt;
    }
    if (body_plan.chunked() == nullptr) {
        return std::size_t{0};
    }
    http1_chunked_body_decoder decoder({.body_limit_ = body_limit});
    std::size_t cursor = 0;
    return skip_buffered_chunked_body(decoder, body_and_pipeline, cursor) ? std::optional<std::size_t>(cursor)
                                                                          : std::nullopt;
}

template <typename stream_type>
class stream_body_reader final {
public:
    // `interim_output` is the request's interim lane, which carries the 100
    // Continue an expectation asks for.
    stream_body_reader(stream_type& stream, inbound_buffer_account* inbound_buffers,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry& scanner_entry,
        http_interim_response_output& interim_output, const worker_ref& worker)
        : stream_(stream),
          buffer_reservation_(inbound_buffers),
          transfer_output_reservation_(inbound_buffers),
          initial_body_and_pipeline_(initial_body_and_pipeline),
          body_plan_(body_plan),
          body_limit_(body_limit_value),
          chunk_decoder_(http1_chunked_body_decoder_config{.body_limit_ = body_limit_value}),
          scanner_entry_(scanner_entry),
          interim_output_(interim_output),
          worker_(worker),
          state_(body_plan_.requires_consumption() ? state_type::active : state_type::finished) {
        const auto expectation_plan =
            body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
        continue_pending_ = expectation_plan == http_server_expectation_plan::send_continue;
        const auto* chunked = body_plan_.chunked();
        if (chunked != nullptr && !chunked->transfer_codings().empty()) {
            const auto& codings = chunked->transfer_codings();
            transfer_decoder_ = std::make_unique<http_transfer_coding_stack_decoder>(std::span<const http_transfer_coding>(codings.data(), codings.size()), body_limit_value);
        }
    }

    stream_body_reader(const stream_body_reader&) = delete;
    stream_body_reader& operator=(const stream_body_reader&) = delete;

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return state_ == state_type::finished ? http1_request_body_consumption::complete
                                              : http1_request_body_consumption::incomplete;
    }
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    const http_request_trailers& trailers() const&& = delete;
    // Consumes, without I/O, the rest of a body that arrived in full with
    // its head and that the route left unread or partly read, so the
    // connection stays reusable. A body that still needs input, or whose
    // read failed, stays incomplete.
    void consume_buffered_body() {
        // Reads keep borrowing the initial view only while it still holds unread
        // body bytes; once they moved to buffer_, the rest needs socket input.
        if (state_ != state_type::active || initial_body_and_pipeline_.empty()) {
            return;
        }
        if (const auto* known_length = body_plan_.known_length()) {
            if (initial_body_and_pipeline_.size() >= known_length->content_length()) {
                delivered_bytes_ = known_length->content_length();
                mark_finished();
            }
            return;
        }
        // Chunked framing resumes where read_chunked() left the decoder, at
        // read_cursor_ in the borrowed view.
        if (skip_buffered_chunked_body(chunk_decoder_, initial_body_and_pipeline_, read_cursor_)) {
            mark_finished();
        }
    }
    // Hands the pipelined suffix over and drops this reader's claim on it. The
    // connection read buffer is deliberately untouched: every view in the
    // request being served still borrows it. `stash` receives bytes when the
    // suffix lives in this reader's own buffer or a later reader supplies
    // `additional_pipeline`; those later bytes follow both existing sources.
    [[nodiscard]] body_pipeline_handover take_pipeline(
        std::string& stash, std::string_view additional_pipeline = {}) {
        // The body reader's suffix is either the tail of the connection read
        // buffer or its own over-read. A passive reader can add a later suffix:
        // in that case all three sources must outlive their readers, in order.
        const auto initial_pipeline = initial_pipeline_remainder();
        const auto buffered_pipeline = buffered_pipeline_remainder();
        body_pipeline_handover handover;
        if (buffered_pipeline.empty() && additional_pipeline.empty()) {
            handover.read_buffer_suffix_bytes_ = initial_pipeline.size();
        } else {
            stash.clear();
            stash.reserve(initial_pipeline.size() + buffered_pipeline.size() + additional_pipeline.size());
            stash.append(initial_pipeline);
            stash.append(buffered_pipeline);
            stash.append(additional_pipeline);
            handover.stashed_ = stash;
        }
        reset_pipeline_state();
        return handover;
    }
    [[nodiscard]] std::size_t pipeline_size() const noexcept {
        return initial_pipeline_remainder().size() + buffered_pipeline_remainder().size();
    }
    // Runs `observer(target)` once, when a read completes the body, before that
    // read returns; its exception fails the read. Once the body is complete no
    // read touches the transport again, so the observer may start one.
    using completion_observer_type = void (*)(void*);
    void observe_completion(void* target, completion_observer_type observer) noexcept {
        completion_target_ = target;
        completion_observer_ = observer;
    }

    // An interrupted read keeps the reader active: the bytes already buffered
    // and the decoder state wait for the next read. Until then the body clock
    // is paused, and a response that leaves the body incomplete closes the
    // connection after it.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        require_not_failed();
        operation_interrupt interrupt(
            interrupts_, worker_, operation, &read_cancellation_, &wake_read, "request body read");
        const read_interrupt_scope running(read_interrupt_, interrupt);
        try {
            co_return co_await read_next();
        } catch (...) {
            if (interrupt.interrupted()) {
                scanner_entry_.set_phase(connection_scanner::phase::idle);
            } else {
                state_ = state_type::failed;
            }
            throw;
        }
    }

    // Like read(), an interruption keeps the bytes already appended to `body`
    // and the decoder state; the next call resumes.
    task<std::string_view> read_all(std::string& body, inbound_buffer_reservation& body_reservation, operation_options operation = {}) {
        require_not_failed();
        operation_interrupt interrupt(
            interrupts_, worker_, operation, &read_cancellation_, &wake_read, "request body read");
        const read_interrupt_scope running(read_interrupt_, interrupt);
        try {
            if (const auto* known_length = body_plan_.known_length()) {
                const auto content_length = known_length->content_length();
                if (state_ == state_type::finished) {
                    if (body.empty() && delivered_bytes_ == 0 && initial_body_and_pipeline_.size() >= content_length) {
                        co_return initial_body_and_pipeline_.substr(0, content_length);
                    }
                    co_return std::string_view(body);
                }
                if (exceeds_limit(content_length)) {
                    throw_request_body_too_large();
                }
                // A complete preread body borrows the connection buffer and needs
                // no I/O coroutine; preserve that borrow until the session retires it.
                if (delivered_bytes_ == 0 && initial_body_and_pipeline_.size() >= content_length) {
                    mark_finished();
                    co_return initial_body_and_pipeline_.substr(0, content_length);
                }
                co_return co_await read_known_length_all(body, body_reservation, content_length);
            }
            if (body_plan_.without_body() != nullptr) {
                co_return std::string_view(body);
            }

            if (continue_pending_) {
                co_await send_continue();
            }
            while (auto chunk = co_await read_chunked()) {
                if (transfer_decoder_ != nullptr) {
                    decode_transfer_append(::ruvia::as_chars(*chunk), body, body_reservation);
                } else {
                    if (body_limit_.addition_exceeds(body.size(), chunk->size())) {
                        throw_request_body_too_large();
                    }
                    body.append(::ruvia::as_chars(*chunk));
                }
            }
            if (transfer_decoder_ != nullptr) {
                require_complete_transfer_coding(*transfer_decoder_);
            }
            mark_finished();
            co_return std::string_view(body);
        } catch (...) {
            if (interrupt.interrupted()) {
                scanner_entry_.set_phase(connection_scanner::phase::idle);
            } else {
                state_ = state_type::failed;
            }
            throw;
        }
    }

private:
    enum class state_type : std::uint8_t { active,
        finished,
        failed };

    // Publishes the running read's interrupt to the waits below it.
    class read_interrupt_scope final {
    public:
        read_interrupt_scope(const operation_interrupt*& slot, const operation_interrupt& interrupt) noexcept
            : slot_(slot) {
            slot_ = &interrupt;
        }
        ~read_interrupt_scope() {
            slot_ = nullptr;
        }
        read_interrupt_scope(const read_interrupt_scope&) = delete;
        read_interrupt_scope& operator=(const read_interrupt_scope&) = delete;

    private:
        const operation_interrupt*& slot_;
    };

    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }

    // The unread window never received bytes; an interrupted read keeps only
    // the `filled` prefix for the next one.
    void throw_if_read_interrupted_keeping(std::string& body, std::size_t filled) const {
        if (read_interrupt_ != nullptr && read_interrupt_->interrupted()) {
            body.resize(filled);
            read_interrupt_->throw_if_interrupted();
        }
    }

    void throw_if_read_interrupted() const {
        if (read_interrupt_ != nullptr) {
            read_interrupt_->throw_if_interrupted();
        }
    }

    task<std::optional<std::span<const std::byte>>> read_next() {
        if (body_plan_.chunked() != nullptr) {
            if (continue_pending_) {
                co_await send_continue();
            }
            co_return co_await read_transfer_decoded_chunked();
        }
        const auto* known_length = body_plan_.known_length();
        if (known_length == nullptr) {
            co_return std::nullopt;
        }
        if (exceeds_limit(known_length->content_length())) {
            throw_request_body_too_large();
        }
        if (continue_pending_) {
            co_await send_continue();
        }
        co_return co_await read_known_length(known_length->content_length());
    }

    task<void> send_continue() {
        co_await interim_output_.send_continue();
        continue_pending_ = false;
    }

    void require_not_failed() const {
        if (state_ == state_type::failed) {
            throw std::logic_error("request body consumption previously failed");
        }
    }

    [[nodiscard]] std::string_view initial_pipeline_remainder() const noexcept {
        if (initial_body_and_pipeline_.empty()) {
            return {};
        }
        if (body_plan_.chunked() != nullptr) {
            return initial_body_and_pipeline_.substr(
                std::min(read_cursor_, initial_body_and_pipeline_.size()));
        }
        const auto* known_length = body_plan_.known_length();
        if (known_length == nullptr) {
            return initial_body_and_pipeline_;
        }
        const auto initial_body_bytes =
            std::min(known_length->content_length(), initial_body_and_pipeline_.size());
        return initial_body_and_pipeline_.substr(initial_body_bytes);
    }

    [[nodiscard]] std::string_view buffered_pipeline_remainder() const noexcept {
        if (read_cursor_ >= buffer_.size()) {
            return {};
        }
        return std::string_view(buffer_.data() + read_cursor_, buffer_.size() - read_cursor_);
    }

    void reset_pipeline_state() noexcept {
        buffer_.clear();
        initial_body_and_pipeline_ = {};
        read_cursor_ = 0;
    }

    void materialize_initial_remainder(std::size_t extra_reserve = 0) {
        if (initial_body_and_pipeline_.empty()) {
            return;
        }
        const auto remainder =
            initial_body_and_pipeline_.substr(std::min(read_cursor_, initial_body_and_pipeline_.size()));
        buffer_.clear();
        if (!remainder.empty() || extra_reserve != 0) {
            const auto hard_limit =
                body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
            const auto target_reserve = extra_reserve > hard_limit - std::min(remainder.size(), hard_limit)
                                            ? hard_limit
                                            : remainder.size() + extra_reserve;
            if (target_reserve > buffer_.capacity()) {
                inbound_reserve(buffer_, buffer_reservation_, std::max(remainder.size(), target_reserve));
            }
            inbound_append(buffer_, buffer_reservation_, remainder);
        }
        initial_body_and_pipeline_ = {};
        read_cursor_ = 0;
    }

    task<void> read_more() {
        // Earlier body views are dead once more input is needed. Drop the consumed
        // prefix here; writable tail space is reused before compaction or growth.
        if (read_cursor_ != 0) {
            buffer_.consume(read_cursor_);
            read_cursor_ = 0;
        }
        const auto old_size = buffer_.size();
        const auto hard_limit =
            body_plan_.chunked() != nullptr ? chunked_encoded_buffer_bytes : body_limit_.read_ceiling();
        if (old_size >= hard_limit) {
            throw_request_body_too_large();
        }
        const auto writable = std::min<std::size_t>(http_body_buffer_bytes, hard_limit - old_size);
        auto read_space = buffer_.prepare_spare();
        if (read_space.empty()) {
            read_space = inbound_prepare(buffer_, buffer_reservation_, writable);
        } else {
            read_space = read_space.first(std::min(writable, read_space.size()));
        }

        throw_if_read_interrupted();
        scanner_entry_.set_phase(connection_scanner::phase::reading_payload);
        auto read_completion =
            co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
                stream_.async_read_some(asio::buffer(read_space),
                    asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
            });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        if (ec == asio::error::operation_aborted) {
            throw_if_read_interrupted();
        }
        if (ec || bytes_read == 0) {
            throw_incomplete_request_body();
        }
        buffer_.commit(bytes_read);
        scanner_entry_.touch();
    }

    task<std::string_view> read_known_length_all(std::string& body, inbound_buffer_reservation& body_reservation, std::size_t content_length) {
        const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
        if (continue_pending_) {
            co_await send_continue();
        }

        // The initial segment is shorter than the body, so it is all body and no
        // pipelined bytes follow it; an earlier read() may also have left a
        // buffered remainder. Take both before reading the socket.
        const auto initial_unread =
            delivered_bytes_ < initial_body_bytes ? initial_body_bytes - delivered_bytes_ : std::size_t{0};
        const auto remaining_after_initial = content_length - delivered_bytes_ - initial_unread;
        const auto buffered_unread =
            read_cursor_ < buffer_.size() ? std::min(buffer_.size() - read_cursor_, remaining_after_initial)
                                          : std::size_t{0};
        const auto preread_unread = initial_unread + buffered_unread;
        if (preread_unread != 0) {
            const auto after_preread = body.size() + preread_unread;
            const auto remaining_after_preread = remaining_after_initial - buffered_unread;
            if (remaining_after_preread == 0) {
                inbound_reserve(body, body_reservation, after_preread);
            } else {
                (void)reserve_known_length_growth(body, body_reservation, after_preread, remaining_after_preread);
            }
        }
        if (initial_unread != 0) {
            body.append(initial_body_and_pipeline_.substr(delivered_bytes_, initial_unread));
            delivered_bytes_ = initial_body_bytes;
        }
        initial_body_and_pipeline_ = {};
        if (buffered_unread != 0) {
            body.append(buffer_.data() + read_cursor_, buffered_unread);
            read_cursor_ += buffered_unread;
            delivered_bytes_ += buffered_unread;
        }

        // Read the rest straight into the tail of `body`, never past the declared
        // length, so no bytes of a pipelined request are taken. Capacity starts at
        // min(declared length, known_length_min_read_bytes) and then grows
        // geometrically from what has actually arrived, so a peer that sends only
        // headers cannot make the declared Content-Length allocate past that floor.
        // `body` is resized once per growth step and the reads fill the window up
        // to its size, so each byte is initialized at most once before a read
        // overwrites it; `filled` marks the bytes the socket has written.
        if (delivered_bytes_ < content_length) {
            scanner_entry_.set_phase(connection_scanner::phase::reading_payload);
        }
        auto filled = body.size();
        while (delivered_bytes_ < content_length) {
            const auto remaining_body = content_length - delivered_bytes_;
            if (filled == body.size()) {
                const auto final_size = reserve_known_length_growth(body, body_reservation, filled, remaining_body);
                inbound_resize(body, body_reservation, std::min(final_size, body.capacity()));
            }
            const auto writable = std::min(remaining_body, body.size() - filled);
            throw_if_read_interrupted_keeping(body, filled);
            auto read_completion =
                co_await ruvia::async_asio<std::size_t>([this, &body, filled, writable](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(body.data() + filled, writable),
                        asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec == asio::error::operation_aborted) {
                throw_if_read_interrupted_keeping(body, filled);
            }
            if (ec || bytes_read == 0) {
                // The unread window never received bytes; it must not survive as body.
                body.resize(filled);
                throw_incomplete_request_body();
            }
            filled += bytes_read;
            delivered_bytes_ += bytes_read;
            scanner_entry_.touch();
        }
        mark_finished();
        co_return std::string_view(body);
    }

    task<std::optional<std::span<const std::byte>>> read_known_length(std::size_t content_length) {
        if (state_ == state_type::finished) {
            co_return std::nullopt;
        }
        if (exceeds_limit(content_length)) {
            throw_request_body_too_large();
        }
        if (content_length == 0 || delivered_bytes_ == content_length) {
            mark_finished();
            co_return std::nullopt;
        }

        const auto initial_body_bytes = std::min(content_length, initial_body_and_pipeline_.size());
        if (delivered_bytes_ < initial_body_bytes) {
            const auto remaining_body = content_length - delivered_bytes_;
            const auto available = initial_body_bytes - delivered_bytes_;
            const auto chunk_bytes = std::min(available, remaining_body);
            auto chunk = initial_body_and_pipeline_.substr(delivered_bytes_, chunk_bytes);
            delivered_bytes_ += chunk_bytes;
            if (delivered_bytes_ == content_length) {
                mark_finished();
            }
            co_return ::ruvia::as_bytes(chunk);
        }

        // The initial segment is now fully consumed as body: a partial-body prefix
        // cannot be followed by pipelined bytes, so the whole borrowed view was
        // body. Drop it before reading into buffer_, so read_cursor_ addresses
        // buffer_ (mirrors the chunked path's materialize_initial_remainder()).
        if (initial_body_bytes == initial_body_and_pipeline_.size()) {
            initial_body_and_pipeline_ = {};
        }

        while (buffer_.size() <= read_cursor_) {
            co_await read_more();
        }

        const auto remaining_body = content_length - delivered_bytes_;
        const auto available = buffer_.size() - read_cursor_;
        const auto chunk_bytes = std::min(available, remaining_body);
        auto chunk = std::string_view(buffer_.data() + read_cursor_, chunk_bytes);
        read_cursor_ += chunk_bytes;
        delivered_bytes_ += chunk_bytes;
        if (delivered_bytes_ == content_length) {
            mark_finished();
        }

        co_return ::ruvia::as_bytes(chunk);
    }

    task<std::optional<std::span<const std::byte>>> read_chunked() {
        if (state_ == state_type::finished) {
            co_return std::nullopt;
        }

        for (;;) {
            const auto source_value = !initial_body_and_pipeline_.empty()
                                          ? std::string_view(initial_body_and_pipeline_)
                                          : buffer_.view();
            const auto result_value = chunk_decoder_.decode(source_value.substr(read_cursor_));
            // Advancing the cursor keeps the returned chunk view intact; the bytes
            // behind it are reclaimed only by read_more().
            read_cursor_ += result_value.consumed_bytes();
            if (const auto* body_chunk = result_value.body_chunk()) {
                co_return ::ruvia::as_bytes(body_chunk->bytes());
            }
            if (const auto* complete = result_value.complete()) {
                if (trailers_.append_received(complete->trailers()).has_value()) {
                    throw std::logic_error("HTTP/1 decoder published invalid request trailers");
                }
                co_return std::nullopt;
            }
            if (const auto* failure = result_value.failure()) {
                throw http_request_chunk_decode_error(failure->error());
            }
            if (result_value.need_more() == nullptr) {
                throw std::logic_error("unexpected HTTP/1 chunk decode result");
            }
            materialize_initial_remainder(http_body_buffer_bytes);
            co_await read_more();
        }
    }

    task<std::optional<std::span<const std::byte>>> read_transfer_decoded_chunked() {
        if (transfer_decoder_ == nullptr) {
            auto chunk = co_await read_chunked();
            if (!chunk) {
                mark_finished();
            }
            co_return chunk;
        }

        const auto output_window = transfer_output();

        // Keep the borrowed encoded chunk until the decoder reports its consumed
        // prefix. read_chunked() is called only after that view is empty, so its
        // compaction cannot invalidate decoder input across application reads.
        for (;;) {
            const auto result_value = transfer_decoder_->decode(transfer_input_, output_window);
            transfer_input_.remove_prefix(std::min(transfer_input_.size(), result_value.consumed_bytes()));
            if (const auto* output = result_value.output()) {
                co_return ::ruvia::as_bytes(output->bytes());
            }
            require_transfer_coding_progress(result_value);

            auto chunk = co_await read_chunked();
            if (!chunk) {
                require_complete_transfer_coding(*transfer_decoder_);
                mark_finished();
                co_return std::nullopt;
            }
            transfer_input_ = ::ruvia::as_chars(*chunk);
        }
    }

    // The transfer decoder's output scratch: a fixed window sized once per reader.
    [[nodiscard]] std::span<char> transfer_output() {
        if (transfer_output_.empty()) {
            inbound_resize(transfer_output_, transfer_output_reservation_, http_body_buffer_bytes);
        }
        return std::span<char>(transfer_output_);
    }

    // Decodes into the reused scratch and appends what the decoder produced, so
    // `target` only ever grows by written bytes.
    void decode_transfer_append(std::string_view input, std::string& target, inbound_buffer_reservation& target_reservation) {
        const auto output_window = transfer_output();
        for (;;) {
            const auto result_value = transfer_decoder_->decode(input, output_window);
            input.remove_prefix(std::min(input.size(), result_value.consumed_bytes()));
            if (const auto* output = result_value.output()) {
                inbound_append(target, target_reservation, output->bytes());
                continue;
            }
            require_transfer_coding_progress(result_value);
            return;
        }
    }

    [[nodiscard]] bool exceeds_limit(std::size_t bytes_value) const noexcept {
        return body_limit_.exceeds(bytes_value);
    }

    void mark_finished() {
        state_ = state_type::finished;
        scanner_entry_.set_phase(connection_scanner::phase::idle);
        if (const auto observer = std::exchange(completion_observer_, nullptr)) {
            observer(completion_target_);
        }
    }
    stream_type& stream_;
    http_io_buffer buffer_;
    inbound_buffer_reservation buffer_reservation_;
    std::string transfer_output_;
    inbound_buffer_reservation transfer_output_reservation_;
    asio::cancellation_signal read_cancellation_;
    std::unique_ptr<http_transfer_coding_stack_decoder>
        transfer_decoder_;
    std::string_view transfer_input_;
    std::string_view initial_body_and_pipeline_;
    http1_request_body_plan body_plan_;
    protocol_byte_limit body_limit_;
    http1_chunked_body_decoder chunk_decoder_;
    http_request_trailers trailers_;
    connection_scanner::entry& scanner_entry_;
    http_interim_response_output& interim_output_;
    const worker_ref& worker_;
    // The running read's interrupt; null between reads.
    const operation_interrupt* read_interrupt_{};
    // Offset of the first unconsumed byte in the active source: the borrowed
    // initial view while it is set, buffer_ otherwise. Consumed buffer_ bytes are
    // dropped only when read_more() needs room for socket input.
    std::size_t read_cursor_{0};
    std::size_t delivered_bytes_{0};
    // A failed read remains terminal and incomplete, including observer failures.
    state_type state_;
    bool continue_pending_{false};
    void* completion_target_{};
    completion_observer_type completion_observer_{};
    operation_interrupt_source interrupts_;
};

template <typename stream_type>
class lazy_buffered_body final {
public:
    lazy_buffered_body(stream_type& stream, inbound_buffer_account* inbound_buffers,
        std::string_view initial_body_and_pipeline, http1_request_body_plan body_plan,
        protocol_byte_limit body_limit_value, connection_scanner::entry& scanner_entry,
        http_interim_response_output& interim_output, const worker_ref& worker)
        : reader_(stream, inbound_buffers, initial_body_and_pipeline, body_plan, body_limit_value, scanner_entry,
              interim_output, worker),
          body_reservation_(inbound_buffers) {}

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return reader_.consumption();
    }
    [[nodiscard]] stream_body_reader<stream_type>& reader() & noexcept RUVIA_LIFETIMEBOUND {
        return reader_;
    }
    stream_body_reader<stream_type>& reader() && = delete;
    [[nodiscard]] const http_request_trailers& trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return reader_.trailers();
    }
    const http_request_trailers& trailers() const&& = delete;

    [[nodiscard]] std::size_t pipeline_size() const noexcept {
        return reader_.pipeline_size();
    }

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        return reader_.read_all(body_, body_reservation_, std::move(operation));
    }

    task<void> discard(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (co_await reader_.read(steps.next("request body read"))) {
        }
    }

private:
    stream_body_reader<stream_type> reader_;
    std::string body_;
    inbound_buffer_reservation body_reservation_;
};

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP2)
#endif

// What every HTTP/1 route dispatch needs from the session that owns the
// request: the transport, the scanner entry, the parsed head, the route table
// and services to run the handler with, the options, the response being built,
// and the keep-alive sequence. Each dispatcher takes this and adds only what
// its own kind of route needs.
//
// Passed by value, as its members were as separate arguments: the references
// cost nothing to copy and context_services is copied per dispatch either way.

namespace ruvia::detail {

template <typename stream_type>
struct http1_route_dispatch final {
    stream_type& stream_;
    connection_scanner::entry& scanner_entry_;
    const http1_server_request_parse_state& parsed_;
    http_response_coding_selection response_coding_;
    // Admission's CORS policy for this request; null when none applies.
    const http_cors_policy* cors_;
    const route_table& routes_;
    context_services base_route_services_;
    const http_server_options& options_;
    http_response& response_;
    http1_request_sequence& request_sequence_;
    inbound_buffer_account& inbound_buffers_;
    // The request was already buffered behind the previous one when the
    // session started it: the client sent it before that response finished.
    bool request_pipelined_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Commits a framework response that ends the connection: a rejection, a
// failed route, or a route response the connection cannot outlive. The
// response is the framework's own (an error or exception handler's output):
// failing to commit it leaves no response to fall back to.
[[nodiscard]] inline http1_session_request_completion close_with_framework_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return http1_session_request_completion::make_buffered_closing(
        http1_commit_final_response(response, connection_plan.require_close()));
}

// Commits a route's buffered response. A response that breaks the HTTP/1
// final-response rules becomes a rejected completion that the session answers
// through the exception handler instead of the route response.
template <typename make_completion_type>
[[nodiscard]] inline http1_session_request_completion commit_http1_route_response(http_response& response,
    http1_request_connection_plan connection_plan, make_completion_type make_completion) {
    try {
        return make_completion(http1_commit_final_response(response, connection_plan));
    } catch (const http1_response_head_error& error) {
        return http1_session_request_completion::make_rejected_final_response(connection_plan, error);
    }
}

[[nodiscard]] inline http1_session_request_completion finalize_buffered_route_response(http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t consumed_bytes) {
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    return commit_http1_route_response(response, connection_plan,
        [consumed_bytes](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_unrestored(committed, consumed_bytes);
        });
}

// Route responses for an upgrade request always close the connection.
[[nodiscard]] inline http1_session_request_completion finalize_closing_route_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return commit_http1_route_response(response, connection_plan.require_close(),
        [](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename stream_type>
struct http_lazy_buffered_body_route_state final {
    std::optional<request_body_loader_binding<lazy_buffered_body<stream_type>>> body_;

    void emplace(stream_type& stream, inbound_buffer_account* inbound_buffers, std::string_view body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        connection_scanner::entry& scanner_entry, http_interim_response_output& interim_output,
        const worker_ref& worker) {
        body_.emplace(stream, inbound_buffers, body_and_pipeline, body_plan, body_limit_value,
            scanner_entry, interim_output, worker);
    }

    [[nodiscard]] context_services with_loader(const context_services& base_services) noexcept {
        auto services = base_services;
        services.bind_lazy_request_body(body_->facade());
        services.bind_request_trailers(body_->loader().trailers());
        return services;
    }

    [[nodiscard]] const lazy_buffered_body<stream_type>& body() const noexcept {
        return body_->loader();
    }

    [[nodiscard]] lazy_buffered_body<stream_type>& body() noexcept {
        return body_->loader();
    }
};

// Returns the setup failure (allocation included) so the caller answers it
// through the route failure path instead of unwinding the session.
template <typename stream_type>
[[nodiscard]] inline std::exception_ptr try_prepare_http_lazy_buffered_body_route(
    http_lazy_buffered_body_route_state<stream_type>& state_value,
    const http1_route_dispatch<stream_type>& d, protocol_byte_limit body_limit_value,
    std::string_view body_and_pipeline) {
    try {
        // Every HTTP/1 request's services carry its session's interim lane.
        state_value.emplace(d.stream_, &d.inbound_buffers_,
            body_and_pipeline, d.parsed_.body_plan(), body_limit_value, d.scanner_entry_,
            *d.base_route_services_.interim_output(), d.base_route_services_.worker());
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

// `request_bytes` is the connection's buffered input from the start of this
// request: its head, then whatever body and pipelined bytes followed it.
[[nodiscard]] inline std::string_view http_body_and_pipeline(
    const http1_server_request_head_ready& request_head, std::string_view request_bytes) noexcept {
    return request_bytes.substr(request_head.header_bytes());
}

inline task<http1_session_request_completion> complete_failed_http_route(
    connection_scanner::entry& scanner_entry, std::exception_ptr exception,
    const http1_server_request_parse_state& parsed_value, const route_table& routes_value, const context_services& exception_services,
    http_response& response) {
    if (auto* lifecycle = exception_services.connection_lifecycle_value()) {
        lifecycle->note_exception(exception);
    }
    response = co_await routes_value.handle_exception(
        parsed_value.request(), exception, exception_services);
    scanner_entry.touch();
    co_return close_with_framework_response(response, parsed_value.connection_plan());
}

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included. A suffix still in that buffer
// becomes a consumed prefix; only bytes the body reader over-read go through
// `pipeline_stash`, request-scoped storage that outlives the returned
// completion. The read buffer is not touched here -- the response has not been
// written and the access log not recorded yet, and both still read views that
// borrow it.
template <typename take_pipeline_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    http1_request_body_consumption body_consumption, std::size_t request_bytes,
    std::string& pipeline_stash, take_pipeline_type take_pipeline) {
    connection_plan = apply_request_body_consumption(connection_plan, body_consumption);
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    scanner_entry.touch();
    return commit_http1_route_response(response, connection_plan,
        [&](http1_request_connection_plan committed) {
            if (committed.disposition() == http1_close_policy::allow_reuse) {
                const auto pipeline = take_pipeline(pipeline_stash);
                if (!pipeline.stashed_.empty()) {
                    return http1_session_request_completion::make_buffered_pipeline_restore(
                        committed, pipeline.stashed_);
                }
                return http1_session_request_completion::make_buffered_unrestored(
                    committed, request_bytes - pipeline.read_buffer_suffix_bytes_);
            }
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

// A body the route left unread costs no reuse when it arrived in full with
// its head: it is consumed here without I/O.
template <typename stream_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t request_bytes, std::string& pipeline_stash, stream_body_reader<stream_type>& reader) {
    reader.consume_buffered_body();
    return complete_successful_http_body_route(scanner_entry, response, connection_plan, request_sequence,
        reader.consumption(), request_bytes, pipeline_stash,
        [&reader](std::string& stash) { return reader.take_pipeline(stash); });
}

// Completes a response produced without reading the request body (an
// unresolved route or a rate-limit rejection). `request_bytes` is the
// connection's buffered input from the start of this request. A body that
// arrived in full with the head is consumed without I/O and the connection
// stays reusable; any other unread body closes it after the response.
[[nodiscard]] inline http1_session_request_completion finalize_unread_body_response(http_response& response,
    const http1_server_request_parse_state& parsed_value, const http1_server_request_head_ready& request_head,
    std::string_view request_bytes, http1_request_sequence& request_sequence,
    protocol_byte_limit body_limit = protocol_byte_limit::unlimited()) {
    const auto body_bytes = buffered_request_body_bytes(
        parsed_value.body_plan(), http_body_and_pipeline(request_head, request_bytes), body_limit);
    const auto connection_plan = apply_request_body_consumption(parsed_value.connection_plan(),
        body_bytes ? http1_request_body_consumption::complete : http1_request_body_consumption::incomplete);
    return finalize_buffered_route_response(
        response, connection_plan, request_sequence, request_head.header_bytes() + body_bytes.value_or(0));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// HTTP/1 流式响应的读侧终止检测：请求体完整消费后由 route 自动启动（同 Go
// net/http background read）。单独到达的请求遇对端 FIN/RST 请求当前请求的 stop。
// pipeline 内（本请求排在前一请求之后到达，或其后已有后续请求字节）的干净结束
// （TCP FIN、TLS close_notify）是请求方向半关闭：不取消当前响应，并经
// request_input_ended() 交给连接，已收到的请求照常服务、不再读取；RST、TLS 截断
// 与其他错误始终中止。route 负责 cancel/join 后再释放 transport。
template <typename stream_type>
class http1_peer_eof_monitor final {
public:
    // `request_stop` is the request's own source; it outlives the monitor.
    // `request_pipelined`: the request itself arrived behind another one.
    http1_peer_eof_monitor(stream_type& stream, const worker_ref& worker,
        stop_source& request_stop, bool request_pipelined, std::size_t existing_pipeline_bytes)
        : stream_(stream),
          request_stop_(request_stop),
          tasks_(worker),
          allowance_(max_http_header_bytes - std::min(max_http_header_bytes, existing_pipeline_bytes)),
          pipelined_(request_pipelined || existing_pipeline_bytes != 0) {}
    http1_peer_eof_monitor(stream_type&, worker_ref&&,
        stop_source&, bool, std::size_t) = delete;
    void start() {
        tasks_.spawn(read_peer());
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_aborted_;
    }

    // The peer ended its request direction behind pipelined bytes. The
    // transport holds no more input; reading it again would only report the
    // same end. Final once join() has returned.
    [[nodiscard]] bool request_input_ended() const noexcept {
        return request_input_ended_;
    }

    [[nodiscard]] std::string_view pipeline() const noexcept {
        return input_.view();
    }

    void ensure_writable() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
        if (peer_aborted_) {
            throw inbound_transport_error(asio::error::operation_aborted,
                "HTTP response stream peer has disconnected");
        }
    }

    // The sink requests the request stop itself on every write failure.
    void write_failed(const std::error_code& error) noexcept {
        peer_aborted_ = peer_aborted_ || is_inbound_connection_end(error);
        cancel();
    }

    void cancel() noexcept {
        // Set before emitting: an already-queued successful completion must
        // retain its bytes but must not schedule another read after teardown.
        closing_ = true;
        cancellation_.emit(asio::cancellation_type::terminal);
    }

    task<void> join() {
        if (!joined_) {
            joined_ = true;
            co_await tasks_.join();
        }
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    task<void> read_peer() {
        try {
            // Stop once this bounded look-ahead is full. No input is discarded;
            // a reset behind that many pipelined bytes then surfaces only once
            // the handler finishes and the session consumes those requests.
            while (!closing_ && input_.size() < allowance_) {
                // A quiet SSE needs no look-ahead allocation. After the first
                // application byte, grow like the connection read buffer
                // instead of reserving the full 64 KiB header allowance.
                char first_byte;
                const bool first_read = input_.capacity() == 0;
                if (!first_read && input_.size() == input_.capacity()) {
                    input_.reserve(std::min(
                        allowance_, std::max(initial_read_buffer_bytes, input_.capacity() * 2)));
                }
                const auto space = first_read ? std::span<char>(&first_byte, 1) : input_.prepare_spare();
                // Cancel this operation, not the socket's concurrent writer.
                // Asio's SSL io_op feeds received encrypted input into its
                // engine before honoring read cancellation; it remains there
                // for the session's next read after this task has joined.
                const auto completion = co_await async_asio<std::size_t>([this, space](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(space),
                        asio::bind_cancellation_slot(cancellation_.slot(), std::move(handler)));
                });
                // A terminal cancellation can race a successful read. Preserve
                // every returned byte before inspecting the shutdown state.
                if (first_read) {
                    if (completion.result() != 0) {
                        input_.reserve(std::min(initial_read_buffer_bytes, allowance_));
                        input_.append(std::string_view(&first_byte, completion.result()));
                    }
                } else {
                    input_.commit(completion.result());
                }
                const auto error = completion.error_code();
                if (error) {
                    if (closing_ && error == asio::error::operation_aborted) {
                        co_return;
                    }
                    if (is_inbound_connection_end(error)) {
                        // RFC 9112 section 9.6: within a pipeline -- this request
                        // arrived behind another, or more follow it -- a clean
                        // end (TCP FIN, TLS close_notify) closes only the request
                        // direction; every request received gets its response.
                        // That no longer depends on when the FIN arrives. A
                        // request that arrived alone, a reset, and a TLS stream
                        // truncated without close_notify end the response.
                        if (error == asio::error::eof && (pipelined_ || input_.size() != 0)) {
                            request_input_ended_ = true;
                            co_return;
                        }
                        peer_aborted_ = true;
                        request_stop_.request_stop();
                        co_return;
                    }
                    throw inbound_transport_error(error, "failed to monitor HTTP response stream peer");
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            request_stop_.request_stop();
        }
    }

    stream_type& stream_;
    stop_source& request_stop_;
    task_scope tasks_;
    http_io_buffer input_;
    asio::cancellation_signal cancellation_;
    std::size_t allowance_;
    std::exception_ptr failure_;
    bool pipelined_;
    bool closing_{};
    bool peer_aborted_{};
    bool request_input_ended_{};
    bool joined_{};
};

// Writes a response stream to the connection. The handler runs in the
// session's idle deadman phase (idle_timeout, like a buffered handler); only
// the sink's own socket writes count as `writing`, so write_timeout measures a
// slow reader and not the time a handler spends waiting for its next event.
template <typename stream_type>
class response_stream_sink final {
public:
    // `cors` is the application's CORS policy (null when none is configured);
    // like `request` and `request_body`, it must outlive the sink. `plan`
    // assumes the request body is left unread; `consumed_plan` is the plan for
    // the same request once `request_body` reports its complete consumption,
    // which commit checks so a route that read its body before streaming keeps
    // the connection reusable. `request_stop` is the request's own stop source,
    // which a peer close or a failed write requests; it outlives the sink.
    // `request_pipelined` tells whether the request arrived behind another.
    response_stream_sink(stream_type& stream, worker_codecs& codecs,
        http_response_head_buffer& head, connection_scanner::entry& scanner_entry,
        const worker_ref& worker_value, const http_request& request, const http_cors_policy* cors,
        http1_response_stream_plan plan, http1_response_stream_plan consumed_plan,
        lazy_buffered_body<stream_type>& request_body, http_response_coding_selection response_coding,
        const compression_config* compression,
        stop_source& request_stop, bool request_pipelined) noexcept
        : stream_(stream),
          head_(head),
          trailers_(),
          scanner_entry_(scanner_entry),
          worker_(worker_value),
          request_(request),
          cors_(cors),
          plan_(plan),
          consumed_plan_(consumed_plan),
          request_body_(request_body),
          connection_plan_(plan.request_connection_plan().require_close()),
          compression_(codecs, response_coding, compression),
          request_stop_(request_stop),
          request_pipelined_(request_pipelined) {
        request_body_.reader().observe_completion(this, &arm_peer_monitor_on_body_completion);
    }

    response_stream_sink(stream_type&, worker_codecs&, http_response_head_buffer&,
        connection_scanner::entry&, worker_ref&&, const http_request&, const http_cors_policy*,
        http1_response_stream_plan, http1_response_stream_plan,
        lazy_buffered_body<stream_type>&, http_response_coding_selection,
        const compression_config*, stop_source&, bool) = delete;

    response_stream_sink(const response_stream_sink&) = delete;
    response_stream_sink& operator=(const response_stream_sink&) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    const http_response_stream_commit_plan* commit_plan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted() || (!state_.ended() && peer_aborted());
    }

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

    // Routes the stop requests of the writer's output operations.
    [[nodiscard]] operation_interrupt_source& output_interrupts() & noexcept RUVIA_LIFETIMEBOUND {
        return output_interrupts_;
    }
    operation_interrupt_source& output_interrupts() && = delete;

    // Read-side termination detection, like Go net/http's background read:
    // it starts once the request body is complete, at dispatch or when the
    // handler's read completes it, and never while body bytes are still unread
    // on the transport. A finished stream does not start it.
    void arm_peer_monitor() {
        if (peer_monitor_ || peer_monitor_retired_ || state_.ended() || state_.aborted() ||
            request_body_.consumption() != http1_request_body_consumption::complete) {
            return;
        }
        peer_monitor_.emplace(
            stream_, worker_, request_stop_, request_pipelined_, request_body_.pipeline_size());
        try {
            peer_monitor_->start();
        } catch (...) {
            peer_monitor_.reset();
            throw;
        }
    }

    // Once the handler has returned: no later body consumption may start a
    // monitor, and a running one stops reading. finish_peer_monitor() joins it.
    void retire_peer_monitor() noexcept {
        peer_monitor_retired_ = true;
        request_body_.reader().observe_completion(nullptr, nullptr);
        if (peer_monitor_) {
            peer_monitor_->cancel();
        }
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_monitor_ && peer_monitor_->peer_aborted();
    }

    // Read once finish_peer_monitor() has returned.
    [[nodiscard]] bool request_input_ended() const noexcept {
        return peer_monitor_ && peer_monitor_->request_input_ended();
    }

    [[nodiscard]] bool peer_eof_monitoring() const noexcept {
        return peer_monitor_.has_value();
    }

    task<void> finish_peer_monitor() {
        if (peer_monitor_) {
            co_await peer_monitor_->join();
        }
    }

    [[nodiscard]] std::string_view monitored_pipeline() const noexcept {
        return peer_monitor_ ? peer_monitor_->pipeline() : std::string_view{};
    }

    task<stream_commit_result> commit() {
        if (state_.committed()) {
            throw std::logic_error("response stream is already committed");
        }
        co_await commit_head(http_response_trailer_intent::none);
        co_return state_.ended() ? stream_commit_result::completed : stream_commit_result::body;
    }

    template <typename sink>
    friend task<void> response_stream_write_thunk(void*, std::string_view);
    template <typename sink>
    friend task<void> response_stream_end_thunk(void*, std::span<const http_header_view>);
    template <typename sink>
    friend task<timer_sleep_result> response_stream_sleep_thunk(
        void*, std::chrono::milliseconds, const stop_token&);
    template <typename sink>
    friend void response_stream_bind_context_thunk(void*, context*);
    template <typename sink>
    friend void response_stream_release_context_thunk(void*) noexcept;

private:
    void ensure_peer_writable() const {
        if (peer_monitor_) {
            peer_monitor_->ensure_writable();
        }
    }

    void mark_write_failure(const std::error_code& error) noexcept {
        request_stop_.request_stop();
        if (peer_monitor_) {
            peer_monitor_->write_failed(error);
        }
        state_.mark_aborted();
    }

    [[noreturn]] void fail_write(const std::error_code& error, const char* what) {
        mark_write_failure(error);
        throw inbound_transport_error(error, what);
    }

    void bind_context(context* context_value) {
        state_.bind_context(context_value);
    }

    void release_context() noexcept {
        state_.release_context();
    }

    // The caller's coroutine frame retains the buffers and temporary scanner phase.
    // Return the native awaiter instead of allocating another task frame per write.
    template <typename buffers_type>
    [[nodiscard]] auto write_to_stream(const buffers_type& buffers) {
        return ruvia::async_asio([this, &buffers](auto handler) mutable {
            ensure_peer_writable();
            asio::async_write(stream_, buffers, connection_write_progress(scanner_entry_), std::move(handler));
        });
    }

    // Copies a small TLS frame into the head buffer so it leaves as one
    // record; otherwise `buffers` stays the equivalent gather write. Plain TCP
    // sends the gather write as one segment already.
    void coalesce_tls_frame(std::array<asio::const_buffer, 3>& buffers, std::string_view first,
        std::string_view second, std::string_view third) noexcept {
        if constexpr (!std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            if (first.size() + second.size() + third.size() > response_head_retained_heap_bytes) {
                return;
            }
            head_.reset();
            if (try_coalesce_tls_frame(head_, {first, second, third})) {
                buffers = {asio::buffer(head_.view()), asio::const_buffer{}, asio::const_buffer{}};
            }
        }
    }

    static void arm_peer_monitor_on_body_completion(void* target) {
        static_cast<response_stream_sink*>(target)->arm_peer_monitor();
    }

    task<void> commit_head(http_response_trailer_intent trailer_intent) {
        ensure_peer_writable();
        if (state_.committed()) {
            if (trailer_intent == http_response_trailer_intent::present) {
                state_.ensure_trailers_allowed();
            }
            co_return;
        }

        try {
            auto& context_value = state_.bound_context();
            if (context_access::output_committed(context_value)) {
                throw std::logic_error("the request's tunnel is already established");
            }
            auto* const interim_output = context_access::interim_output(context_value);
            auto response = co_await context_access::commit_streaming_head(context_value);
            validate_response_stream_status(request_, response);
            ensure_peer_writable();
            // The same final-representation order as a buffered response: the
            // application CORS policy first, then content coding.
            if (cors_ != nullptr) {
                (void)cors_->apply_response_headers(request_, response);
            }
            compression_.prepare(request_, response);
            // The request body is settled at commit: read in full by now, the
            // connection stays reusable; anything less closes it after the
            // stream, since the body bytes still in the socket would be read as
            // the next request.
            if (request_body_.consumption() == http1_request_body_consumption::complete) {
                plan_ = consumed_plan_;
            }
            auto stream_head =
                prepare_http1_response_stream_head(std::move(response), plan_, trailer_intent);
            compression_.activate(stream_head.commit_plan().body_plan());

            head_.reset();
            append_http1_response_head(stream_head.response(), head_, stream_head.response_head_plan());
            connection_plan_ = stream_head.connection_plan();
            // Mark committed before the write; a partial header flush must never be
            // followed by the normal error-response path on the same socket.
            if (interim_output != nullptr) {
                interim_output->commit_final();
            }
            state_.mark_committed(stream_head.commit_plan());
            arm_peer_monitor();
        } catch (...) {
            // Representation metadata and protocol framing are one pre-wire
            // transaction. Once either side has failed, no handler retry may
            // manufacture a second head from half-prepared compression state.
            if (!state_.committed()) {
                compression_.abort();
                state_.mark_aborted();
            }
            throw;
        }
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result =
                co_await write_to_stream(asio::buffer(head_.view()));
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream head");
        }
    }

    task<timer_sleep_result>
    sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        state_.ensure_body_allowed();
        co_return co_await sleep_for(worker_, duration, stop_token_value);
    }

    task<void> write(std::string_view chunk) {
        ensure_peer_writable();
        state_.ensure_body_allowed();
        if (chunk.empty()) {
            co_return;
        }

        if (compression_.active()) {
            try {
                co_await compression_.write(
                    chunk, [this](std::string_view encoded) { return write_encoded(encoded); });
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_return;
        }

        co_await write_encoded(chunk);
    }

    task<void> write_encoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }

        const http1_chunk_header chunk_header(chunk.size());
        std::array<asio::const_buffer, 3> buffers{};
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            buffers[0] = asio::buffer(chunk);
        } else {
            buffers = {asio::buffer(chunk_header.view()), asio::buffer(chunk), asio::buffer(http1_chunk_data_terminator)};
            coalesce_tls_frame(buffers, chunk_header.view(), chunk, http1_chunk_data_terminator);
        }
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result = co_await write_to_stream(buffers);
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream body");
        }
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }
        ensure_peer_writable();

        const auto trailer_section = validate_http_response_trailers(trailers);
        const auto trailer_intent = response_trailer_intent(trailer_section);
        http_clear_string_retaining_small(trailers_);
        if (!trailer_section.empty()) {
            append_http1_response_trailers(trailers_, trailer_section);
        }

        co_await commit_head(trailer_intent);
        ensure_peer_writable();
        if (state_.ended()) {
            co_return;
        }
        if (compression_.active()) {
            try {
                compression_.finish();
            } catch (...) {
                state_.mark_aborted();
                throw;
            }
            co_await write_encoded(compression_.output());
            compression_.release_output();
        }
        ensure_peer_writable();
        if (plan_.framing() == http_response_stream_framing::http1_close_delimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.mark_ended();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        std::array<asio::const_buffer, 3> buffers{asio::buffer(http1_last_chunk_prefix),
            asio::buffer(trailers_), asio::buffer(http1_trailer_section_terminator)};
        coalesce_tls_frame(buffers, http1_last_chunk_prefix, trailers_, http1_trailer_section_terminator);
        std::error_code ec;
        {
            const connection_scanner::entry::phase_scope write_phase(
                scanner_entry_, connection_scanner::phase::writing);
            const auto write_result = co_await write_to_stream(buffers);
            ec = write_result.error_code();
        }
        if (ec) {
            fail_write(ec, "failed to write HTTP response stream end");
        }
        state_.mark_ended();
    }

    stream_type& stream_;
    http_response_head_buffer& head_;
    std::string trailers_;
    connection_scanner::entry& scanner_entry_;
    // The connection/server owns an address-stable handle for the complete
    // route dispatch. Streaming must not acquire shared ownership per request.
    const worker_ref& worker_;
    const http_request& request_;
    const http_cors_policy* cors_;
    http1_response_stream_plan plan_;
    http1_response_stream_plan consumed_plan_;
    lazy_buffered_body<stream_type>& request_body_;
    http1_request_connection_plan connection_plan_;
    http_streaming_response_compression compression_;
    response_stream_state state_;
    stop_source& request_stop_;
    bool request_pipelined_;
    std::optional<http1_peer_eof_monitor<stream_type>> peer_monitor_;
    bool peer_monitor_retired_{false};
    operation_interrupt_source output_interrupts_;
};

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included; `pipeline_stash` is
// request-scoped storage for a pipelined request the body reader over-read.
template <typename stream_type>
task<http1_session_request_completion> dispatch_http_response_stream_route(http1_route_dispatch<stream_type> d,
    http_response_head_buffer& response_head, const http1_server_request_head_ready& request_head,
    const resolved_route& resolved, std::string_view request_bytes, protocol_byte_limit body_limit_value,
    std::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);
    http_lazy_buffered_body_route_state<stream_type> body_state;
    const auto body_setup_failure =
        try_prepare_http_lazy_buffered_body_route(body_state, d, body_limit_value, body_and_pipeline);
    if (body_setup_failure != nullptr) {
        co_return co_await complete_failed_http_route(d.scanner_entry_, body_setup_failure, d.parsed_,
            d.routes_, d.base_route_services_, d.response_);
    }
    const auto close_policy = d.request_sequence_.next_response_close_policy();
    // A body that arrived in full with the head does not cost the stream its
    // reuse even if the handler never reads it: completion consumes it
    // without I/O.
    const auto unread_body_consumption =
        buffered_request_body_bytes(d.parsed_.body_plan(), body_and_pipeline, body_limit_value)
            ? http1_request_body_consumption::complete
            : http1_request_body_consumption::incomplete;
    const auto stream_plan = http1_plan_response_stream(d.parsed_, close_policy, unread_body_consumption);
    using response_sink_type = response_stream_sink<stream_type>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().response_stream();
    body_reader stream_body_facade = make_body_reader_facade(body_state.body_->loader().reader());
    if (endpoint.request_body_mode() == request_body_mode::stream) {
        d.base_route_services_.bind_streaming_request_body(stream_body_facade);
        d.base_route_services_.bind_request_trailers(body_state.body().trailers());
    } else {
        d.base_route_services_ = body_state.with_loader(d.base_route_services_);
    }
    // The client closing the connection while the handler waits, or a failed
    // write, stops the request: context::stop_token() combines it with the
    // worker's stop, so every framework wait of the handler observes it.
    stop_source peer_stop;
    const stop_group request_stop(d.base_route_services_.get_stop_token(), peer_stop.token());
    const auto request_stop_token = request_stop.token();
    d.base_route_services_.bind_stop_token(request_stop_token);
    response_sink_type response_sink(d.stream_, *d.options_.codecs_, response_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.parsed_.request(), d.cors_,
        stream_plan,
        http1_plan_response_stream(d.parsed_, close_policy, http1_request_body_consumption::complete),
        body_state.body(), d.response_coding_, d.options_.compression_ ? &*d.options_.compression_ : nullptr,
        peer_stop, d.request_pipelined_);

    // The handler stays in the session's idle deadman phase; the sink times
    // its own writes.
    std::optional<response_stream_dispatch_result> dispatched;
    std::exception_ptr escaped_failure;
    try {
        // This request arrived before the peer ended its request direction:
        // that end is no abandonment of it, and the transport has no input left.
        if (d.request_sequence_.request_input_ended()) {
            response_sink.retire_peer_monitor();
        }
        response_sink.arm_peer_monitor();
        dispatched.emplace(co_await dispatch_response_stream_with(response_sink, d.routes_, d.parsed_.request(),
            resolved, d.base_route_services_,
            [&response_sink] { return response_sink.peer_aborted(); }));
    } catch (...) {
        escaped_failure = std::current_exception();
    }
    // Even error recovery can throw. Drain the reader before the body, sink,
    // stream, or their borrowed request storage may be destroyed or reused.
    response_sink.retire_peer_monitor();
    if (response_sink.peer_eof_monitoring()) {
        try {
            co_await response_sink.finish_peer_monitor();
        } catch (...) {
            const auto monitor_failure = std::current_exception();
            const auto* reported_failure = dispatched ? dispatched->failed_after_commit() : nullptr;
            if (escaped_failure != nullptr || reported_failure != nullptr) {
                const auto prior_failure = escaped_failure != nullptr
                                               ? escaped_failure
                                               : reported_failure->exception();
                if (monitor_failure != prior_failure) {
                    d.options_.connection_failure_.invoke(
                        d.base_route_services_.remote_address(), monitor_failure);
                }
            } else {
                escaped_failure = monitor_failure;
            }
        }
    }
    if (response_sink.request_input_ended()) {
        d.request_sequence_.mark_request_input_ended();
    }
    if (escaped_failure != nullptr) {
        if (!response_sink.committed()) {
            std::rethrow_exception(escaped_failure);
        }
        dispatched.emplace(response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(response_sink), escaped_failure));
    }
    auto& result_value = *dispatched;

    if (const auto* failed = result_value.failed_after_commit()) {
        if (auto* lifecycle = d.base_route_services_.connection_lifecycle_value()) {
            lifecycle->note_exception(failed->exception());
        }
        d.options_.connection_failure_.invoke(
            d.base_route_services_.remote_address(), failed->exception());
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), failed->status());
    }
    // The cancelled read can deliver a peer end after dispatch already
    // returned. Classify only after joining, when its final result is known.
    if (result_value.aborted_before_commit() != nullptr || result_value.aborted_after_commit() != nullptr || response_sink.peer_aborted()) {
        if (!response_sink.committed()) {
            throw inbound_transport_error(asio::error::eof, "HTTP response stream aborted before commit");
        }
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), committed_response_stream_status(response_sink));
    }

    if (auto* recovered = result_value.recovered_failure()) {
        d.base_route_services_.set_connection_close_reason(connection_close_reason::handler_error);
        d.response_ = std::move(*recovered).take_response();
        d.scanner_entry_.touch();
        co_return close_with_framework_response(d.response_, stream_plan.request_connection_plan());
    }
    auto& body_reader_value = body_state.body().reader();
    body_reader_value.consume_buffered_body();
    if (auto* route_response = result_value.route_response()) {
        d.response_ = std::move(*route_response).take_response();
        d.scanner_entry_.touch();
        co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan(),
            d.request_sequence_, body_reader_value.consumption(), request_bytes.size(), pipeline_stash,
            [&body_reader_value, &response_sink](std::string& stash) {
                return body_reader_value.take_pipeline(stash, response_sink.monitored_pipeline());
            });
    }

    // Only the completed committed alternative remains.
    const auto committed_status = result_value.committed_status();

    auto connection_plan = response_sink.connection_plan();
    d.request_sequence_.complete_committed_response(connection_plan);
    // A reusable head promised a body read in full or buffered in full; a
    // read the handler failed after commit breaks that promise, so the
    // connection closes after the stream instead.
    if (body_reader_value.consumption() != http1_request_body_consumption::complete) {
        connection_plan = connection_plan.require_close();
    }
    if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
        co_return http1_session_request_completion::make_committed_stream(
            connection_plan, *committed_status, request_head.header_bytes());
    }
    // What followed the body is the next request, in the read buffer or in
    // the reader's own over-read.
    const auto pipeline = body_reader_value.take_pipeline(pipeline_stash, response_sink.monitored_pipeline());
    if (!pipeline.stashed_.empty()) {
        co_return http1_session_request_completion::make_committed_stream_pipeline_restore(
            connection_plan, *committed_status, pipeline.stashed_);
    }
    co_return http1_session_request_completion::make_committed_stream(
        connection_plan, *committed_status, request_bytes.size() - pipeline.read_buffer_suffix_bytes_);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Serialized output still bounded by one coalesced write. A pipelined
// connection holds small buffered responses back while their successors are
// already buffered, then sends them with the response that ends the run.
inline constexpr std::size_t pipelined_output_batch_bytes = std::size_t{16} * 1024;

// One write of a connection's serialized response output, optionally followed
// by a borrowed body. The output is the work set's head buffer: the current
// response's head, preceded by any pipelined responses held back for this
// write. A write starts with one non-blocking attempt on plain TCP; only a
// remainder suspends, through async_asio at the caller's co_await site so a
// response the socket accepts at once costs no coroutine frame:
//
//     if (!output.start(body)) {
//         auto completion = co_await async_asio<std::size_t>(
//             [&output](auto handler) { output.write_remaining(std::move(handler)); });
//         output.finish(completion.error_code(), completion.result());
//     }
//     const auto written = output.complete();
//
// complete() reports every held-back response whose head the write committed
// and empties the output. The output must be empty before anything else
// writes to the stream (interim heads, streams, upgrades, file bodies) and
// before the session reads or closes.
template <typename stream_type>
class http1_response_output final {
public:
    http1_response_output(stream_type& stream, http_response_head_buffer& output,
        http1_deferred_access_log& deferred_access, const access_log_sink& access_log,
        ip_address remote_address, connection_scanner::entry& progress) noexcept
        : stream_(&stream),
          output_(&output),
          deferred_access_(&deferred_access),
          access_log_(&access_log),
          remote_address_(remote_address),
          progress_(&progress) {}

    http1_response_output(const http1_response_output&) = delete;
    http1_response_output& operator=(const http1_response_output&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return output_->view().empty();
    }

    // Begins writing the output followed by `body`, which stays borrowed until
    // complete(). Returns true when the write already settled.
    // A final plain response half-closes TCP here only after every byte was
    // accepted, so its Linux send hint is flushed before any logging or frame
    // cleanup; the session's lingering close then waits for the peer's end.
    // A partial write falls back to ordinary async_write without that hint.
    // MSVC 19.51 can lose a required coroutine tail call when this synchronous
    // path is inlined into its caller.
    [[nodiscard]]
#if defined(_MSC_VER) && !defined(__clang__)
    __declspec(noinline)
#endif
    bool start(std::string_view body = {}, bool finish_connection = false) noexcept {
        body_ = body;
        written_ = {};
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            const auto flags = finish_connection ? final_plain_tcp_send_flags : 0;
            const auto output = output_->view();
            const bool completed = body.empty()
                                       ? try_plain_tcp_sync_write(*stream_, asio::buffer(output), output.size(),
                                             flags, written_.error_, written_.bytes_)
                                       : try_plain_tcp_sync_write(*stream_,
                                             std::array<asio::const_buffer, 2>{asio::buffer(output), asio::buffer(body)},
                                             output.size() + body.size(), flags, written_.error_, written_.bytes_);
            if (written_.bytes_ != 0) {
                progress_->note_write_progress();
            }
            if (completed && !written_.error_ && finish_connection) {
                shutdown_http_connection_send(stream_->lowest_layer());
            }
            return completed;
        } else {
            return false;
        }
    }

    // Initiates the asynchronous write of what start() left. Every partial
    // transfer touches the scanner entry, so write_timeout measures
    // inactivity rather than the time to drain the whole output.
    template <typename handler_type>
    void write_remaining(handler_type&& handler) {
        progress_->set_phase(connection_scanner::phase::writing);
        async_write_after(*stream_, output_->view(), body_, written_.bytes_, *progress_, std::forward<handler_type>(handler));
    }

    [[nodiscard]] auto async_write_remaining() {
        return async_asio<std::size_t>(
            [this](auto handler) { this->write_remaining(std::move(handler)); });
    }

    // Records the asynchronous remainder's outcome.
    void finish(std::error_code error, std::size_t bytes) noexcept {
        written_.error_ = error;
        written_.bytes_ += bytes;
    }

    // Reports the held-back responses whose heads were written, empties the
    // output and returns the write's outcome.
    [[nodiscard]] connection_write_outcome complete() noexcept {
        deferred_access_->complete(*access_log_, remote_address_, written_.bytes_);
        output_->reset();
        body_ = {};
        return written_;
    }

    // Writes held-back output through a coroutine, for the rare writers
    // (interim heads, file responses) that need the output empty first. A
    // failed write leaves the connection unusable. The caller's scanner phase
    // (a handler's idle deadman, a body read, a request-head read) resumes
    // once the write settles.
    [[nodiscard]] task<std::error_code> flush() {
        if (empty()) {
            co_return std::error_code{};
        }
        if (!start()) {
            const connection_scanner::entry::phase_scope caller_phase(*progress_);
            const auto completion = co_await async_write_remaining();
            finish(completion.error_code(), completion.result());
        }
        co_return complete().error_;
    }

private:
    stream_type* stream_;
    http_response_head_buffer* output_;
    http1_deferred_access_log* deferred_access_;
    const access_log_sink* access_log_;
    ip_address remote_address_{};
    connection_scanner::entry* progress_;
    std::string_view body_;
    connection_write_outcome written_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
// Reads more of a cleartext connection's first bytes, which so far match the
// prior-knowledge client preface (RFC 9113 §3.4), under the request-header
// deadlines. Returns false when the read ends the connection. Once the preface
// is complete, the HTTP/1 connection hands its pending bytes to the HTTP/2
// session and returns its working memory first.
template <typename stream_type>
task<bool> read_more_cleartext_http2_preface(
    stream_type& stream, connection_scanner::entry& scanner_entry, connection_read_buffer& read_buffer) {
    scanner_entry.read_request_head(connection_scanner::phase::reading_initial);
    const auto space = read_buffer.prepare_read();
    auto read_completion = co_await ruvia::async_asio<std::size_t>([&stream, space](auto handler) mutable {
        stream.async_read_some(asio::buffer(space), std::move(handler));
    });
    if (read_completion.error_code()) {
        co_return false;
    }
    read_buffer.commit_read(read_completion.result());
    scanner_entry.touch();
    co_return true;
}

template <typename stream_type>
class http1_interim_response_sink final {
public:
    // `request` is the session's reused parse target; it is borrowed for every
    // dispatch that can reach output(). Pipelined responses still held back in
    // `pending` precede any interim head on the wire.
    http1_interim_response_sink(stream_type& stream, const http_request& request,
        http1_response_output<stream_type>& pending, connection_scanner::entry& scanner_entry)
        : stream_(stream),
          request_(request),
          pending_(pending),
          scanner_entry_(scanner_entry),
          output_(this, write) {}
    [[nodiscard]] http_interim_response_output& output() noexcept {
        return output_;
    }

private:
    static task<void> write(void* raw, const http_interim_response_head& response) {
        auto& self = *static_cast<http1_interim_response_sink*>(raw);
        // A typical interim head fits the head buffer's inline storage; a
        // larger one is encoded again into heap storage of exactly its size.
        http_response_head_buffer head;
        auto encoded = prepare_http1_interim_response_head(
            response, std::span<char>(head.stack_cursor(response_head_stack_bytes), response_head_stack_bytes));
        if (const auto* too_small = std::get_if<http1_interim_response_buffer_too_small>(&encoded)) {
            const auto head_bytes = too_small->required_head_bytes();
            head.append_generated(head_bytes, [&](char* out) noexcept {
                encoded = prepare_http1_interim_response_head(response, std::span<char>(out, head_bytes));
            });
        }
        // Validation threw http1_response_head_error before any buffer was
        // touched; an exactly sized buffer always holds the prepared head.
        const auto* prepared = &std::get<prepared_http1_interim_response>(encoded);
        if (prepared->connection_disposition() != http1_interim_connection_disposition::unchanged) {
            throw std::invalid_argument("application interim response cannot close the exchange");
        }
        if (self.request_.protocol_version() == http_protocol_version::http10) {
            // RFC 9110 §15.2: a server MUST NOT send a 1xx response to an
            // HTTP/1.0 client, which would take it as the final response.
            // Interim heads are advisory, so the exchange simply omits it.
            co_return;
        }
        if (!self.pending_.empty()) {
            if (const auto error = co_await self.pending_.flush()) {
                throw inbound_transport_error(error, "failed to write HTTP/1 responses before an interim response");
            }
        }
        const connection_scanner::entry::write_scope write_stall_clock(self.scanner_entry_);
        const auto completion = co_await async_asio<std::size_t>([&self, bytes = prepared->head()](auto handler) {
            asio::async_write(
                self.stream_, asio::buffer(bytes), connection_write_progress(self.scanner_entry_), std::move(handler));
        });
        if (completion.error_code()) {
            throw inbound_transport_error(completion.error_code(), "failed to write HTTP/1 interim response");
        }
    }
    stream_type& stream_;
    const http_request& request_;
    http1_response_output<stream_type>& pending_;
    connection_scanner::entry& scanner_entry_;
    http_interim_response_output output_;
};

template <typename stream_type>
task<http1_session_request_completion> dispatch_http_stream_body_route(http1_route_dispatch<stream_type> d,
    const http1_server_request_head_ready& request_head, const route_resolution& route_resolution_value,
    protocol_byte_limit body_limit, std::string_view request_bytes, std::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);

    std::exception_ptr exception;
    std::optional<body_reader_binding<stream_body_reader<stream_type>>> body_reader;
    try {
        // The reader's buffers are inbound memory: they count against the
        // connection's and the worker's inbound budgets.
        body_reader.emplace(d.stream_, &d.inbound_buffers_,
            body_and_pipeline, d.parsed_.body_plan(), body_limit, d.scanner_entry_,
            *d.base_route_services_.interim_output(), d.base_route_services_.worker());
        auto services = d.base_route_services_;
        services.bind_streaming_request_body(body_reader->facade());
        services.bind_request_trailers(body_reader->reader().trailers());
        co_await d.routes_.dispatch_buffered_response(d.parsed_.request(), route_resolution_value, document_root_binding::none(), services,
            static_file_selection_mode::identity_only, d.response_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exception_services = d.base_route_services_;
        if (body_reader) {
            exception_services.bind_streaming_request_body(body_reader->facade());
            exception_services.bind_request_trailers(body_reader->reader().trailers());
        }
        co_return co_await complete_failed_http_route(d.scanner_entry_, exception, d.parsed_,
            d.routes_, exception_services, d.response_);
    }

    co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan(),
        d.request_sequence_, request_bytes.size(), pipeline_stash, body_reader->reader());
}

// Admission already validated a CONNECT-UDP head (`udp`). HTTP/1 framing adds
// one rule of its own: a CONNECT request cannot carry request content. A
// successful handshake is access-logged through `tail` once its head reaches
// the transport; a buffered answer is left to the session's ordinary tail.
[[nodiscard]] inline http1_response_head_plan prepare_http1_tunnel_response_head_plan(
    const http_response& head_response, http_protocol_version version, bool udp) {
    if (!udp) {
        return prepare_http1_connect_response_head(head_response, version);
    }
    return prepare_http1_connect_udp_response_head(head_response);
}

template <typename stream_type>
struct http1_tunnel_upgrade_commit final {
    using transport_type = http_socket_tunnel_transport<stream_type>;

    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    bool udp_;
    std::optional<tls_tunnel_output>& tls_output_;

    // An HTTP/1.1 Upgrade (a request stream's accept_tunnel()) becomes the
    // 101 head; CONNECT and CONNECT-UDP keep their own heads.
    [[nodiscard]] task<transport_type> operator()(context& context_value, http_response& head_response) const {
        const auto& request = dispatch_.parsed_.request();
        const auto plan = !udp_ && request.known_method() != http_known_method::connect
                              ? prepare_http1_upgrade_response_head(head_response, request)
                              : prepare_http1_tunnel_response_head_plan(head_response, request.protocol_version(), udp_);
        http_response_head_buffer head;
        append_http1_response_head(head_response, head, plan);
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_http1_upgrade_response_head(dispatch_.stream_,
                dispatch_.parsed_.body_plan().expectations(), head_response.status(), head.view(),
                dispatch_.scanner_entry_);
            ec) {
            throw inbound_transport_error(ec, "CONNECT head write");
        }
        tail_.record_access(dispatch_.options_.access_log_, head_response.status());
        co_return make_transport();
    }

    [[nodiscard]] transport_type make_transport() const {
        if constexpr (requires { dispatch_.stream_.next_layer(); }) {
            tls_output_.emplace(*dispatch_.stream_.native_handle(), dispatch_.stream_.lowest_layer(),
                dispatch_.base_route_services_.worker(), dispatch_.base_route_services_.connection_lifecycle_value());
            tls_output_->start();
        }
        return transport_type(dispatch_.stream_, tls_output_ ? &*tls_output_ : nullptr, &dispatch_.scanner_entry_);
    }

    [[nodiscard]] task<websocket_upgrade_attempt<transport_type>> accept_websocket(
        context& context_value, const websocket_route_endpoint& endpoint, std::span<const http_header_view> headers) const {
        const auto validation = validate_websocket_handshake(dispatch_.parsed_.request(),
            dispatch_.parsed_.body_plan(), endpoint.handshake_validation());
        if (const auto* failure = validation.failure()) {
            co_return reject_request_stream_websocket(context_value, *failure);
        }
        const auto handshake = make_websocket_server_handshake(dispatch_.parsed_.request(),
            {.supported_subprotocols_ = endpoint.subprotocols(), .response_headers_ = headers, .deflate_ = endpoint.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto error = co_await write_websocket_handshake(dispatch_.stream_, handshake,
                dispatch_.parsed_.body_plan().expectations(), dispatch_.scanner_entry_)) {
            throw inbound_transport_error(error, "WebSocket handshake write");
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        co_return websocket_upgrade_commit<transport_type>(
            make_transport(), handshake.compression(), handshake.subprotocol());
    }
};

template <typename stream_type>
[[nodiscard]] task<std::optional<http1_session_request_completion>> dispatch_http_tunnel_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved, bool udp,
    std::string_view pending) {
    if (d.parsed_.body_plan().requires_consumption()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request(),
            http_error_info({.status_ = http_status::bad_request, .message_ = "CONNECT route does not accept HTTP request content"}), d.base_route_services_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan());
    }
    // The pending bytes remain owned by the HTTP input budget until acceptance
    // hands them to the tunnel. A refusal uses the ordinary framed sink; its
    // peer monitor starts only when that refusal is actually committed.
    http_lazy_buffered_body_route_state<stream_type> body_state;
    if (const auto setup_failure = try_prepare_http_lazy_buffered_body_route(
            body_state, d, protocol_byte_limit::unlimited(), pending)) {
        std::rethrow_exception(setup_failure);
    }
    stop_source peer_stop;
    const stop_group request_stop(d.base_route_services_.get_stop_token(), peer_stop.token());
    const auto request_stop_token = request_stop.token();
    d.base_route_services_.bind_stop_token(request_stop_token);
    http_response_head_buffer refusal_head;
    const auto refusal_plan = http1_plan_response_stream(d.parsed_, http1_close_policy::close_after_response,
        http1_request_body_consumption::complete);
    response_stream_sink<stream_type> refusal_sink(d.stream_, *d.options_.codecs_, refusal_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.parsed_.request(), d.cors_, refusal_plan, refusal_plan, body_state.body(),
        d.response_coding_, d.options_.compression_ ? &*d.options_.compression_ : nullptr,
        peer_stop, d.request_pipelined_);
    auto refusal_writer = make_response_stream_writer(refusal_sink);
    if (is_request_stream_tunnel_route(resolved.route())) {
        d.base_route_services_.bind_response_stream(refusal_writer);
    }
    std::optional<tls_tunnel_output> tls_output;
    // The TLS output pump must be stopped and joined on every exit, including
    // a handler failure that execute_tunnel_upgrade_session surfaces.
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    const http1_tunnel_upgrade_commit<stream_type> commit{
        .dispatch_ = d,
        .tail_ = tail,
        .udp_ = udp,
        .tls_output_ = tls_output,
    };
    auto commit_websocket = [&commit](context& context_value, const websocket_route_endpoint& endpoint,
                                std::span<const http_header_view> headers) {
        return commit.accept_websocket(context_value, endpoint, headers);
    };
    try {
        buffered = co_await execute_tunnel_upgrade_session<http_socket_tunnel_transport<stream_type>>(
            d.routes_, d.parsed_.request(), resolved, d.base_route_services_,
            d.options_, d.scanner_entry_, d.inbound_buffers_, udp, pending,
            commit, commit_websocket);
    } catch (...) {
        failure = std::current_exception();
    }
    refusal_sink.retire_peer_monitor();
    try {
        co_await refusal_sink.finish_peer_monitor();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (tls_output) {
        if (failure) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (refusal_sink.committed()) {
        co_return http1_session_request_completion::make_committed_stream(
            refusal_sink.connection_plan().require_close(), committed_response_stream_status(refusal_sink), 0);
    }
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan());
    }
    co_return std::nullopt;
}

// A rejected upgrade returns the exact HTTP/1 request completion that the
// session must write and clean up. A successful upgrade is access-logged
// through `tail` once its handshake reaches the transport, then transfers
// transport ownership to the websocket session, so no HTTP request completion
// remains.
template <typename stream_type>
struct http1_websocket_upgrade_commit final {
    http1_route_dispatch<stream_type>& dispatch_;
    http_response_tail& tail_;
    const websocket_route_endpoint& websocket_endpoint_;

    [[nodiscard]] task<websocket_upgrade_commit<stream_type>> operator()(
        context& context_value, std::span<const http_header_view> response_headers_value) const {
        const auto handshake = make_websocket_server_handshake(dispatch_.parsed_.request(),
            {.supported_subprotocols_ = websocket_endpoint_.subprotocols(),
                .response_headers_ = response_headers_value,
                .deflate_ = websocket_endpoint_.deflate()});
        context_access::commit_upgrade(context_value);
        if (const auto ec = co_await write_websocket_handshake(dispatch_.stream_, handshake,
                dispatch_.parsed_.body_plan().expectations(), dispatch_.scanner_entry_);
            ec) {
            throw inbound_transport_error(ec, "WebSocket handshake write");
        }
        tail_.record_access(dispatch_.options_.access_log_, http_status::switching_protocols);
        co_return websocket_upgrade_commit<stream_type>(
            dispatch_.stream_, handshake.compression(), handshake.subprotocol());
    }
};

template <typename stream_type>
task<std::optional<http1_session_request_completion>> dispatch_http_websocket_route(
    http1_route_dispatch<stream_type> d, http_response_tail& tail, const resolved_route& resolved,
    std::string_view pending_frames) {
    const auto& websocket_endpoint = *resolved.route().endpoint().websocket();
    const auto handshake_validation = validate_websocket_handshake(
        d.parsed_.request(), d.parsed_.body_plan(), websocket_endpoint.handshake_validation());
    if (const auto* failure = handshake_validation.failure()) {
        // A generated rejection does not inherit the negotiated coding promise.
        tail.disable_response_coding();
        d.response_ = co_await d.routes_.handle_error(d.parsed_.request(),
            http_error_info({.status_ = failure->protocol_error().status(), .code_ = failure->protocol_error().code(), .message_ = std::string_view(failure->protocol_error().what())}),
            d.base_route_services_);
        failure->apply_required_response_headers(d.response_);
        co_return close_with_framework_response(d.response_, d.parsed_.connection_plan());
    }
    auto buffered = co_await execute_websocket_upgrade_session<stream_type>(
        d.routes_, d.parsed_.request(), resolved, d.base_route_services_,
        d.options_, d.scanner_entry_, d.inbound_buffers_, pending_frames,
        http1_websocket_upgrade_commit<stream_type>{
            .dispatch_ = d,
            .tail_ = tail,
            .websocket_endpoint_ = websocket_endpoint,
        });
    if (buffered) {
        d.response_ = std::move(*buffered);
        co_return finalize_closing_route_response(d.response_, d.parsed_.connection_plan());
    }
    co_return std::nullopt;
}

// RFC 9112 §3.2.2: an absolute-form target's authority is the request's host,
// and an HTTP/1.0 request may carry it without any Host field.
[[nodiscard]] inline std::string_view auto_https_redirect_authority(const http_request& request) noexcept {
    if (!request.authority().empty()) {
        return request.authority();
    }
    return request.header("Host").value_or(std::string_view{});
}

inline http_response make_auto_https_redirect_response(
    const http_request& request, std::uint16_t https_port) {
    http_response response;
    response.status(ruvia::http_status::permanent_redirect);

    const auto authority = parse_http_authority(auto_https_redirect_authority(request));
    const auto host = authority ? authority->host() : std::string_view{};
    auto path = request.path();
    if (path.empty() || path.front() != '/') {
        path = "/";
    }

    // The raw target retains the distinction between an absent query and an
    // empty query ("/x" versus "/x?"); query_string() alone cannot express it.
    const bool has_query = (request.target().find('?') != std::string_view::npos);
    const auto query = request.query_string();
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = https_port == 443 ? std::string_view{}
                                               : http_format_unsigned(port_buffer, https_port);
    static constexpr std::string_view scheme_prefix = "https://";
    const std::size_t total_size = scheme_prefix.size() + host.size() +
                                   (port_digits.empty() ? 0U : 1U + port_digits.size()) +
                                   path.size() + (has_query ? 1U + query.size() : 0U);
    std::string location;
    location.reserve(total_size);
    location.append(scheme_prefix);
    location.append(host);
    if (!port_digits.empty()) {
        location.push_back(':');
        location.append(port_digits);
    }
    location.append(path);
    if (has_query) {
        location.push_back('?');
        location.append(query);
    }
    response.header("Location", location);
    // The Location is derived from the request's Host header, so this redirect
    // varies by Host. Mark it private so a shared cache never stores one Host's
    // redirect and serves it for another (a Host-header cache-poisoning open
    // redirect); a browser still caches it per-origin, keeping the HTTPS memory.
    response.header_stable_view("Cache-Control", "private");
    return response;
}

[[nodiscard]] inline std::optional<http_request_body_failure> content_length_limit_failure(
    const http1_request_body_plan& body_plan, protocol_byte_limit limit) noexcept {
    const auto* known_length = body_plan.known_length();
    return known_length == nullptr ? std::nullopt
                                   : http_request_body_size_failure(known_length->content_length(), limit);
}
// Whether a pipelined request can be served while earlier responses wait in
// the connection's output: its handling neither reads the socket (no request
// content, hence no 100 Continue) nor writes to it ahead of its own buffered
// response (no stream, tunnel or upgrade). Interim heads flush the output
// themselves.
[[nodiscard]] inline bool http1_request_follows_held_output(
    const http1_server_request_parse_state& parsed_value, const route_resolution& resolution) noexcept {
    if (parsed_value.body_plan().requires_consumption()) {
        return false;
    }
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        return true;
    }
    const auto* buffered = resolved->route().endpoint().buffered();
    return buffered != nullptr && buffered->request_body_mode() == request_body_mode::buffered;
}

// How a failed request-head read closes. A clean peer end (FIN, or TLS
// close_notify), or a timeout that the scanner signalled by cancelling the
// read, ends a connection that owes no response: TLS answers with
// close_notify. Any other failure closes at once.
[[nodiscard]] inline http_connection_close http1_request_head_read_close(
    const std::error_code& error, const connection_scanner::entry& scanner_entry) noexcept {
    return error == asio::error::eof || scanner_entry.close_requested() ? http_connection_close::notify
                                                                        : http_connection_close::abort;
}

// What every request of one connection shares: the resident state run_stream
// keeps for the connection's whole life. serve_requests borrows it while the
// connection is active.
template <typename stream_type>
struct http1_connection_session final {
    http_server_session_config& listener_;
    stream_type& stream_;
    connection_scanner::entry& scanner_entry_;
    // The connection's route services, bound to its inbound buffer budget.
    context_services& route_services_;
    inbound_buffer_account& inbound_buffers_;
    http1_request_sequence& request_sequence_;
    const http_request_admission_policy& admission_policy_;
    const response_file_io& file_output_;
    std::string_view remote_address_;
    // nginx-aligned wait semantics. The first request on a connection, and any
    // partially-received request header, are bounded by request_header_timeout
    // (reading_initial). The idle wait for the *next* request on a reused
    // keep-alive connection -- no bytes of it received yet -- is bounded by
    // idle_timeout (idle), matching nginx keepalive_timeout. Flips true
    // once a request has been served so later idle waits use the keepalive
    // deadline instead of the header deadline.
    bool served_keepalive_request_{false};
};

[[nodiscard]] inline std::exception_ptr try_start_buffered_response_write(
    http_response_head_buffer& response_head, const http_response& response,
    const http1_buffered_response_plan& planned_response, bool next_request_buffered,
    std::optional<http1_response_write_start>& started_write, bool coalesce_for_tls = false) {
    const auto held_output_bytes = response_head.view().size();
    try {
        started_write.emplace(start_response_write(response_head, response, planned_response,
            next_request_buffered ? pipelined_output_batch_bytes : 0, coalesce_for_tls));
        return nullptr;
    } catch (...) {
        if (response_head.view().size() != held_output_bytes) {
            // Never flush an orphan head or append an error response as
            // its body. Propagate the real failure and abort this connection.
            throw;
        }
        return std::current_exception();
    }
}

template <typename stream_type>
task<http_connection_close> worker_connections::run_stream(http_server_session_config& listener_value, stream_type& stream,
    tcp_socket_type& socket, context_services base_route_services) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the
    // inbound buffer budget. Everything a request needs beyond that is held
    // only while the connection is actively serving and given back the moment
    // it goes idle, so an idle keep-alive connection holds none of it: the
    // heavy working set (read buffer, request arena, parse result, response
    // head) is allocated from the worker heap, and the request state lives in
    // the frame of serve_requests.
    connection_scanner::entry scanner_entry;
    scanner_entry.bind_lifecycle(base_route_services.connection_lifecycle_value());
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    const auto admission_policy = http_request_admission_policy::server(options_);
    const response_file_io file_output{
        .worker_ = worker_,
        .stop_ = stop_token_,
        .timeout_ = options_.server_.write_timeout_,
    };
    inbound_buffer_account connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    base_route_services = base_route_services.with_inbound_buffers(connection_buffers);
    base_route_services.bind_routes(routes_);
    http1_request_sequence request_sequence(options_.server_.max_requests_per_connection_, state_);
    http1_connection_session<stream_type> session{
        .listener_ = listener_value,
        .stream_ = stream,
        .scanner_entry_ = scanner_entry,
        .route_services_ = base_route_services,
        .inbound_buffers_ = connection_buffers,
        .request_sequence_ = request_sequence,
        .admission_policy_ = admission_policy,
        .file_output_ = file_output,
        .remote_address_ = base_route_services.remote_address(),
    };
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, idle_resident_read_bytes> idle_read_buffer;

    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
        enable_plain_tcp_sync_write(socket);
    }
    for (;;) {
        // Allocate-on-use / release-on-idle for the whole work set: the
        // connection holds none while it waits for its next request, and reads
        // that request's first bytes into the small connection-resident buffer
        // instead, so an idle keep-alive connection occupies no work set
        // (memory scales with in-flight requests, not total connections).
        // Reading directly -- rather than a bufferless readiness wait -- costs
        // no extra reactor pass when data is already queued. serve_requests
        // returns only once no input is buffered: a pipelined burst stays in
        // it. TLS reads through the stream as well, so plaintext the engine
        // already decrypted completes the read at once; whatever exceeds the
        // landing pad stays in the engine for the next read.
        //
        // A stopping worker has already closed the connection; it starts no
        // further read.
        if (!http_server_worker_running(state_)) {
            co_return http_connection_close::notify;
        }
        // Every request received before the peer ended its request direction
        // has been served; the transport only repeats that end.
        if (request_sequence.request_input_ended()) {
            co_return http_connection_close::notify;
        }
        // Idle wait for the next keep-alive request uses idle_timeout;
        // the connection's first request uses request_header_timeout.
        scanner_entry.read_request_head(session.served_keepalive_request_
                                            ? connection_scanner::phase::idle
                                            : connection_scanner::phase::reading_initial);
        auto idle_completion =
            co_await ruvia::async_asio<std::size_t>([&stream, &idle_read_buffer](auto handler) mutable {
                stream.async_read_some(asio::buffer(idle_read_buffer), std::move(handler));
            });
        const auto idle_ec = idle_completion.error_code();
        const auto idle_bytes = idle_completion.result();
        // A clean peer end or timeout is answered as in the request read loop.
        if (idle_ec) {
            co_return http1_request_head_read_close(idle_ec, scanner_entry);
        }
        auto work_set = make_connection_work_set();
        static_assert(idle_resident_read_bytes <= initial_read_buffer_bytes);
        work_set->read_buffer_.assign(std::string_view(idle_read_buffer.data(), idle_bytes));
        if (idle_bytes == idle_read_buffer.size()) {
            if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
                if (stream.non_blocking()) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (!read_space.empty()) {
                        std::error_code extra_ec;
                        const auto extra_bytes = stream.read_some(asio::buffer(read_space), extra_ec);
                        if (!extra_ec && extra_bytes != 0) {
                            work_set->read_buffer_.commit_read(extra_bytes);
                        }
                    }
                }
            } else {
                for (int pending = SSL_pending(stream.native_handle()); pending > 0;
                    pending = SSL_pending(stream.native_handle())) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (read_space.empty()) {
                        break;
                    }
                    std::size_t extra_bytes = 0;
                    const auto to_read = (std::min)(read_space.size(), static_cast<std::size_t>(pending));
                    if (SSL_read_ex(stream.native_handle(), read_space.data(), to_read, &extra_bytes) != 1 ||
                        extra_bytes == 0) {
                        break;
                    }
                    work_set->read_buffer_.commit_read(extra_bytes);
                }
            }
        }
        scanner_entry.touch();
        const auto served = co_await serve_requests(session, *work_set);
        if (const auto* close_mode = std::get_if<http_connection_close>(&served)) {
            co_return *close_mode;
        }
#if defined(RUVIA_ENABLE_HTTP2)
        if (std::get<http1_serve_handoff>(served) == http1_serve_handoff::cleartext_http2) {
            // The session consumes the buffered input in its first feed, before
            // it suspends, and reads into its own buffer afterwards: the HTTP/1
            // working memory is released before entering the HTTP/2 session.
            const std::string pending_input(work_set->read_buffer_.pending());
            work_set.reset();
            // The HTTP/1 request channels pointed into serve_requests' frame.
            auto http2_services = base_route_services;
            http2_services.bind_request_channels({});
            co_return co_await run_http2_sans_io_session(stream, routes_,
                http2_sans_io_session_context(std::move(http2_services), options_, scanner_entry), pending_input);
        }
#endif
    }
}

// Serves the connection's requests while it has input, and returns
// await_request once it holds no buffered bytes (run_stream then destroys the
// work set and waits for the next request), cleartext_http2 on a complete
// prior-knowledge preface, or how the connection closes. One frame per active
// period: a pipelined burst stays in it.
template <typename stream_type>
task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests(
    http1_connection_session<stream_type>& session, connection_work_set& work_set) {
    auto& stream = session.stream_;
    auto& listener_value = session.listener_;
    auto& scanner_entry = session.scanner_entry_;
    auto& connection_buffers = session.inbound_buffers_;
    auto& request_sequence = session.request_sequence_;
    const auto& routes_value = routes_;
    auto& base_route_services = session.route_services_;
    const auto& admission_policy = session.admission_policy_;
    const auto& file_output = session.file_output_;
    const auto remote_address = session.remote_address_;
    constexpr bool plain_tcp = std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>;
    const auto* redirect = plain_tcp ? listener_value.redirect() : nullptr;
    auto& read_buffer = work_set.read_buffer_;
    auto& parsed_value = work_set.parsed_;
    auto& response_head = work_set.response_head_;
    auto& route_resolution_value = work_set.route_resolution_;
    // The first request of an active period arrived alone; every later one was
    // already (at least partly) buffered behind its predecessor: pipelined.
    bool request_pipelined = false;
    for (;;) {
        // Pipelined responses held back for one coalesced write. Bound to the
        // work set, which keeps any held output until it is written.
        http1_response_output<stream_type> response_output(stream, response_head,
            work_set.deferred_access_, options_.access_log_,
            base_route_services.get_conn_info().remote().address(), scanner_entry);

        // Work-set storage outlives this arena. Release its descriptor block
        // on every exit (including exceptions/cancellation) before the arena dies.
        struct request_header_lifetime final {
            http1_server_request_parse_state& parsed_;
            ~request_header_lifetime() {
                (void)std::move(parsed_).take_request();
            }
        } header_lifetime{parsed_value};
        http_response response;
        // Holds the next pipelined request when a body reader over-read it into
        // its own buffer, which dies with the route, until the read buffer is
        // cleaned up below. Declared before request_completion, which borrows
        // it; a suffix still in the read buffer is compacted in place instead.
        std::string pipeline_stash;
        http1_interim_response_sink interim_sink(
            stream, parsed_value.request(), response_output, scanner_entry);
        base_route_services.bind_interim_output(interim_sink.output());
        std::optional<http_request_admission> admission;
        std::optional<http1_session_request_completion> request_completion;
        // Rejections funnel through one co_await site after the read loop:
        // every co_await expression in a coroutine reserves its own frame
        // slots for the call's temporaries (GCC does not overlap them), so
        // inlining handle_error at each rejection site costs ~660 resident
        // bytes per site in every active connection's frame.
        std::optional<http_request_rejection> request_rejection;
        std::size_t header_search_offset = 0;
        // Request timing starts with the request's first bytes: the idle wait
        // above already has them; a pipelined request's arrived earlier.
        const auto request_start = options_.access_log_.callback_ ? std::chrono::steady_clock::now()
                                                                  : std::chrono::steady_clock::time_point{};
        for (;;) {
#if defined(RUVIA_ENABLE_HTTP2)
            if constexpr (plain_tcp) {
                // The client preface must be the first bytes of the connection
                // (RFC 9113 section 3.4). auto_https reserves the cleartext
                // listener for HTTP/1 redirects, and a connection that already
                // completed an HTTP/1 exchange stays HTTP/1, so both refuse
                // prior-knowledge HTTP/2.
                const auto preface_probe =
                    session.served_keepalive_request_ || redirect != nullptr
                        ? http2_cleartext_preface_probe::http1
                        : probe_http2_cleartext_preface(read_buffer.pending());
                if (preface_probe == http2_cleartext_preface_probe::complete_preface) {
                    co_return http1_serve_handoff::cleartext_http2;
                }
                if (preface_probe == http2_cleartext_preface_probe::drop_connection) {
                    base_route_services.set_connection_close_reason(connection_close_reason::protocol_error);
                    co_return http_connection_close::abort;
                }
                if (preface_probe == http2_cleartext_preface_probe::need_more_preface) {
                    if (!co_await read_more_cleartext_http2_preface(stream, scanner_entry, read_buffer)) {
                        co_return http_connection_close::abort;
                    }
                    continue;
                }
            }
#endif
            const auto buffer_view = read_buffer.pending();
            parsed_value.parse_head(buffer_view, header_search_offset);
            const auto* const ready_head = parsed_value.head_ready();
            if (ready_head != nullptr) {
                route_resolution_value = base_route_services.request_stream()
                                             ? resolve_request_stream_route(http1_request_stream_tunnel(
                                                   parsed_value.request(), parsed_value.body_plan().requires_consumption()))
                                             : routes_value.resolve(parsed_value.request());
            }
            // Held-back pipelined responses leave before anything that could
            // make the peer wait for them or reach the stream ahead of them:
            // a socket read, a rejection that closes, request content, or a
            // streamed, tunnelled or upgraded response.
            if (!response_output.empty() &&
                (ready_head == nullptr || !http1_request_follows_held_output(parsed_value, route_resolution_value))) {
                if (co_await response_output.flush()) {
                    co_return http_connection_close::abort;
                }
                scanner_entry.set_phase(connection_scanner::phase::idle);
            }
            if (const auto* request_head = ready_head) {
                base_route_services.note_connection_request();
                // Reset phase so request_header_timeout stops counting against dispatch
                // time. Body readers will set reading_payload on their own; upgraded
                // sessions enter long_lived; a response stream's sink times its own
                // writes; the buffered write path sets writing before responding.
                // Until one of those transitions, idle_timeout governs as the
                // deadman switch for hung handlers.
                scanner_entry.set_phase(connection_scanner::phase::idle);
                const auto expectation_plan =
                    parsed_value.body_plan().expectation_plan(http_unsupported_expectation_policy::reject);
                request_body_state initial_body;
                if (const auto* known = parsed_value.body_plan().known_length()) {
                    initial_body.framing_ = request_body_framing::known_length;
                    initial_body.content_length_ = known->content_length();
                } else if (parsed_value.body_plan().chunked() != nullptr) {
                    initial_body.framing_ = request_body_framing::chunked;
                }
                base_route_services.bind_body_state(initial_body);
                admission.emplace(admit_http_request(
                    {
                        .request_ = parsed_value.request(),
                        .resolution_ = route_resolution_value,
                        .coding_ = http_response_coding_selection_for(parsed_value.request()),
                        .expectation_ = &expectation_plan,
                        .start_ = request_start,
                    },
                    admission_policy, base_route_services));

                if (redirect != nullptr) {
                    if (admission->endpoint() == http_request_endpoint::rejection) {
                        request_rejection = *admission->rejection();
                        break;
                    }
                    if (auto_https_redirect_authority(parsed_value.request()).empty()) {
                        request_rejection = http_request_rejection::error(http_error_info(
                            {.status_ = ruvia::http_status::bad_request, .message_ = "missing Host header"}));
                        break;
                    }
                    response = make_auto_https_redirect_response(
                        parsed_value.request(), redirect->https_port_);
                    if (http_response_needs_not_acceptable(
                            admission->tail().coding_policy(), parsed_value.request(), response)) {
                        request_rejection =
                            http_request_rejection::error(http_error_info(http_not_acceptable_coding_error()));
                        break;
                    }
                    request_completion.emplace(close_with_framework_response(response, parsed_value.connection_plan()));
                    scanner_entry.touch();
                    break;
                }
                if (const auto* rejection = admission->rejection()) {
                    request_rejection = *rejection;
                    break;
                }
                if (const auto body_failure =
                        content_length_limit_failure(parsed_value.body_plan(), admission->body_limit())) {
                    request_rejection = http_request_rejection::error([&] { const auto err = http_request_body_protocol_error(*body_failure); return http_error_info({.status_ = err.status(), .code_ = err.code(), .message_ = std::string_view(err.what())}); }());
                    break;
                }

                const auto* resolved = admission->resolved();
                if (resolved == nullptr) {
                    co_await routes_value.dispatch_buffered_response(parsed_value.request(),
                        route_resolution_value, options_.document_root_.binding(),
                        admission->services(), admission->static_file_mode(), response);
                    // An unresolved request never reads its body, whether the
                    // shared Web dispatch selected a document-root file, 404,
                    // 405, or OPTIONS response.
                    request_completion.emplace(finalize_unread_body_response(response, parsed_value,
                        *request_head, buffer_view, request_sequence, admission->body_limit()));
                    scanner_entry.touch();
                    break;
                }

                // One bundle of what every route dispatch below needs from
                // this session; each dispatcher adds only its own arguments.
                const http1_route_dispatch<stream_type> route_dispatch{
                    .stream_ = stream,
                    .scanner_entry_ = scanner_entry,
                    .parsed_ = parsed_value,
                    .response_coding_ = *admission->tail().coding_policy().selection(),
                    .cors_ = admission->tail().cors(options_),
                    .routes_ = routes_value,
                    .base_route_services_ = admission->services(),
                    .options_ = options_,
                    .response_ = response,
                    .request_sequence_ = request_sequence,
                    .inbound_buffers_ = connection_buffers,
                    .request_pipelined_ = request_pipelined,
                };

                const auto endpoint_kind = admission->endpoint();
                if (endpoint_kind == http_request_endpoint::tunnel ||
                    endpoint_kind == http_request_endpoint::websocket) {
                    const auto pending = buffer_view.substr(request_head->header_bytes());
                    auto completion = endpoint_kind == http_request_endpoint::tunnel
                                          ? co_await dispatch_http_tunnel_route(route_dispatch, admission->tail(),
                                                *resolved, admission->udp_tunnel(), pending)
                                          : co_await dispatch_http_websocket_route(
                                                route_dispatch, admission->tail(), *resolved, pending);
                    if (!completion) {
                        co_return endpoint_kind == http_request_endpoint::websocket&& stream.lowest_layer().is_open()
                            ? http_connection_close::linger
                            : http_connection_close::abort;
                    }
                    request_completion.emplace(std::move(*completion));
                    break;
                }

                if (endpoint_kind == http_request_endpoint::response_stream) {
                    request_completion.emplace(co_await dispatch_http_response_stream_route(route_dispatch,
                        response_head, *request_head, *resolved, buffer_view, admission->body_limit(),
                        pipeline_stash));
                    // The stream serialized its own head into the (then empty)
                    // output buffer; it is not response output to keep.
                    response_head.reset();
                    break;
                }
                if (endpoint_kind == http_request_endpoint::stream_body) {
                    request_completion.emplace(co_await dispatch_http_stream_body_route(route_dispatch,
                        *request_head, route_resolution_value, admission->body_limit(), buffer_view,
                        pipeline_stash));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const bool has_body = parsed_value.body_plan().requires_consumption();
                    http_lazy_buffered_body_route_state<stream_type> body_state;
                    if (has_body) {
                        const auto body_and_pipeline = http_body_and_pipeline(*request_head, buffer_view);

                        // The body reader/loader setup can throw (e.g. constructing a
                        // transfer-coding decoder for a bad Transfer-Encoding), so it
                        // stays guarded. The dispatch turns any handler or routing
                        // failure into a response, so it sits outside the guard; the
                        // one exception it lets through is an inbound_transport_error
                        // (an interim head or 100 Continue the peer never received),
                        // which ends the connection through the session's caller.
                        if (const auto body_setup_exception = try_prepare_http_lazy_buffered_body_route(
                                body_state, route_dispatch, admission->body_limit(), body_and_pipeline)) {
                            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                                body_setup_exception, parsed_value, routes_value,
                                admission->services(), response));
                            break;
                        }
                    }

                    std::optional<context_services> body_route_services;
                    if (has_body) {
                        body_route_services.emplace(body_state.with_loader(admission->services()));
                    }
                    const auto& route_services =
                        body_route_services ? *body_route_services : admission->services();
                    (void)co_await routes_value.dispatch_resolved_route(
                        parsed_value.request(), *resolved, route_services, nullptr, response);

                    if (has_body) {
                        request_completion.emplace(complete_successful_http_body_route(scanner_entry,
                            response, parsed_value.connection_plan(), request_sequence,
                            buffer_view.size(), pipeline_stash, body_state.body().reader()));
                    } else {
                        scanner_entry.touch();
                        request_completion.emplace(finalize_buffered_route_response(
                            response, parsed_value.connection_plan(), request_sequence, request_head->header_bytes()));
                    }
                    break;
                }
            }

            if (const auto* failure = parsed_value.failure()) {
                base_route_services.set_connection_close_reason(connection_close_reason::protocol_error);
                if constexpr (plain_tcp) {
                    if (redirect == nullptr &&
                        ruvia::should_drop_invalid_cleartext_http1_input(buffer_view, failure->source())) {
                        co_return http_connection_close::abort;
                    }
                }
                const auto error = failure->protocol_error();
                request_rejection = http_request_rejection::error(
                    copy_http_protocol_error_info(error));
                break;
            }

            header_search_offset = buffer_view.size();

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idle_timeout); once any header bytes are
            // buffered, or on the first request, request_header_timeout governs.
            // Either way no request is admitted yet, and a stopping worker
            // (which already closed the connection) starts no read for one.
            if (!http_server_worker_running(state_)) {
                co_return http_connection_close::notify;
            }
            // The peer ended its request direction behind the requests already
            // buffered: nothing more arrives, as a clean peer end would report.
            if (request_sequence.request_input_ended()) {
                co_return http_connection_close::notify;
            }
            scanner_entry.read_request_head(connection_scanner::phase::reading_initial);
            const auto read_space = read_buffer.prepare_read();
            if (read_space.empty()) {
                // The buffer only fills at max_http_header_bytes, where
                // parse_head has already classified and rejected the head.
                throw std::logic_error("HTTP/1 request head filled the read buffer unrejected");
            }

            auto read_completion =
                co_await ruvia::async_asio<std::size_t>([&stream, read_space](auto handler) mutable {
                    stream.async_read_some(asio::buffer(read_space), std::move(handler));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec) {
                // A clean peer end, or a timeout that cancelled the read, is
                // answered with this side's close alert; anything else closes
                // immediately, as does a stopping worker.
                co_return http1_request_head_read_close(ec, scanner_entry);
            }
            read_buffer.commit_read(bytes_read);
            scanner_entry.touch();
        }

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch. A request rejected before admission (malformed
        // head) runs with the connection's services and no coding promise.
        // A rejection is a generated terminal error: it must not inherit the
        // negotiated coding promise, or a client that accepts no coding would
        // see it replaced by a 406 through a second error-handler call.
        // A rate-limited request is well framed: like a 404 it keeps the
        // connection when its body is already in hand. Every other rejection
        // closes it.
        if (request_rejection) {
            if (admission) {
                admission->tail().disable_response_coding();
            }
            response = co_await respond_http_request_rejection(routes_value, parsed_value.request(), *request_rejection,
                admission ? admission->services() : base_route_services);
            const auto* rejected_head = parsed_value.head_ready();
            request_completion.emplace(
                request_rejection->rate_limit() != nullptr && rejected_head != nullptr
                    ? finalize_unread_body_response(response, parsed_value, *rejected_head,
                          read_buffer.pending(), request_sequence, admission->body_limit())
                    : close_with_framework_response(response, parsed_value.connection_plan()));
        }

        if (!request_completion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        if (const auto* rejected = request_completion->rejected_final_response()) {
            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                std::make_exception_ptr(rejected->exception()), parsed_value, routes_value, admission->services(), response));
        }
        const auto peer_ip = base_route_services.get_conn_info().remote().address();
        http_response_tail fallback_tail(parsed_value.request(),
            http_response_coding_policy::disabled(), peer_ip, peer_ip, request_start);
        auto& tail = admission ? admission->tail() : fallback_tail;
        auto connection_plan = request_completion->connection_plan();
        if (request_completion->buffered_response() != nullptr) {
            // Preparation may still run handler-side work (error recovery,
            // offloaded compression); it stays timed as idle. Only the socket
            // writes below run as `writing` (write_remaining() sets it).
            const auto& services = admission ? admission->services() : base_route_services;
            std::optional<http1_buffered_response_plan> planned_response;
            std::optional<http1_response_write_start> started_write;
            bool file_response = false;
            bool head_failure_recovered = false;
            std::optional<http1_file_response_body<stream_type>> file_body;
            for (;;) {
                const auto prepared = co_await tail.prepare_or_finish(
                    response, options_, routes_value, services);
                if (prepared.recovered_) {
                    connection_plan = http1_commit_final_response(response, connection_plan);
                    request_completion = request_completion->with_buffered_connection_plan(connection_plan);
                    connection_plan = request_completion->connection_plan();
                }
                planned_response.emplace(get_http1_buffered_response_plan(prepared.write_plan_, connection_plan));
                file_response = response.file_body() || response.has_multipart_file_body();
                if (file_response && !response_output.empty()) {
                    if (co_await response_output.flush()) {
                        co_return http_connection_close::abort;
                    }
                }
                // A response waits in the output only while the connection stays
                // open and already holds its next request, so the response that
                // ends a pipelined run carries the held ones in one write.
                const auto* const compaction = request_completion->buffer_completion().compaction();
                const bool next_request_buffered = connection_plan.disposition() == http1_close_policy::allow_reuse &&
                                                   compaction != nullptr &&
                                                   compaction->consumed_bytes() < read_buffer.pending().size() &&
                                                   http_server_worker_running(state_);
                // Head validation fails before touching output. Body coalescing can
                // fail after appending a head, leaving an incomplete buffered message.
                auto head_failure = try_start_buffered_response_write(
                    response_head, response, *planned_response, next_request_buffered, started_write, !plain_tcp);
                if (head_failure == nullptr && file_response) {
                    // The first file chunk is read (and the file validated)
                    // before its head leaves, so a file that cannot be served
                    // still gets an error response.
                    file_body.emplace(response, file_output);
                    if (!(co_await file_body->start(*planned_response))) {
                        const auto& file_failure = *file_body->failure();
                        options_.connection_failure_.invoke(remote_address, file_failure.fault());
                        head_failure = file_failure.response_error();
                        file_body.reset();
                        response_head.reset();
                    }
                }
                if (head_failure == nullptr) {
                    break;
                }
                // An error response that cannot be serialized either is a real
                // fault of the error handler.
                if (head_failure_recovered) {
                    std::rethrow_exception(head_failure);
                }
                // Nothing of this response reached the wire. Like a stream whose
                // head fails, it becomes the generic error response that closes
                // the connection; the complete responses held before it go first.
                if (co_await response_output.flush()) {
                    co_return http_connection_close::abort;
                }
                tail.disable_response_coding();
                request_completion.emplace(co_await complete_failed_http_route(scanner_entry, head_failure,
                    parsed_value, routes_value, services, response));
                connection_plan = request_completion->connection_plan();
                head_failure_recovered = true;
            }
            const auto& response_plan = *planned_response;
            const auto& write_start = *started_write;
            if (write_start.held_) {
                // Logged once the write that carries its head commits it.
                tail.defer_access(options_.access_log_, work_set.deferred_access_, write_start.head_bytes_,
                    response_plan.body_plan().response_status());
                scanner_entry.set_phase(connection_scanner::phase::idle);
            } else {
                std::optional<http1_buffered_response_write_result> write_result;
                if (file_response) {
                    scanner_entry.set_phase(connection_scanner::phase::writing);
                    write_result.emplace(co_await file_body->finish(
                        stream, response_head, response_plan, write_start, scanner_entry));
                    response_head.reset();
                    if (const auto* file_failure = file_body->failure()) {
                        options_.connection_failure_.invoke(remote_address, file_failure->fault());
                    }
                } else {
                    if (!response_output.start(write_start.body_,
                            connection_plan.disposition() == http1_close_policy::close_after_response &&
                                !stop_token_.stop_requested())) {
                        const auto completion = co_await response_output.async_write_remaining();
                        response_output.finish(completion.error_code(), completion.result());
                    }
                    const auto written = response_output.complete();
                    write_result.emplace(classify_http1_buffered_response_write(
                        response_plan, write_start.head_bytes_, written.error_, written.bytes_));
                }
                scanner_entry.set_phase(connection_scanner::phase::idle);
                if (const auto committed_status = write_result->committed_status_) {
                    tail.record_access(options_.access_log_, *committed_status);
                }
                if (!write_result->completed_) {
                    co_return http_connection_close::abort;
                }
            }
        } else if (const auto* committed = request_completion->committed_stream()) {
            scanner_entry.set_phase(connection_scanner::phase::idle);
            tail.record_access(options_.access_log_, committed->status());
            if (committed->truncated()) {
                co_return http_connection_close::abort;
            }
        } else {
            throw std::logic_error("HTTP/1 request completion has no wire alternative");
        }

        // A stopping worker ends the connection after the response.
        if (connection_plan.disposition() == http1_close_policy::close_after_response ||
            !http_server_worker_running(state_)) {
            co_return http_connection_close::linger;
        }
        read_buffer.complete_request(request_completion->buffer_completion());
        read_buffer.trim();
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        session.served_keepalive_request_ = true;
        if (read_buffer.empty()) {
            co_return http1_serve_handoff::await_request;
        }
        request_pipelined = true;
    }
}

template task<http_connection_close> worker_connections::run_stream<inbound_socket_stream>(
    http_server_session_config&, inbound_socket_stream&, tcp_socket_type&, context_services);
template task<http_connection_close> worker_connections::run_stream<server_tls_stream>(
    http_server_session_config&, server_tls_stream&, tcp_socket_type&, context_services);
template task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests<inbound_socket_stream>(
    http1_connection_session<inbound_socket_stream>&, connection_work_set&);
template task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests<server_tls_stream>(
    http1_connection_session<server_tls_stream>&, connection_work_set&);

#if defined(RUVIA_ENABLE_HTTP2)
template <typename stream_type>
task<http_connection_close> worker_connections::run_http2(stream_type& stream, tcp_socket_type& socket,
    context_services services) {
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    co_return co_await run_http2_sans_io_session(
        stream, routes_, http2_sans_io_session_context(std::move(services), options_, scanner_entry));
}

template task<http_connection_close> worker_connections::run_http2<inbound_socket_stream>(
    inbound_socket_stream&, tcp_socket_type&, context_services);
template task<http_connection_close> worker_connections::run_http2<server_tls_stream>(
    server_tls_stream&, tcp_socket_type&, context_services);
#endif

}  // namespace ruvia::detail
