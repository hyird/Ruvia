#include "ruvia/web/WebWorker.h"

#include <asio/bind_executor.hpp>
#include <asio/post.hpp>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

namespace ruvia {

WebWorkerContext::WebWorkerContext(WorkerHandle worker, std::pmr::memory_resource* resource, detail::DbRegistry* databases, detail::RedisRegistry* redis, detail::HttpClientRegistry* httpClients, const detail::WorkerStateRegistry* workerStates, BlockingPool* blockingPool, StopToken stopToken) noexcept
    : worker_(std::move(worker)),
      resource_(detail::pmrResourceOrDefault(resource)),
      databases_(databases),
      redis_(redis),
      httpClients_(httpClients),
      workerStates_(workerStates),
      blockingPool_(blockingPool),
      stopToken_(stopToken) {}

BlockingPool& WebWorkerContext::blockingPool() const {
    if (blockingPool_ == nullptr) {
        throw std::logic_error("no blocking pool is configured: call App::setBlockingPool() before App::run()");
    }
    return *blockingPool_;
}

void* WebWorkerContext::workerStateInstance(const void* typeKey) const {
    auto* instance = workerStates_ == nullptr ? nullptr : workerStates_->instance(typeKey);
    if (instance == nullptr) {
        throw std::logic_error("worker state type is not registered: call App::useWorkerState<T>() before App::run()");
    }
    return instance;
}

const WorkerHandle& WebWorkerContext::worker() const& noexcept {
    return worker_;
}

std::pmr::memory_resource* WebWorkerContext::resource() const noexcept {
    return resource_;
}

StopToken WebWorkerContext::stopToken() const noexcept {
    return stopToken_;
}

#ifdef RUVIA_ENABLE_DATABASE
DbHandle WebWorkerContext::db() const {
    return databases_->get(resource_, operationScope_);
}

DbHandle WebWorkerContext::db(std::string_view alias) const {
    return databases_->get(alias, resource_, operationScope_);
}
#endif

HttpClient WebWorkerContext::httpClient() const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(resource_, operationScope_);
}

HttpClient WebWorkerContext::httpClient(std::string_view alias) const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(alias, resource_, operationScope_);
}

#ifdef RUVIA_ENABLE_REDIS
RedisHandle WebWorkerContext::redis() const {
    return redis_->get(resource_, operationScope_);
}

RedisHandle WebWorkerContext::redis(std::string_view alias) const {
    return redis_->get(alias, resource_, operationScope_);
}
#endif

WebWorkerHandle::WebWorkerHandle(std::shared_ptr<detail::WebWorkerDispatch> dispatch) noexcept
    : dispatch_(std::move(dispatch)) {}

bool WebWorkerHandle::valid() const noexcept {
    return dispatch_ && dispatch_->valid();
}

bool WebWorkerHandle::accepting() const noexcept {
    return dispatch_ && dispatch_->accepting();
}

WorkerId WebWorkerHandle::id() const noexcept {
    return dispatch_ ? dispatch_->id() : 0;
}

WebWorkerStats WebWorkerHandle::stats() const noexcept {
    return dispatch_ ? dispatch_->stats() : WebWorkerStats{};
}

WebWorkerPostResult WebWorkerHandle::postTask(MoveOnlyFunction<Task<void>(WebWorkerContext&)> task) const {
    return dispatch_ ? dispatch_->post(std::move(task)) : WebWorkerPostResult::reject(PostStatus::kWorkerStopping, std::move(task));
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

// A move-safe reservation for the outstanding_ count post() takes before the
// start-lambda runs. It rides inside the posted lambda; unique_ptr move semantics
// keep exactly one owner as MoveOnlyFunction relocates the lambda. If the
// lambda runs it release()s the reservation and complete() owns the decrement; if
// the lambda is destroyed unrun (rejected post, or shutdown abandoning queued
// mailbox work), the deleter reconciles the count.
struct AbandonReservationDeleter {
    void operator()(WebWorkerDispatch* dispatch) const noexcept {
        dispatch->abandon();
    }
};
using AbandonReservation = std::unique_ptr<WebWorkerDispatch, AbandonReservationDeleter>;

}  // namespace

