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
class DataAccessServiceState;
}

class DataAccessContext;

using DataAccessPostResult = PostOutcome<Task<void>(DataAccessContext&)>;

struct DataAccessStats final {
    std::uint64_t accepted{0};
    std::uint64_t queueFull{0};
    std::uint64_t workerStopping{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::size_t outstanding{0};
};

#ifdef RUVIA_ENABLE_DATABASE
struct DataAccessDatabaseConfig final {
    std::string alias{"default"};
    DbConfig config;
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct DataAccessRedisConfig final {
    std::string alias{"default"};
    RedisConfig config;
};
#endif

struct DataAccessOptions final {
    std::chrono::milliseconds maintenanceInterval{std::chrono::seconds(1)};
#ifdef RUVIA_ENABLE_DATABASE
    std::vector<DataAccessDatabaseConfig> databases;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::vector<DataAccessRedisConfig> redis;
#endif
    // Runs on the bound worker when a posted job lets an exception escape.
    // With no callback, the exception fails the event-loop runner; throwing
    // from the callback has the same worker-fatal effect.
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler;
};

// A short-lived capability scope for one coroutine/job running on the bound
// event loop. Handles obtained from it expire with the context, so the context
// and its handles must not escape that job.
class DataAccessContext final {
public:
    DataAccessContext(const DataAccessContext&) = delete;
    DataAccessContext& operator=(const DataAccessContext&) = delete;
    DataAccessContext(DataAccessContext&&) = delete;
    DataAccessContext& operator=(DataAccessContext&&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;
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
    friend class DataAccessService;
    friend class detail::DataAccessServiceState;

    explicit DataAccessContext(std::shared_ptr<detail::DataAccessServiceState> state) noexcept;

    std::shared_ptr<detail::DataAccessServiceState> state_;
    // Declared last so cold operations and borrowed handles expire while the
    // data access state and its memory resource are still alive.
    mutable detail::ScopedOperationScope operationScope_;
};

// Attaches database and Redis pools plus job-lifetime tracking to one
// application-created core EventLoop. It does not create or own the event
// loop's thread or io_context.
// connect(), post(), and stats() may be called from any thread. Destruction may
// also occur on any thread after concurrent calls on this object have ceased;
// close() is worker-affine. EventLoop shutdown closes the pools on that same
// worker. post() is the only public operation-scope entry point, so every
// context and pool lease remains covered by outstanding-job tracking.
class DataAccessService final {
public:
    explicit DataAccessService(EventLoop loop, DataAccessOptions options = {});
    ~DataAccessService();

    DataAccessService(const DataAccessService&) = delete;
    DataAccessService& operator=(const DataAccessService&) = delete;
    DataAccessService(DataAccessService&&) = delete;
    DataAccessService& operator=(DataAccessService&&) = delete;

    // Schedules connection startup on the bound loop. It is valid to call this
    // before EventLoopPool::start(); wait on the returned future only after the
    // loop has a thread driving it. If startup cannot be scheduled because the
    // loop is stopping, the submission gate is closed before the future reports
    // failure, so later post() calls deterministically return kWorkerStopping.
    [[nodiscard]] std::future<void> connect();

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&, DataAccessContext&> && std::same_as<std::invoke_result_t<std::decay_t<Fn>&, DataAccessContext&>, Task<void>>
    [[nodiscard]] DataAccessPostResult post(Fn&& fn) {
        return postTask(MoveOnlyFunction<Task<void>(DataAccessContext&)>(std::forward<Fn>(fn)));
    }

    void close();
    [[nodiscard]] DataAccessStats stats() const noexcept;
    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    [[nodiscard]] DataAccessPostResult postTask(MoveOnlyFunction<Task<void>(DataAccessContext&)> task);

    std::shared_ptr<detail::DataAccessServiceState> state_;
};

}  // namespace ruvia
