#include "web_worker_runtime.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/post.hpp>

#include "blocking_pool.h"
#include "context_access.h"
#include "http_client.h"
#include "move_only_function.h"
#include "route_table.h"
#include "server_listener.h"
#include "server_options.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_capabilities.h"
#include "worker_ref.h"
#include "worker_runtime.h"
#include "worker_state.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis.h"
#endif

namespace ruvia {

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

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration, const route_table& routes_value, worker_capability_definitions capabilities, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    ) :
    web_worker_runtime(configuration.listeners(), routes_value, capabilities, configuration.options(), pool, worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_placement
#endif
    ) {
}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners, const route_table& routes_value, worker_capability_definitions capabilities, http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    ) :
    runtime_({.queue_capacity_ = validated_options.server_.worker_queue_capacity_, .io_policy_ = ruvia::worker_io_policy::single_owner, .blocking_pool_ = pool}),
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
            .route_rate_limits_ = routes_.has_route_rate_limit() ? route_rate_limit_presence::present : route_rate_limit_presence::absent,
            .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
            .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
            .http_client_worker_ = options_.server_.http_client_worker_,
            .env_ = options_.env_,
            .trusted_proxies_ = options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
            .precompressed_static_files_ = options_.compression_.has_value(),
            .file_io_timeout_ = options_.server_.write_timeout_,
            .connection_failure_ = &options_.connection_failure_,
        }),
    connections_(io_context_, runtime_.worker(), routes_, capabilities_, options_, stop_token_, worker_state_, background_tasks_, listeners,
        worker_failure_sink{
            .target_ = &runtime_,
            .invoke_ =
                [](void* target, const std::exception_ptr& failure) noexcept {
                    (void)static_cast<ruvia::worker_runtime*>(target)->fail(failure);
                }
        },
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
        .startup_ =
            [this] {
        worker_state_ = http_server_worker_state::running;
        // Startup runs before the context does; the task starts in its first turn.
        asio::post(io_context_, [this] {
            ruvia::async_start(run_worker(), asio::bind_executor(io_context_, [this](std::exception_ptr failure) noexcept {
                if (failure) {
                    (void)runtime_.fail(failure);
                }
            }));
        });
    },
        .serve_ =
            [this] {
        serve_on_context();
    },
        .stop_admission_ =
            [this] {
        stop_admission_on_context();
    },
        .finalize_ =
            [this] {
        finalize_on_context();
    },
        .failure_ =
            [this](std::exception_ptr failure) noexcept {
        worker_failures_.fetch_add(1, std::memory_order_relaxed);
        options_.failure_.notify(failure);
    },
        .shutdown_ = [this]() noexcept {
        connections_.retire_tls();
        worker_state_ = http_server_worker_state::stopped;
    },
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
