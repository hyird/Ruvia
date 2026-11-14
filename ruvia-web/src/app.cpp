#include "app.h"

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

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context_access.h"
#include "controller.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_content_negotiation.h"
#include "http_header.h"
#include "http_response.h"
#include "http_server.h"
#include "inline_storage.h"
#include "middleware.h"
#include "rate_limit.h"
#include "response_file.h"
#include "runtime_control.h"
#include "server_session.h"
#include "startup_config.h"
#include "stop_token.h"
#include "worker_ref.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db_pool.h"
#include "db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis.h"
#include "redis_pool.h"
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
#include <fstream>
#include <initializer_list>
#include <istream>
#include <span>
#include <system_error>
#include <type_traits>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/crypto.h>
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
    state_(std::make_shared<detail::app_state>()) {}

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

web_worker_context::web_worker_context(const worker_ref& worker_value, const detail::worker_client_registry_view& client_registries, const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value) noexcept :
    capabilities_(worker_value, stop_token_value, worker_states),
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

web_worker_ref::web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept :
    dispatch_(std::move(dispatch)) {}

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

web_worker_post_result web_worker_ref::post_task(move_only_function<task<void>(web_worker_context&)> task_value) const {
    if (dispatch_) {
        return dispatch_->post(std::move(task_value));
    }
    // A handle without a worker refuses with worker_stopping and returns the task.
    return worker_ref().post_payload(std::move(task_value), [](move_only_function<task<void>(web_worker_context&)>&&) -> move_only_function<void()> {
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

web_worker_dispatch::web_worker_dispatch(ruvia::worker_runtime& runtime, worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value) :
    worker_(runtime.worker()),
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
            route_plan_ = detail::compile_route_plan(controller_registrars, controllers_, configuration_);
            routes_.emplace(*route_plan_, controllers_);

            capabilities_.emplace(event_loop_.io_context(), worker_, detail::worker_capability_definitions{.worker_states_ = configuration_.worker_states()},
                detail::worker_capability_options{
                    .default_rate_limit_ = options_.default_rate_limit_per_worker_,
                    .route_rate_limits_ = routes_->has_route_rate_limit() ? detail::route_rate_limit_presence::present : detail::route_rate_limit_presence::absent,
                    .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
                    .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
                    .http_client_worker_ = options_.server_.http_client_worker_,
                    .env_ = &env_,
                    .trusted_proxies_ = options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
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
        detail::inline_http_header_views<16> active_headers(request.headers_.size() + (!request.cookies_.empty() ? 1U : 0U));
        for (const auto& [name, value] : request.headers_) {
            active_headers.push_back(http_header_view(name, value));
        }
        if (!request.cookies_.empty()) {
            active_headers.push_back(http_header_view("Cookie", request.cookies_));
        }
        std::optional<http_protocol_error> parse_error;
        auto parsed_value = [&]() {
            try {
                return make_parsed_http_request(request.method_, request.target_, active_headers.view(), std::as_bytes(std::span(request.body_.data(), request.body_.size())));
            } catch (const http_protocol_error& error) {
                parse_error = error;
                return make_parsed_http_request("GET", "/", {}, {});
            }
        }();

        const auto& routes_value = *routes_;
        const auto resolution = routes_value.resolve(parsed_value);
        // A test request has no transport connection, hence connection id zero.
        const auto services = capabilities_->make_context_services(stop_token_).with_plain_transport(0, request.remote_address_, request.remote_port_);

        // A malformed request is rejected before admission, with no coding
        // promise, exactly as a server answers a malformed head.
        std::optional<detail::http_request_admission> admission;
        http_response response;
        if (parse_error.has_value()) {
            const auto rejection = detail::http_request_rejection::error(detail::copy_http_protocol_error_info(*parse_error));
            response = co_await detail::respond_http_request_rejection(routes_value, parsed_value, rejection, services);
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
            auto admitted = co_await detail::admit_and_dispatch_buffered_request(routes_value, input, policy, services, response);
            admission.emplace(std::move(admitted));
        }
        const auto& request_services = admission ? admission->services() : services;
        const auto remote_ip = ip_address::parse(request.remote_address_).value_or(ip_address{});
        const auto tail = admission ? admission->tail() : detail::http_response_tail(parsed_value, detail::http_response_coding_policy::disabled(), remote_ip, remote_ip, request_start);
        const auto prepared = co_await tail.prepare_or_finish(response, options_, worker_, routes_value, request_services);

        dispatch_response result_value;
        result_value.copy_head_from(response);
        // On the wire, HEAD responses and content-forbidden statuses carry no
        // body, so the facade must not surface one
        // either. Writer-synthesized fields (Content-Length, Date, Connection)
        // are framing concerns and stay absent here.
        if (!prepared.write_plan_.body_plan().body_suppressed()) {
            if (response.file_body() || response.has_multipart_file_body()) {
                detail::response_file_source source(response, {.worker_ = worker_, .stop_ = stop_token_, .timeout_ = options_.server_.write_timeout_});
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

test_app::test_app() :
    impl_(std::make_unique<impl_type>()) {}

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
class shutdown_signal_listener final {
public:
    explicit shutdown_signal_listener(application& owner) :
        runtime_({.queue_capacity_ = 1, .io_policy_ = worker_io_policy::single_owner}),
        signals_(runtime_.io_context()) {
        runtime_.configure({
            .stop_admission_ =
                [this] {
            std::error_code ignored;
            signals_.clear(ignored);
            signals_.cancel(ignored);
            runtime_.finalize();
        },
            .failure_ = [&owner](std::exception_ptr) noexcept {
            owner.stop();
        },
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

    ~shutdown_signal_listener() {
        stop();
    }

    shutdown_signal_listener(const shutdown_signal_listener&) = delete;
    shutdown_signal_listener& operator=(const shutdown_signal_listener&) = delete;

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
    app_run_coordinator(application& owner_value, detail::app_state& state_value) :
        owner_(owner_value),
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
            return !listener.connection_.has_value() && !listener.request_stream_ && !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(listener.transport_);
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
            .invoke_ = [](void* target, const std::exception_ptr&) noexcept {
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
            const auto precompression = prepared_options.compression_.has_value() ? document_root.precompression_ : detail::static_root_precompression_options{};
            runtime->document_root_ = std::make_unique<detail::document_root_publisher>(http_make_path_from_native_path(document_root.root_), document_root.static_options_, document_root.refresh_interval_, precompression, document_root.resident_, state_.options_.server_.worker_count_);
            prepared_options.document_root_ = detail::http_server_options::document_root_state::refreshing(*runtime->document_root_);
        }
        if (state_.blocking_pool_.has_value()) {
            runtime->blocking_pool_ = std::make_unique<blocking_pool>(*state_.blocking_pool_);
        }

        const auto validated_configuration = detail::validate_http_server_configuration(state_.listeners_, std::move(prepared_options));
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
            runtime->route_plan_ = detail::compile_route_plan(controller_registrars, first_controllers, state_.configuration_);
        } else {
            runtime->route_plan_ = std::make_unique<detail::compiled_route_plan>();
        }
#if defined(RUVIA_ENABLE_HTTP3)
        if (std::ranges::any_of(validated_configuration.listeners(), [](const auto& listener) {
            return listener.http3_.has_value();
        })) {
            runtime->http3_directory_ = std::make_unique<detail::http3_worker_directory>(state_.options_.server_.worker_count_);
        }
#endif
#if defined(_WIN32)
        runtime->acceptor_ = std::make_unique<detail::windows_acceptor>(validated_configuration.listeners(), state_.options_.server_.worker_count_,
#if defined(RUVIA_ENABLE_HTTP3)
            runtime->http3_directory_.get(),
#endif
            validated_configuration.options().failure_);
#endif
        runtime->workers_.reserve(state_.options_.server_.worker_count_);
        for (std::size_t i = 0; i < state_.options_.server_.worker_count_; ++i) {
            auto controllers = i == 0 ? std::move(first_controllers) : detail::controller_store(controller_registrars);
            auto routes_value = std::make_unique<detail::route_table>(*runtime->route_plan_, controllers);
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
            auto worker_value = std::make_unique<detail::web_worker_runtime>(validated_configuration, *routes_value, capabilities, runtime->blocking_pool_.get(), i,
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
            auto worker_value = std::make_unique<detail::web_worker_runtime>(validated_configuration, *routes_value, capabilities, runtime->blocking_pool_.get(), i);
#endif
#if defined(_WIN32)
            runtime->acceptor_->bind_worker(i, worker_value->acceptor_target());
#endif
            runtime->workers_.emplace_back(std::move(controllers), std::move(routes_value), std::move(worker_value));
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
        state_.lifecycle_changed_.wait(lock, [this] {
            return state_.lifecycle_.stop_requested();
        });
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
            state_.lifecycle_changed_.wait(lock, [this] {
                return state_.runtime_controls_ == 0;
            });
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
    std::optional<shutdown_signal_listener> signals_;
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

worker_capabilities::worker_capabilities(asio::io_context& io_context, const worker_ref& worker_value, worker_capability_definitions definitions, worker_capability_options options) :
    worker_(require_worker_capabilities_worker(worker_value)),
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
    http_clients_(io_context, worker_, definitions.http_clients_, http_client_services_),
    worker_states_(definitions.worker_states_),
    rate_limiter_(options.default_rate_limit_, options.route_rate_limits_, options.rate_limit_capacity_),
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
    co_return;
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
[[nodiscard]] auto find_borrowed_alias(const std::vector<entry_type>& entries, std::string_view alias) noexcept -> decltype(entries.front().client_) {
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
        throw std::logic_error("worker state type is not registered: call application::use_worker_state<state_type>() or router::bind_worker_state()");
    }
    return instance;
}

#ifdef RUVIA_ENABLE_DATABASE
db_session worker_client_registry_view::db(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return db(default_capability_alias, operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(operation_scope, io_metrics).with_options(context_operation_options(stop_token_value));
}

db_session worker_client_registry_view::db(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
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
    return databases_->get(alias, operation_scope, io_metrics).with_options(context_operation_options(stop_token_value));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session worker_client_registry_view::redis(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return redis(default_capability_alias, operation_scope, io_metrics, stop_token_value);
    }
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(operation_scope, io_metrics).with_options(context_operation_options(stop_token_value));
}

redis_session worker_client_registry_view::redis(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
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
    return redis_->get(alias, operation_scope, io_metrics).with_options(context_operation_options(stop_token_value));
}
#endif

http_client_ref worker_client_registry_view::client(::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return client(default_capability_alias, operation_scope, stop_token_value);
    }
    if (!attached()) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return http_clients_->get(operation_scope, stop_token_value);
}

http_client_ref worker_client_registry_view::client(std::string_view alias, ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (borrowed_ != nullptr) {
        return borrowed_http_clients(alias).client().bind_caller_scope(operation_scope, stop_token_value);
    }
    if (!attached()) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return http_clients_->get(alias, operation_scope, stop_token_value);
}
const http_client& worker_client_registry_view::borrowed_http_clients(std::string_view alias) const {
    const auto* clients = borrowed_->http_clients(alias);
    if (clients == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "http client is not configured");
    }
    return *clients;
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

template <typename variables_type>
[[nodiscard]] auto find_variable_slot(variables_type& variables, std::string_view name) noexcept {
    return std::ranges::lower_bound(variables, name, std::ranges::less{}, [](const detail::env_variable& variable) noexcept {
        return std::string_view(variable.name_);
    });
}

}  // namespace

env::env() :
    state_(std::make_unique<detail::env_state>()) {}

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
    if (http_ascii_equals_ignore_case(value, "true") || http_ascii_equals_ignore_case(value, "yes") || http_ascii_equals_ignore_case(value, "on")) {
        return true;
    }
    if (http_ascii_equals_ignore_case(value, "false") || http_ascii_equals_ignore_case(value, "no") || http_ascii_equals_ignore_case(value, "off")) {
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
        const auto length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
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
            return std::filesystem::path(std::string_view(buffer.data(), static_cast<std::size_t>(length))).parent_path();
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
    if (options.existing_variables_ != dotenv_existing_variable_policy::preserve && options.existing_variables_ != dotenv_existing_variable_policy::override) {
        throw std::invalid_argument("dotenv existing variable policy is invalid");
    }
    if (options.missing_file_ != dotenv_missing_file_policy::ignore && options.missing_file_ != dotenv_missing_file_policy::require) {
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

[[nodiscard]] std::string location_message(const std::filesystem::path& path, std::size_t line_number, std::string_view message) {
    auto result_value = dotenv_path_message("invalid dotenv entry in ", path);
    result_value += ':';
    result_value += std::to_string(line_number);
    result_value += ": ";
    result_value += message;
    return result_value;
}

[[nodiscard]] std::size_t dotenv_utf8_sequence_length(std::string_view value, std::size_t index, const std::filesystem::path& path, std::size_t line_number) {
    const auto sequence = ::ruvia::scan_utf8_sequence(value, index);
    if (!sequence.well_formed_) {
        throw std::invalid_argument(location_message(path, line_number, "value is not valid UTF-8").c_str());
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

[[nodiscard]] std::string parse_double_quoted_value(std::string_view value, const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
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
                throw std::invalid_argument(location_message(path, line_number, "unfinished escape sequence").c_str());
            }
            has_escape = true;
            result.append(value.substr(literal_start, index - literal_start));
            const char escaped = value[++index];
            if (const auto decoded = decode_double_quoted_escape(escaped); decoded.has_value()) {
                result.push_back(*decoded);
            } else {
                result.push_back('\\');
                const auto length = static_cast<unsigned char>(escaped) >= 0x80 ? dotenv_utf8_sequence_length(value, index, path, line_number) : 1;
                result.append(value.substr(index, length));
                index += length - 1;
            }
            literal_start = index + 1;
        }
    }
    throw std::invalid_argument(location_message(path, line_number, "unterminated double-quoted value").c_str());
}

[[nodiscard]] std::string parse_single_quoted_value(std::string_view value, const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    for (std::size_t index = 1; index < value.size(); ++index) {
        if (value[index] == '\'') {
            consumed = index + 1;
            return std::string(value.substr(1, index - 1));
        }
        if (static_cast<unsigned char>(value[index]) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
        }
    }
    throw std::invalid_argument(location_message(path, line_number, "unterminated single-quoted value").c_str());
}

void validate_quoted_remainder(std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (!value.empty() && value.front() != '#') {
        throw std::invalid_argument(location_message(path, line_number, "unexpected characters after quoted value").c_str());
    }
}

[[nodiscard]] std::string parse_unquoted_value(std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
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

[[nodiscard]] std::string parse_value(std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
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
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == bom[0] && static_cast<unsigned char>(line[1]) == bom[1] && static_cast<unsigned char>(line[2]) == bom[2]) {
        line.remove_prefix(3);
    }
}

[[nodiscard]] env_variable parse_entry(std::string_view line, const std::filesystem::path& path, std::size_t line_number) {
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
        throw std::invalid_argument(location_message(path, line_number, "invalid variable name").c_str());
    }

    auto parsed_value = parse_value(line.substr(separator + 1), path, line_number);
    env_variable entry;
    entry.name_.assign(key.data(), key.size());
    entry.value_ = std::move(parsed_value);
    return entry;
}

}  // namespace

std::vector<env_variable> read_dotenv_entries(std::istream& input, const std::filesystem::path& path) {
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
        const auto slot = std::ranges::lower_bound(entries_by_name, std::string_view(entry.name_), std::ranges::less{}, entry_name);
        if (slot != entries_by_name.end() && entry_name(*slot) == std::string_view(entry.name_)) {
            throw std::invalid_argument(location_message(path, line_number, "duplicate variable name").c_str());
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
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
        state_value.env_.load_dotenv(options);
    });
}

application& application::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot load dotenv while app is running", [&](detail::app_state& state_value) {
        state_value.env_.load_dotenv(path, options);
    });
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
    return !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() || !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() || !identity.private_key_password_.empty() || config.client_certificates_.ca_file_.has_value() || config.client_certificates_.requirement_ != tls_client_certificate_requirement::optional || !config.sni_.empty() || !config.local_addresses_.empty() || config.certificate_store_ != nullptr || static_cast<bool>(config.select_certificate_) || config.default_certificate_ != 0;
}

// listen() and replace_tls_identities() own their tls_config, the only copy
// of each private-key password and in-memory PEM text the application ever
// holds: credentials are loaded from it and every such secret is cleansed
// when the call returns or throws.
class tls_secret_cleanser final {
public:
    explicit tls_secret_cleanser(tls_config& config) noexcept :
        config_(config) {}
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

void append_listen_config(std::vector<detail::http_server_listener_definition>& replacement, const listen_config& config) {
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
    if (config.auto_https_redirect_ && (!config.http_.has_value() || !config.https_.has_value())) {
        throw std::invalid_argument("automatic HTTPS redirect requires both HTTP and HTTPS ports");
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
        http3.emplace(detail::http_server_listener_definition::http3_service{.port_ = config.http3_->value(), .alt_svc_max_age_ = config.http3_alt_svc_max_age_, .early_data_ = config.http3_early_data_, .retry_ = config.http3_retry_, .secret_ = {}});
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
            if (existing.http3_.has_value() && existing.http3_->port_ == port && existing.endpoint_.address() == address) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    };

    if (config.http_.has_value() && !mixed) {
        const auto endpoint = asio::ip::tcp::endpoint(address, config.http_->value());
        ensure_unique_endpoint(endpoint);
        if (config.auto_https_redirect_) {
            replacement.emplace_back(endpoint, detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
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
                tls.plaintext_redirect_.emplace(detail::http_server_listener_definition::redirect_http_to_https{config.https_->value()});
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
    explicit tls_secret_span_cleanser(std::span<const listen_config> configs) noexcept :
        configs_(configs) {}

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
                detail::adopt_inherited_tcp_socket(socket, {address, port->value()}, "inherited TCP socket does not serve its endpoint", config.ipv6_only_);
            }
        };
        tcp(sockets.http_, config.http_);
        tcp(sockets.https_, config.https_);
        if (!sockets.http3_.empty() && !config.http3_) {
            throw std::invalid_argument("inherited UDP sockets require HTTP/3");
        }
        for (const auto socket : sockets.http3_) {
            detail::adopt_inherited_udp_socket(socket, {address, config.http3_->value()}, "inherited UDP socket does not serve its endpoint", config.ipv6_only_);
        }
    }
}

[[nodiscard]] bool holds_socket(std::span<const native_socket> sockets, native_socket socket) noexcept {
    return std::ranges::find(sockets, socket) != sockets.end();
}

// A failed listen() or add_listener() closes every handle it was given that
// no definition it built owns -- those close with their definition -- each
// handle once. No run has claimed any of them yet.
void close_unowned_inherited_sockets(std::span<const listen_config> configs, std::span<const detail::http_server_listener_definition> built) noexcept {
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
    if ((!sockets.http_.empty() && !config.http_.has_value()) || (!sockets.https_.empty() && !config.https_.has_value()) || (!sockets.http3_.empty() && !config.http3_.has_value())) {
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
        const auto udp = definition.http3_.has_value() ? std::span<const native_socket>(sockets.http3_) : std::span<const native_socket>();
        if (tcp.empty() && udp.empty()) {
            continue;
        }
        for (const auto socket : tcp) {
            detail::adopt_inherited_tcp_socket(socket, definition.endpoint_, https ? "inherited HTTPS sockets must listen on the configured address and port" : "inherited HTTP sockets must listen on the configured address and port", config.ipv6_only_);
        }
        if (!udp.empty()) {
            const asio::ip::udp::endpoint endpoint(definition.endpoint_.address(), definition.http3_->port_);
            for (const auto socket : udp) {
                detail::adopt_inherited_udp_socket(socket, endpoint, "inherited HTTP/3 sockets must be UDP sockets bound to the configured address and port", config.ipv6_only_);
            }
        }
        definition.inherited_sockets_ = std::make_shared<detail::inherited_listener_sockets>(std::vector<native_socket>(tcp.begin(), tcp.end()), std::vector<native_socket>(udp.begin(), udp.end()));
    }
}

// The listener definitions `configs` describe, with their inherited sockets.
void append_listen_configs(std::vector<detail::http_server_listener_definition>& replacement, std::span<const listen_config> configs) {
    ensure_distinct_inherited_sockets(configs);
    std::size_t endpoint_count = 0;
    for (const auto& config : configs) {
        endpoint_count += config.http_.has_value() && config.http_ == config.https_ ? 1 : static_cast<std::size_t>(config.http_.has_value()) + static_cast<std::size_t>(config.https_.has_value());
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
[[nodiscard]] detail::app_runtime_graph& running_runtime(detail::app_state& state_value, const char* not_running_message) {
    if (!state_value.lifecycle_.running() || state_value.runtime_ == nullptr) {
        throw std::logic_error(not_running_message);
    }
    return *state_value.runtime_;
}

class active_app_control final {
public:
    active_app_control(std::shared_ptr<detail::app_state> state, const char* not_running) :
        state_(std::move(state)) {
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
    explicit owned_tls_config(tls_config config) :
        config_(std::move(config)) {}
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
    auto result = co_await run_blocking(caller, completion.make_command([function = std::move(function)](const auto& receipt) mutable -> result_type {
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
[[nodiscard]] std::size_t runtime_listener_slot(const detail::app_runtime_graph& runtime, const asio::ip::tcp::endpoint& target) {
    for (std::size_t slot = 0; slot < runtime.listener_slots_.size(); ++slot) {
        if (runtime.listener_slots_[slot] == target) {
            return slot;
        }
    }
    throw std::logic_error("the running app serves no slot for a stored listener");
}

// The index of the HTTPS listener `target` names in the running app, whose
// identities `config` may replace. The caller holds the application lock.
[[nodiscard]] std::size_t find_replaceable_tls_listener(detail::app_state& state_value, const asio::ip::tcp::endpoint& target, const tls_config& config) {
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
void make_worker_controls(control_list& controls, detail::app_runtime_graph& runtime, detail::runtime_control_completion& publication, const run_type& run) {
    controls.reserve(runtime.workers_.size());
    for (std::size_t index = 0; index < runtime.workers_.size(); ++index) {
        controls.emplace_back(publication.make_command([run, worker = runtime.workers_[index].runtime_.get(), index](const auto& receipt) mutable {
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

[[nodiscard]] runtime_route_candidate prepare_runtime_routes(const detail::app_state& state_value, std::size_t worker_count) {
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
            rate_limiters.push_back(std::make_unique<detail::rate_limiter>(std::nullopt, detail::route_rate_limit_presence::present, state_value.options_.rate_limit_capacity_per_worker_));
        }
    }
    return result;
}

task<void> enable_runtime_routes(worker_ref caller, detail::app_runtime_graph& runtime, runtime_route_candidate& candidate) {
    detail::runtime_control_completion publication(caller);
    control_list controls;
    make_worker_controls(controls, runtime, publication, [&runtime, &candidate](detail::web_worker_runtime& worker, std::size_t index) noexcept {
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
[[nodiscard]] move_only_function<void()> make_acceptor_control(detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
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
[[nodiscard]] bool post_acceptor_control(detail::windows_acceptor& acceptor, detail::runtime_control_completion& publication, run_type run) {
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
void detail::ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing, std::span<const http_server_listener_definition> added) {
    for (const auto& candidate : added) {
        for (const auto& listener : existing) {
            if (listener.endpoint_ == candidate.endpoint_) {
                throw std::invalid_argument("duplicate listen endpoint");
            }
            if (candidate.http3_.has_value() && listener.http3_.has_value() && listener.http3_->port_ == candidate.http3_->port_ && listener.endpoint_.address() == candidate.endpoint_.address()) {
                throw std::invalid_argument("duplicate HTTP/3 listen endpoint");
            }
        }
    }
}

std::vector<detail::http_server_listener_definition> detail::load_server_listeners(std::span<const listen_config> configs) {
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
    return std::ranges::any_of(configs_, [](const auto& config) {
        return config.https_.has_value();
    });
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
    detail::mutate_stopped_app(*this, *state_, "cannot change listeners while app is running", [&replacement](detail::app_state& state_value) {
        state_value.listeners_.swap(replacement);
    });
    return *this;
}

application& application::listen(std::initializer_list<listen_config> configs) & {
    return listen(std::span<const listen_config>(configs.begin(), configs.size()));
}

namespace {
task<void> replace_app_tls_identities(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, owned_tls_config input) {
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
        make_worker_controls(controls, runtime, completion, [slot, identities](detail::web_worker_runtime& worker, std::size_t) {
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

task<void> application::replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) & {
    owned_tls_config input(std::move(config));
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return replace_app_tls_identities(state_, std::move(caller), target, std::move(input));
}

namespace {
task<bool> close_app_connection(std::shared_ptr<detail::app_state> state, worker_ref caller, std::uint64_t connection_id) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "connections can be closed only while the app is running");
    auto& runtime = active.runtime();
    std::atomic<bool> closed{false};
    detail::runtime_control_completion completion(caller);
    control_list controls;
    make_worker_controls(controls, runtime, completion, [connection_id, &closed](detail::web_worker_runtime& worker, std::size_t) noexcept {
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
task<void> wait_app_connections(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, operation_options options) {
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

[[nodiscard]] std::exception_ptr control_round_failure(const detail::runtime_control_completion& completion, bool posted, std::size_t targets, const char* message) noexcept {
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

task<void> add_app_listener(std::shared_ptr<detail::app_state> state, worker_ref caller, detail::owned_listen_configs input, std::shared_ptr<detail::prepared_listener_state> transaction) {
    const auto serialized = co_await state->runtime_control_gate_.acquire(caller);
    active_app_control active(state, "listeners can be added only while the app is running");
    auto& runtime = active.runtime();
    const auto worker_count = runtime.workers_.size();
    const auto key_origin = runtime.tls_key_origin_;
    const bool needs_routes = !runtime.routes_enabled_;
    const bool inexpensive = std::ranges::none_of(input.configs_, [needs_routes](const auto& config) {
        return config.https_.has_value() || (needs_routes && config.dispatch_ == listener_dispatch::routes);
    });
    auto prepare = [state, worker_count, key_origin, needs_routes, input = std::move(input)]() mutable {
        listener_addition_candidate result;
        result.added_ = input.load();
        result.prepared_.reserve(result.added_.size());
        for (const auto& definition : result.added_) {
            result.prepared_.push_back(detail::validate_run_listener(definition, state->options_, key_origin));
        }
        result.sessions_.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            auto& sessions = result.sessions_.emplace_back();
            sessions.reserve(result.prepared_.size());
            for (const auto& definition : result.prepared_) {
                if (std::holds_alternative<detail::http_server_listener_definition::tls>(definition.transport_)) {
                    sessions.push_back(detail::prepare_http_server_session_config(definition, static_cast<bool>(state->options_.tls_client_hello_)));
                } else {
                    sessions.emplace_back();
                }
            }
        }
        if (needs_routes && std::ranges::any_of(result.added_, [](const auto& listener) {
            return !listener.connection_.has_value() && !listener.request_stream_ && !std::holds_alternative<detail::http_server_listener_definition::redirect_http_to_https>(listener.transport_);
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
        if (runtime.http3_directory_ == nullptr && std::ranges::any_of(prepared, [](const auto& definition) {
            return definition.http3_.has_value();
        })) {
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
            http3_slots.push_back(definition.http3_.has_value() ? std::optional<std::size_t>(runtime.http3_slot_count_++) : std::nullopt);
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
    make_worker_controls(prepare_controls, runtime, prepared_completion, [&](detail::web_worker_runtime& worker, std::size_t worker_index) {
        for (std::size_t index = 0; index < prepared.size(); ++index) {
#if defined(RUVIA_ENABLE_HTTP3)
            worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index], std::move(candidate.sessions_[worker_index][index]), directory);
#else
            worker.prepare_listener(first_slot + index, http3_slots[index], prepared[index], std::move(candidate.sessions_[worker_index][index]));
#endif
        }
    });
    make_worker_controls(discard_controls, runtime, discarded_completion, [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::web_worker_runtime& worker, std::size_t) {
        discard(worker, receipt);
    });
    make_worker_controls(open_controls, runtime, opened_completion, [open](detail::web_worker_runtime& worker, std::size_t) {
        open(worker);
    });
#if defined(_WIN32)
    auto prepare_acceptor = make_acceptor_control(*runtime.acceptor_, prepared_completion, [&](detail::windows_acceptor& acceptor) {
        for (std::size_t index = 0; index < prepared.size(); ++index) {
#if defined(RUVIA_ENABLE_HTTP3)
            acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index], directory);
#else
            acceptor.prepare_listener(first_slot + index, http3_slots[index], prepared[index]);
#endif
        }
    });
    auto discard_acceptor = make_acceptor_control(*runtime.acceptor_, discarded_completion, [discard, receipt = std::make_shared<detail::runtime_control_completion::receipt>(discarded_completion.make_receipt())](detail::windows_acceptor& acceptor) {
        discard(acceptor, receipt);
    });
    detail::runtime_control_completion accepting_completion(caller);
    auto open_acceptor = make_acceptor_control(*runtime.acceptor_, accepting_completion, [open](detail::windows_acceptor& acceptor) {
        open(acceptor);
    });
#endif
    bool posted = post_worker_controls(runtime, prepare_controls);
#if defined(_WIN32)
    if (posted) {
        posted = runtime.acceptor_->post_control(std::move(prepare_acceptor));
    }
    prepare_acceptor = nullptr;
#endif
    co_await prepared_completion.wait();
    auto failure = control_round_failure(prepared_completion, posted, control_targets(runtime), "the app began stopping before every worker prepared the listeners");
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
        failure = control_round_failure(opened_completion, posted, runtime.workers_.size(), "the app began stopping before every worker accepted the listeners");
#if defined(_WIN32)
        if (!failure) {
            {
                std::lock_guard lock(state->mutex_);
                posted = state->lifecycle_.running() && runtime.acceptor_->post_control(std::move(open_acceptor));
            }
            open_acceptor = nullptr;
            co_await accepting_completion.wait();
            failure = control_round_failure(accepting_completion, posted, 1, "the app began stopping before the acceptor accepted the listeners");
        }
#endif
    }
    if (!failure) {
        std::lock_guard lock(state->mutex_);
        if (!state->lifecycle_.running()) {
            try {
                failure = std::make_exception_ptr(std::logic_error("the app began stopping before every worker accepted the listeners"));
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
    return detail::prepared_listener_access::launch(transaction, add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<prepared_listener> application::prepare_listeners(worker_ref caller, std::span<const listen_config> configs) & {
    detail::owned_listen_configs input(configs);
    auto transaction = detail::prepared_listener_access::make(caller);
    return detail::prepared_listener_access::launch(transaction, add_app_listener(state_, std::move(caller), std::move(input), transaction));
}
task<void> application::add_listener(worker_ref caller, listen_config config) & {
    return detail::prepared_listener_access::add(prepare_listener(std::move(caller), std::move(config)));
}

namespace {
task<void> remove_app_listener(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target, listener_remove_options options) {
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
    auto accepting_lifetime = std::make_shared<detail::runtime_control_completion::receipt>(accepting_completion.make_receipt());
    const bool accepting_posted = post_acceptor_control(*runtime.acceptor_, accepting_completion, [slot, receipt = std::move(accepting_lifetime)](detail::windows_acceptor& acceptor) {
        acceptor.remove_listener(slot, receipt);
    });
    co_await accepting_completion.wait();
    if (auto failure = control_round_failure(accepting_completion, accepting_posted, 1, "the app began stopping before the acceptor removed the listener")) {
        std::rethrow_exception(failure);
    }
#endif
    detail::runtime_control_completion completion(caller);
    control_list controls;
    auto lifetime = std::make_shared<detail::runtime_control_completion::receipt>(completion.make_receipt());
    make_worker_controls(controls, runtime, completion, [slot, receipt = std::move(lifetime), options](detail::web_worker_runtime& worker, std::size_t) {
        worker.remove_listener(slot, receipt, options);
    });
    bool posted = false;
    {
        std::lock_guard lock(state->mutex_);
        posted = state->lifecycle_.running() && post_worker_controls(runtime, controls);
    }
    controls.clear();
    co_await completion.wait();
    if (auto failure = control_round_failure(completion, posted, runtime.workers_.size(), "the app began stopping before every worker removed the listener")) {
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
task<listener_sockets> duplicate_app_listener_sockets(std::shared_ptr<detail::app_state> state, worker_ref caller, asio::ip::tcp::endpoint target) {
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
        make_worker_controls(controls, runtime, completion, [slot, &duplicates](detail::web_worker_runtime& worker, std::size_t index) {
            duplicates[index] = worker.duplicate_listener(slot);
        });
    }
    const bool posted = post_worker_controls(runtime, controls);
    co_await completion.wait();
    listener_sockets result;
    auto& tcp = https ? result.https_ : result.http_;
    try {
        if (auto failure = control_round_failure(completion, posted, duplicates.size(), "the app began stopping before every worker duplicated its sockets")) {
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

task<listener_sockets> application::duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const& {
    const asio::ip::tcp::endpoint target(normalize_listen_address(endpoint.address_.view()), endpoint.port_.value());
    return duplicate_app_listener_sockets(state_, std::move(caller), target);
}
#endif

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

application& application::on_access(access_log_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register access-log hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        // Captured old target retires with this lambda, after the app lock.
        std::swap(state_value.access_log_callback_, callback_value);
        state_value.options_.access_log_.callback_ = detail::callback_access::ref(state_value.access_log_callback_);
    });
}

application& application::on_connection_failure(connection_failure_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register connection-failure hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.connection_failure_callback_, callback_value);
        state_value.options_.connection_failure_.callback_ = detail::callback_access::ref(state_value.connection_failure_callback_);
    });
}

application& application::on_tls_client_hello(tls_client_hello_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register TLS ClientHello hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.tls_client_hello_callback_, callback_value);
        state_value.options_.tls_client_hello_ = detail::callback_access::ref(state_value.tls_client_hello_callback_);
    });
}

application& application::on_tls_client_hello(std::nullptr_t) & {
    return on_tls_client_hello(tls_client_hello_callback{});
}

application& application::on_http2_input(http2_input_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register HTTP/2 input hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.http2_input_callback_, callback_value);
        state_value.options_.http2_input_ = detail::callback_access::ref(state_value.http2_input_callback_);
    });
}

application& application::on_http2_input(std::nullptr_t) & {
    return on_http2_input(http2_input_callback{});
}

#if defined(RUVIA_ENABLE_HTTP2)
application& application::on_http2_frame(http2_frame_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register HTTP/2 frame hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.http2_frame_callback_, callback_value);
        state_value.options_.http2_frame_ = detail::callback_access::ref(state_value.http2_frame_callback_);
    });
}

application& application::on_http2_frame(std::nullptr_t) & {
    return on_http2_frame(http2_frame_callback{});
}

application& application::on_http2_headers(http2_headers_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register HTTP/2 headers hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
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
    return detail::mutate_stopped_app(*this, *state_, "cannot register HTTP/3 frame hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.http3_frame_callback_, callback_value);
        state_value.options_.http3_frame_ = detail::callback_access::ref(state_value.http3_frame_callback_);
    });
}

application& application::on_http3_frame(std::nullptr_t) & {
    return on_http3_frame(http3_frame_callback{});
}

application& application::on_http3_field(http3_field_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register HTTP/3 field hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.http3_field_callback_, callback_value);
        state_value.options_.http3_field_ = detail::callback_access::ref(state_value.http3_field_callback_);
    });
}

application& application::on_http3_field(std::nullptr_t) & {
    return on_http3_field(http3_field_callback{});
}
#endif

application& application::on_connection(connection_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register connection handler while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.connection_callback_, callback_value);
        state_value.options_.connection_ = detail::callback_access::ref(state_value.connection_callback_);
    });
}

application& application::on_connection(std::nullptr_t) & {
    return on_connection(connection_callback{});
}

application& application::on_connection_admission(connection_admission_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register connection admission hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.connection_admission_callback_, callback_value);
        state_value.options_.connection_admission_ = detail::callback_access::ref(state_value.connection_admission_callback_);
    });
}

application& application::on_connection_admission(std::nullptr_t) & {
    return on_connection_admission(connection_admission_callback{});
}

application& application::on_connection_open(connection_open_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register connection open hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.connection_open_callback_, callback_value);
        state_value.options_.connection_open_ = detail::callback_access::ref(state_value.connection_open_callback_);
    });
}

application& application::on_connection_open(std::nullptr_t) & {
    return on_connection_open(connection_open_callback{});
}

application& application::on_connection_close(connection_close_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register connection close hook while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.connection_close_callback_, callback_value);
        state_value.options_.connection_close_ = detail::callback_access::ref(state_value.connection_close_callback_);
    });
}

application& application::on_connection_close(std::nullptr_t) & {
    return on_connection_close(connection_close_callback{});
}

application& application::on_request_stream(request_stream_callback callback_value) & {
    return detail::mutate_stopped_app(*this, *state_, "cannot register request stream handler while app is running", [callback_value = detail::own_callback(std::move(callback_value))](detail::app_state& state_value) mutable {
        std::swap(state_value.request_stream_callback_, callback_value);
        state_value.options_.request_stream_ = detail::callback_access::ref(state_value.request_stream_callback_);
    });
}

application& application::on_request_stream(std::nullptr_t) & {
    return on_request_stream(request_stream_callback{});
}

#ifdef RUVIA_ENABLE_DATABASE
application& application::database(db_registration_config config) & {
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

application& application::database(std::nullptr_t) & {
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
