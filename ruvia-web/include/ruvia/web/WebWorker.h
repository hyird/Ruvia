#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/integration/WorkerState.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia {

namespace detail {
class DbRegistry;
class HttpServer;
class RedisRegistry;
class WebWorkerDispatch;
class WorkerStateRegistry;
}

class WebWorkerContext final {
public:
    WebWorkerContext(const WebWorkerContext&) = delete;
    WebWorkerContext& operator=(const WebWorkerContext&) = delete;
    WebWorkerContext(WebWorkerContext&&) = delete;
    WebWorkerContext& operator=(WebWorkerContext&&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const & noexcept;
    const WorkerHandle& worker() const && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] StopToken stopToken() const noexcept;

    // This worker's instance of an App::useWorkerState<T>() registration --
    // the same instance Context::workerState<T>() returns for HTTP requests
    // dispatched on this worker. Throws std::logic_error for an unregistered
    // type.
    template <typename T>
    [[nodiscard]] T& workerState() const {
        return *static_cast<T*>(
            workerStateInstance(detail::workerStateTypeKey<T>()));
    }

    // Offloads blocking work exactly as Context::runBlocking() does for
    // requests -- same pool, same ownership rule for the callable, same
    // exceptions. A posted background task blocks its worker just as a handler
    // would.
    template <typename Fn>
    [[nodiscard]] Task<std::invoke_result_t<Fn&>> runBlocking(Fn fn) const {
        auto result = co_await tryRunBlocking(std::move(fn));
        if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
            std::move(result).value();
            co_return;
        } else {
            co_return std::move(result).value();
        }
    }

    template <typename Rep, typename Period, typename Fn>
    [[nodiscard]] Task<std::invoke_result_t<Fn&>> runBlocking(
        std::chrono::duration<Rep, Period> timeout,
        Fn fn) const {
        auto result = co_await tryRunBlocking(timeout, std::move(fn));
        if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
            std::move(result).value();
            co_return;
        } else {
            co_return std::move(result).value();
        }
    }

    template <typename Fn>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<Fn&>>>
    tryRunBlocking(Fn fn) const {
        return ruvia::runBlocking(blockingPool(), worker_, std::move(fn));
    }

    template <typename Rep, typename Period, typename Fn>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<Fn&>>>
    tryRunBlocking(std::chrono::duration<Rep, Period> timeout, Fn fn) const {
        return ruvia::runBlocking(blockingPool(), worker_, timeout, std::move(fn));
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif

private:
    friend class detail::WebWorkerDispatch;

    WebWorkerContext(
        WorkerHandle worker,
        std::pmr::memory_resource* resource,
        detail::DbRegistry* databases,
        detail::RedisRegistry* redis,
        const detail::WorkerStateRegistry* workerStates,
        BlockingPool* blockingPool,
        StopToken stopToken) noexcept;

    [[nodiscard]] void* workerStateInstance(const void* typeKey) const;
    [[nodiscard]] BlockingPool& blockingPool() const;

    WorkerHandle worker_;
    std::pmr::memory_resource* resource_;
    [[maybe_unused]] detail::DbRegistry* databases_;
    [[maybe_unused]] detail::RedisRegistry* redis_;
    const detail::WorkerStateRegistry* workerStates_;
    BlockingPool* blockingPool_;
    StopToken stopToken_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable detail::ScopedOperationScope operationScope_;
};

using WebWorkerPostResult =
    PostOutcome<Task<void>(WebWorkerContext&)>;

struct WebWorkerStats final {
    std::uint64_t accepted{0};
    std::uint64_t queueFull{0};
    std::uint64_t workerStopping{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::size_t outstanding{0};
};

class WebWorkerHandle final {
public:
    WebWorkerHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;
    [[nodiscard]] WebWorkerStats stats() const noexcept;

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&, WebWorkerContext&> &&
                 std::same_as<
                     std::invoke_result_t<std::decay_t<Fn>&, WebWorkerContext&>,
                     Task<void>>
    [[nodiscard]] WebWorkerPostResult post(Fn&& fn) const {
        return postTask(MoveOnlyFunction<Task<void>(WebWorkerContext&)>(
            std::forward<Fn>(fn)));
    }

private:
    friend class detail::WebWorkerDispatch;

    WebWorkerHandle(
        std::shared_ptr<detail::WebWorkerDispatch> dispatch) noexcept;

    [[nodiscard]] WebWorkerPostResult postTask(
        MoveOnlyFunction<Task<void>(WebWorkerContext&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::WebWorkerDispatch> dispatch_;
};

}  // namespace ruvia
