#include "ruvia/web/web_worker.h"

#include <exception>
#include <memory>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"

#include "app/web_worker_dispatch.h"
#include "integration/worker_capabilities.h"
#include "integration/worker_client_registry_view.h"

namespace ruvia {

web_worker_context::web_worker_context(const worker_handle& worker_value, std::pmr::memory_resource* resource,
    const detail::worker_client_registry_view& client_registries,
    const detail::worker_state_registry* worker_states, blocking_pool* blocking_pool_value,
    const stop_token& stop_token_value) noexcept
    : capabilities_(worker_value, stop_token_value, worker_states, blocking_pool_value),
      resource_(resource),
      client_registries_(client_registries) {}

const worker_handle& web_worker_context::worker() const& noexcept {
    return capabilities_.worker();
}

std::pmr::memory_resource* web_worker_context::pool() const noexcept {
    return resource_;
}

stop_token web_worker_context::get_stop_token() const noexcept {
    return capabilities_.stop_token();
}

#ifdef RUVIA_ENABLE_DATABASE
db_handle web_worker_context::db() const {
    return client_registries_.db(operation_scope_, capabilities_.stop_token());
}

db_handle web_worker_context::db(std::string_view alias) const {
    return client_registries_.db(alias, operation_scope_, capabilities_.stop_token());
}
#endif

http_client_handle web_worker_context::get_http_client() const {
    return client_registries_.get_http_client(operation_scope_, capabilities_.stop_token());
}

http_client_handle web_worker_context::get_http_client(std::string_view alias) const {
    return client_registries_.get_http_client(alias, operation_scope_, capabilities_.stop_token());
}

#ifdef RUVIA_ENABLE_REDIS
redis_handle web_worker_context::redis() const {
    return client_registries_.redis(operation_scope_, capabilities_.stop_token());
}

redis_handle web_worker_context::redis(std::string_view alias) const {
    return client_registries_.redis(alias, operation_scope_, capabilities_.stop_token());
}
#endif

web_worker_handle::web_worker_handle(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept
    : dispatch_(std::move(dispatch)) {}

bool web_worker_handle::valid() const noexcept {
    return dispatch_ && dispatch_->valid();
}

bool web_worker_handle::accepting() const noexcept {
    return dispatch_ && dispatch_->accepting();
}

worker_id_type web_worker_handle::id() const noexcept {
    return dispatch_ ? dispatch_->id() : 0;
}

web_worker_stats web_worker_handle::stats() const noexcept {
    return dispatch_ ? dispatch_->stats() : web_worker_stats{};
}

web_worker_post_result_type web_worker_handle::post_task(
    move_only_function<task<void>(web_worker_context&)> task_value) const {
    return dispatch_ ? dispatch_->post(std::move(task_value))
                     : web_worker_post_result_type::reject(post_status::worker_stopping, std::move(task_value));
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

web_worker_dispatch::web_worker_dispatch(ruvia::worker_runtime& runtime, std::pmr::memory_resource* resource,
    worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value)
    : worker_(runtime.context().handle()),
      runtime_(runtime),
      resource_(pmr_resource_or_default(resource)),
      client_registries_(capabilities.client_registries()),
      worker_states_(capabilities.worker_states()),
      blocking_pool_(capabilities.get_blocking_pool()),
      jobs_(jobs),
      stop_token_(stop_token_value) {}

web_worker_handle web_worker_dispatch::handle(std::shared_ptr<web_worker_dispatch> dispatch) noexcept {
    return web_worker_handle(std::move(dispatch));
}

bool web_worker_dispatch::valid() const noexcept {
    return worker_.valid();
}

bool web_worker_dispatch::accepting() const noexcept {
    return worker_.accepting();
}

worker_id_type web_worker_dispatch::id() const noexcept {
    return worker_.id();
}

web_worker_post_result_type web_worker_dispatch::post(job_type job) {
    // Reserve before entering the core queue; core invokes the factory only
    // for an accepted post and destroys an unrun job on abandonment.
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    abandon_reservation_type reservation(this);
    const auto status = worker_.post_factory([&job, &reservation]() mutable -> move_only_function<void()> {
        return [job = std::move(job), reservation = std::move(reservation)]() mutable {
            reservation.release()->start(std::move(job));
        };
    });
    post_counters_.record(status);
    if (status == post_status::accepted) {
        return web_worker_post_result_type::accept();
    }
    return web_worker_post_result_type::reject(status, std::move(job));
}

web_worker_stats web_worker_dispatch::stats() const noexcept {
    return web_worker_stats{
        .accepted_ = post_counters_.accepted(),
        .queue_full_ = post_counters_.queue_full(),
        .worker_stopping_ = post_counters_.worker_stopping(),
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
        web_worker_context context(
            worker_, resource_, client_registries_, &worker_states_, blocking_pool_, stop_token_);
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
