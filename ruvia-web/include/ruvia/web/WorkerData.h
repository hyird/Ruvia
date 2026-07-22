#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/ScopedOperation.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#include "ruvia/web/db/DbTypes.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/redis/RedisTypes.h"
#endif

namespace ruvia {

namespace detail {
class WorkerDataRuntimeState;
}

class WorkerDataContext;

using WorkerDataPostResult =
    PostOutcome<Task<void>(WorkerDataContext&)>;

struct WorkerDataStats final {
    std::uint64_t accepted{0};
    std::uint64_t queueFull{0};
    std::uint64_t workerStopping{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::size_t outstanding{0};
};

#ifdef RUVIA_ENABLE_DATABASE
struct WorkerDatabaseConfig final {
    std::string alias{"default"};
    DbConfig config;
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct WorkerRedisConfig final {
    std::string alias{"default"};
    RedisConfig config;
};
#endif

struct WorkerDataOptions final {
    std::chrono::milliseconds maintenanceInterval{
        std::chrono::seconds(1)};
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<WorkerDatabaseConfig> databases;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<WorkerRedisConfig> redis;
#endif
    // Runs on the bound worker when a posted job lets an exception escape.
    // With no callback, the exception fails the event-loop runner; throwing
    // from the callback has the same worker-fatal effect.
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler;
};

// A short-lived capability scope for one coroutine/job running on the bound
// event loop. Handles obtained from it expire with the context, so the context
// and its handles must not escape that job.
class WorkerDataContext final {
public:
    WorkerDataContext(const WorkerDataContext&) = delete;
    WorkerDataContext& operator=(const WorkerDataContext&) = delete;
    WorkerDataContext(WorkerDataContext&&) = delete;
    WorkerDataContext& operator=(WorkerDataContext&&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const & noexcept;
    const WorkerHandle& worker() const && = delete;
    // Borrowed job-local allocation domain. Objects using this resource,
    // including DB/Redis results, must be destroyed before the posted job
    // returns.
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] StopToken stopToken() const noexcept;

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif

private:
    friend class WorkerDataRuntime;
    friend class detail::WorkerDataRuntimeState;

    explicit WorkerDataContext(
        std::shared_ptr<detail::WorkerDataRuntimeState> state) noexcept;

    std::shared_ptr<detail::WorkerDataRuntimeState> state_;
    // Declared last so cold operations and borrowed handles expire while the
    // worker data state and its memory resource are still alive.
    mutable detail::ScopedOperationScope operationScope_;
};

// Owns database and Redis pools for one application-created core EventLoop.
// connect(), post(), and stats() may be called from any thread. Destruction may
// also occur on any thread after concurrent calls on this object have ceased;
// close() is worker-affine. EventLoop shutdown closes the pools on that same
// worker. post() is the only public operation-scope entry point, so every
// context and pool lease remains covered by outstanding-job tracking.
class WorkerDataRuntime final {
public:
    explicit WorkerDataRuntime(
        EventLoop loop,
        WorkerDataOptions options = {});
    ~WorkerDataRuntime();

    WorkerDataRuntime(const WorkerDataRuntime&) = delete;
    WorkerDataRuntime& operator=(const WorkerDataRuntime&) = delete;
    WorkerDataRuntime(WorkerDataRuntime&&) = delete;
    WorkerDataRuntime& operator=(WorkerDataRuntime&&) = delete;

    // Schedules connection startup on the bound loop. It is valid to call this
    // before EventLoopPool::start(); wait on the returned future only after the
    // loop has a thread driving it. If startup cannot be scheduled because the
    // loop is stopping, the submission gate is closed before the future reports
    // failure, so later post() calls deterministically return kWorkerStopping.
    [[nodiscard]] std::future<void> connect();

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&, WorkerDataContext&> &&
                 std::same_as<
                     std::invoke_result_t<std::decay_t<Fn>&, WorkerDataContext&>,
                     Task<void>>
    [[nodiscard]] WorkerDataPostResult post(Fn&& fn) {
        return postTask(MoveOnlyFunction<Task<void>(WorkerDataContext&)>(
            std::forward<Fn>(fn)));
    }

    void close();
    [[nodiscard]] WorkerDataStats stats() const noexcept;
    [[nodiscard]] const WorkerHandle& worker() const & noexcept;
    const WorkerHandle& worker() const && = delete;

private:
    [[nodiscard]] WorkerDataPostResult postTask(
        MoveOnlyFunction<Task<void>(WorkerDataContext&)> task);

    std::shared_ptr<detail::WorkerDataRuntimeState> state_;
};

}  // namespace ruvia
