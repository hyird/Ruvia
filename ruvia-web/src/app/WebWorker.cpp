#include "ruvia/web/WebWorker.h"

#include <asio/bind_executor.hpp>
#include <asio/post.hpp>

#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/redis/RedisInternal.h"

namespace ruvia {

WebWorkerContext::WebWorkerContext(
    WorkerHandle worker,
    std::pmr::memory_resource* resource,
    detail::DbRegistry* databases,
    detail::RedisRegistry* redis,
    std::stop_token stopToken) noexcept
    : worker_(std::move(worker)),
      resource_(detail::pmrResourceOrDefault(resource)),
      databases_(databases),
      redis_(redis),
      stopToken_(stopToken) {}

const WorkerHandle& WebWorkerContext::worker() const & noexcept {
    return worker_;
}

std::pmr::memory_resource* WebWorkerContext::resource() const noexcept {
    return resource_;
}

std::stop_token WebWorkerContext::stopToken() const noexcept {
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

#ifdef RUVIA_ENABLE_REDIS
RedisHandle WebWorkerContext::redis() const {
    return redis_->get(resource_, operationScope_);
}

RedisHandle WebWorkerContext::redis(std::string_view alias) const {
    return redis_->get(alias, resource_, operationScope_);
}
#endif

WebWorkerHandle::WebWorkerHandle(
    std::shared_ptr<detail::WebWorkerDispatch> dispatch) noexcept
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

PostResult WebWorkerHandle::postTask(
    std::move_only_function<Task<void>(WebWorkerContext&)> task) const {
    return dispatch_
        ? dispatch_->post(std::move(task))
        : PostResult::kWorkerStopping;
}

}  // namespace ruvia

namespace ruvia::detail {

WebWorkerDispatch::WebWorkerDispatch(
    asio::any_io_executor executor,
    WorkerHandle worker,
    std::pmr::memory_resource* resource,
    DbRegistry& databases,
    RedisRegistry& redis,
    std::move_only_function<void()> drained,
    std::move_only_function<void(std::exception_ptr)> failed)
    : executor_(std::move(executor)),
      worker_(std::move(worker)),
      resource_(pmrResourceOrDefault(resource)),
      databases_(&databases),
      redis_(&redis),
      drained_(std::move(drained)),
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

PostResult WebWorkerDispatch::post(Task task) {
    std::lock_guard lock(submitMutex_);
    if (!accepting_) {
        workerStopping_.fetch_add(1, std::memory_order_relaxed);
        return PostResult::kWorkerStopping;
    }

    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    const auto result = worker_.post(
        [this, task = std::move(task)]() mutable { start(std::move(task)); });
    if (result != PostResult::kAccepted) {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }
    switch (result) {
    case PostResult::kAccepted:
        accepted_.fetch_add(1, std::memory_order_relaxed);
        break;
    case PostResult::kQueueFull:
        queueFull_.fetch_add(1, std::memory_order_relaxed);
        break;
    case PostResult::kWorkerStopping:
        workerStopping_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    return result;
}

void WebWorkerDispatch::close() noexcept {
    std::lock_guard lock(submitMutex_);
    accepting_ = false;
    stopSource_.request_stop();
}

void WebWorkerDispatch::retire() noexcept {
    std::lock_guard lock(submitMutex_);
    accepting_ = false;
    stopSource_.request_stop();
    if (outstanding_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
    // A public handle may keep this terminal endpoint alive after HttpServer.
    // Remove every callback/pointer into server-owned state before that state is
    // destroyed; terminal queries use only atomics and the stable WorkerHandle.
    drained_ = nullptr;
    failed_ = nullptr;
    databases_ = nullptr;
    redis_ = nullptr;
    resource_ = nullptr;
    executor_ = asio::any_io_executor{};
}

bool WebWorkerDispatch::accepting() const noexcept {
    std::lock_guard lock(submitMutex_);
    return accepting_ && worker_.accepting();
}

std::size_t WebWorkerDispatch::outstanding() const noexcept {
    return outstanding_.load(std::memory_order_acquire);
}

WebWorkerStats WebWorkerDispatch::stats() const noexcept {
    return WebWorkerStats{
        .accepted = accepted_.load(std::memory_order_relaxed),
        .queueFull = queueFull_.load(std::memory_order_relaxed),
        .workerStopping = workerStopping_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_relaxed),
        .failed = failedCount_.load(std::memory_order_relaxed),
        .outstanding = outstanding_.load(std::memory_order_acquire),
    };
}

void WebWorkerDispatch::start(Task task) {
    try {
        auto operation = run(std::move(task));
        asyncStartTask(
            std::move(operation),
            asio::bind_executor(
                executor_,
                [this](TaskCompletionResult<void> result) {
                    if (const auto* failure = result.failure()) {
                        failedCount_.fetch_add(1, std::memory_order_relaxed);
                        if (failed_) {
                            failed_(failure->exception());
                        }
                    }
                    complete();
                }));
    } catch (...) {
        complete();
        throw;
    }
}

ruvia::Task<void> WebWorkerDispatch::run(Task task) {
    WebWorkerContext context(
        worker_, resource_, databases_, redis_, stopSource_.get_token());
    co_await task(context);
}

void WebWorkerDispatch::complete() {
    completed_.fetch_add(1, std::memory_order_relaxed);
    if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1 && drained_) {
        drained_();
    }
}

}  // namespace ruvia::detail
