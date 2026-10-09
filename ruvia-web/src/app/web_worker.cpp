#include "ruvia/web/web_worker.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/post.hpp>

#include "ruvia/core/async.h"
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
      resource_(detail::pmr_resource_or_default(resource)),
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

// A move-safe reservation for the outstanding_ count post() takes before the
// start-lambda runs. It rides inside the posted lambda; unique_ptr move semantics
// keep exactly one owner as move_only_function relocates the lambda. If the
// lambda runs it release()s the reservation and complete() owns the decrement; if
// the lambda is destroyed unrun (rejected post, or shutdown abandoning queued
// queue work), the deleter reconciles the count.
struct abandon_reservation_deleter {
    void operator()(web_worker_dispatch* dispatch) const noexcept {
        dispatch->abandon();
    }
};
using abandon_reservation_type = std::unique_ptr<web_worker_dispatch, abandon_reservation_deleter>;

}  // namespace

web_worker_dispatch::web_worker_dispatch(asio::any_io_executor executor, worker_handle worker_value,
    std::pmr::memory_resource* resource, worker_capabilities& capabilities,
    move_only_function<void(std::exception_ptr)> failed)
    : executor_(std::move(executor)),
      worker_(std::move(worker_value)),
      resource_(pmr_resource_or_default(resource)),
      client_registries_(capabilities.client_registries()),
      worker_states_(&capabilities.worker_states()),
      blocking_pool_(capabilities.get_blocking_pool()),
      failed_(std::move(failed)) {}

web_worker_dispatch::~web_worker_dispatch() {
    if (outstanding_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
}

web_worker_handle web_worker_dispatch::handle() {
    return web_worker_handle(shared_from_this());
}

bool web_worker_dispatch::valid() const noexcept {
    return worker_.valid();
}

worker_id_type web_worker_dispatch::id() const noexcept {
    return worker_.id();
}

web_worker_post_result_type web_worker_dispatch::post(task task_value) {
    bool accepting = false;
    {
        std::lock_guard lock(submit_mutex_);
        accepting = accepting_;
    }
    if (!accepting) {
        post_counters_.record_worker_stopping();
        return web_worker_post_result_type::reject(post_status::worker_stopping, std::move(task_value));
    }

    // Reserve before entering the core queue. The core factory may be delayed
    // past detach; its only obligation is then to return an abandonment guard.
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    abandon_reservation_type reservation(this);
    const auto status = (worker_).post_factory([&task_value, &reservation]() mutable -> move_only_function<void()> {
        return [task_value = std::move(task_value), reservation = std::move(reservation)]() mutable {
            web_worker_dispatch* self = reservation.release();
            self->start(std::move(task_value));
        };
    });
    post_counters_.record(status);
    if (status == post_status::accepted) {
        return web_worker_post_result_type::accept();
    }
    return web_worker_post_result_type::reject(status, std::move(task_value));
}

void web_worker_dispatch::close() noexcept {
    {
        std::lock_guard lock(submit_mutex_);
        accepting_ = false;
    }
    stop_source_.request_stop();
}

void web_worker_dispatch::retire() noexcept {
    {
        std::lock_guard lock(submit_mutex_);
        accepting_ = false;
    }
    stop_source_.request_stop();
    if (active_started_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
    // A public handle may keep this terminal endpoint alive after web_worker_runtime.
    // Remove every callback/pointer into server-owned state before that state is
    // destroyed; terminal queries use only atomics and the stable worker_handle.
    failed_ = nullptr;
    client_registries_ = worker_client_registry_view::detached();
    worker_states_ = nullptr;
    blocking_pool_ = nullptr;
    resource_ = nullptr;
    executor_ = asio::any_io_executor{};
}

bool web_worker_dispatch::accepting() const noexcept {
    std::lock_guard lock(submit_mutex_);
    return accepting_ && worker_.accepting();
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

void web_worker_dispatch::start(task task_value) {
    // Closing abandons factories that have not started. Runtime retirement now
    // drains reserved queue publication before detach; it must not turn that
    // quiescence drain into new application work against retired capabilities.
    if (stop_source_.stop_requested() || !worker_.accepting()) {
        abandon();
        return;
    }
    active_started_.fetch_add(1, std::memory_order_acq_rel);
    try {
        auto operation = run(std::move(task_value));
        async_start_task(std::move(operation),
            asio::bind_executor(executor_, [this](const task_completion_result<void>& result_value) {
                std::exception_ptr failure;
                if (const auto* failed = result_value.failure()) {
                    failed_count_.fetch_add(1, std::memory_order_relaxed);
                    failure = failed->exception();
                }
                // Reconcile the accepted task before invoking the failure
                // sink. The sink normally stops this worker and is allowed
                // to trigger arbitrary terminal control flow; no such path
                // may leave retire() observing a phantom outstanding job.
                complete();
                if (failure != nullptr && failed_) {
                    failed_(std::move(failure));
                }
            }));
    } catch (...) {
        complete();
        throw;
    }
}

ruvia::task<void> web_worker_dispatch::run(task task_value) {
    web_worker_context context(
        worker_, resource_, client_registries_, worker_states_, blocking_pool_, stop_token_);
    co_await task_value(context);
}

void web_worker_dispatch::complete() noexcept {
    completed_.fetch_add(1, std::memory_order_relaxed);
    active_started_.fetch_sub(1, std::memory_order_acq_rel);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

void web_worker_dispatch::abandon() noexcept {
    // A start-lambda was destroyed without running. Reconcile only the reservation
    // post() took; this is not a completion, so it fires no drained_ and records
    // nothing (a rejected post is already counted via post()'s switch).
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace ruvia::detail