WebWorkerDispatch::WebWorkerDispatch(asio::any_io_executor executor, WorkerHandle worker, std::pmr::memory_resource* resource, DbRegistry& databases, RedisRegistry& redis, HttpClientRegistry& httpClients, const WorkerStateRegistry& workerStates, BlockingPool* blockingPool, MoveOnlyFunction<void(std::exception_ptr)> failed)
    : executor_(std::move(executor)),
      worker_(std::move(worker)),
      resource_(pmrResourceOrDefault(resource)),
      databases_(&databases),
      redis_(&redis),
      httpClients_(&httpClients),
      workerStates_(&workerStates),
      blockingPool_(blockingPool),
      failed_(std::move(failed)) {}

WebWorkerDispatch::~WebWorkerDispatch() {
    if (outstanding_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
}

WebWorkerHandle WebWorkerDispatch::handle() {
    return WebWorkerHandle(shared_from_this());
}

bool WebWorkerDispatch::valid() const noexcept {
    return worker_.valid();
}

WorkerId WebWorkerDispatch::id() const noexcept {
    return worker_.id();
}

WebWorkerPostResult WebWorkerDispatch::post(Task task) {
    std::lock_guard lock(submitMutex_);
    if (!accepting_) {
        postCounters_.recordWorkerStopping();
        return WebWorkerPostResult::reject(PostStatus::kWorkerStopping, std::move(task));
    }

    const auto status = WorkerHandleAccess::postFactory(worker_, [this, &task]() mutable -> MoveOnlyFunction<void()> {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        AbandonReservation reservation(this);
        return [task = std::move(task), reservation = std::move(reservation)]() mutable {
            WebWorkerDispatch* self = reservation.release();
            self->start(std::move(task));
        };
    });
    postCounters_.record(status);
    return status == PostStatus::kAccepted ? WebWorkerPostResult::accept() : WebWorkerPostResult::reject(status, std::move(task));
}

void WebWorkerDispatch::close() noexcept {
    std::lock_guard lock(submitMutex_);
    accepting_ = false;
    stopSource_.requestStop();
}

void WebWorkerDispatch::retire() noexcept {
    std::lock_guard lock(submitMutex_);
    accepting_ = false;
    stopSource_.requestStop();
    if (outstanding_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
    // A public handle may keep this terminal endpoint alive after HttpServer.
    // Remove every callback/pointer into server-owned state before that state is
    // destroyed; terminal queries use only atomics and the stable WorkerHandle.
    failed_ = nullptr;
    databases_ = nullptr;
    redis_ = nullptr;
    httpClients_ = nullptr;
    resource_ = nullptr;
    executor_ = asio::any_io_executor{};
}

bool WebWorkerDispatch::accepting() const noexcept {
    std::lock_guard lock(submitMutex_);
    return accepting_ && worker_.accepting();
}

WebWorkerStats WebWorkerDispatch::stats() const noexcept {
    return WebWorkerStats{
        .accepted = postCounters_.accepted(),
        .queueFull = postCounters_.queueFull(),
        .workerStopping = postCounters_.workerStopping(),
        .completed = completed_.load(std::memory_order_relaxed),
        .failed = failedCount_.load(std::memory_order_relaxed),
        .outstanding = outstanding_.load(std::memory_order_acquire),
    };
}

void WebWorkerDispatch::start(Task task) {
    try {
        auto operation = run(std::move(task));
        asyncStartTask(std::move(operation), asio::bind_executor(executor_, [this](TaskCompletionResult<void> result) {
            std::exception_ptr failure;
            if (const auto* failed = result.failure()) {
                failedCount_.fetch_add(1, std::memory_order_relaxed);
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

ruvia::Task<void> WebWorkerDispatch::run(Task task) {
    WebWorkerContext context(worker_, resource_, databases_, redis_, httpClients_, workerStates_, blockingPool_, stopSource_.token());
    co_await task(context);
}

void WebWorkerDispatch::complete() noexcept {
    completed_.fetch_add(1, std::memory_order_relaxed);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

void WebWorkerDispatch::abandon() noexcept {
    // A start-lambda was destroyed without running. Reconcile only the reservation
    // post() took; this is not a completion, so it fires no drained_ and records
    // nothing (a rejected post is already counted via post()'s switch).
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace ruvia::detail
