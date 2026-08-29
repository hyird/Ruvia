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
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/integration/BlockingCapability.h"
#include "ruvia/web/detail/integration/WorkerClientRegistryView.h"
#include "ruvia/web/detail/integration/WorkerStateCapability.h"
#include "ruvia/web/detail/integration/WorkerState.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia {

namespace detail {
class WebWorkerDispatch;
class WorkerStateRegistry;
}  // namespace detail

class WebWorkerContext final : public detail::BlockingCapability<WebWorkerContext>,
                               public detail::WorkerStateCapability<WebWorkerContext> {
public:
    WebWorkerContext(const WebWorkerContext&) = delete;
    WebWorkerContext& operator=(const WebWorkerContext&) = delete;
    WebWorkerContext(WebWorkerContext&&) = delete;
    WebWorkerContext& operator=(WebWorkerContext&&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;
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
    [[nodiscard]] HttpClientHandle httpClient() const;
    [[nodiscard]] HttpClientHandle httpClient(std::string_view alias) const;

private:
    friend class detail::WebWorkerDispatch;

    WebWorkerContext(const WorkerHandle& worker, std::pmr::memory_resource* resource,
        detail::WorkerClientRegistryView clientRegistries,
        const detail::WorkerStateRegistry* workerStates, BlockingPool* blockingPool,
        const StopToken& stopToken) noexcept;
    WebWorkerContext(WorkerHandle&&, std::pmr::memory_resource*, detail::WorkerClientRegistryView,
        const detail::WorkerStateRegistry*, BlockingPool*, const StopToken&) = delete;
    WebWorkerContext(const WorkerHandle&, std::pmr::memory_resource*,
        detail::WorkerClientRegistryView, const detail::WorkerStateRegistry*, BlockingPool*,
        StopToken&&) = delete;
    WebWorkerContext(WorkerHandle&&, std::pmr::memory_resource*, detail::WorkerClientRegistryView,
        const detail::WorkerStateRegistry*, BlockingPool*, StopToken&&) = delete;

    [[nodiscard]] void* workerStateInstance(const void* typeKey) const;
    friend class detail::BlockingCapability<WebWorkerContext>;
    friend class detail::WorkerStateCapability<WebWorkerContext>;
    [[nodiscard]] BlockingPool& blockingPool() const;
    [[nodiscard]] const WorkerHandle& blockingWorker() const noexcept {
        return worker_;
    }
    [[nodiscard]] StopToken blockingStopToken() const noexcept {
        return stopToken_;
    }

    // WebWorkerDispatch owns these stable values until every posted task has
    // completed. Contexts borrow them so starting a task does not copy endpoint
    // or cancellation-state ownership on the worker thread.
    const WorkerHandle& worker_;
    std::pmr::memory_resource* resource_;
    detail::WorkerClientRegistryView clientRegistries_;
    const detail::WorkerStateRegistry* workerStates_;
    BlockingPool* blockingPool_;
    const StopToken& stopToken_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable detail::ScopedOperationScope operationScope_;
};

using WebWorkerPostResult = PostOutcome<Task<void>(WebWorkerContext&)>;

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
                 std::same_as<std::invoke_result_t<std::decay_t<Fn>&, WebWorkerContext&>,
                     Task<void>>
    [[nodiscard]] WebWorkerPostResult post(Fn&& fn) const {
        return postTask(MoveOnlyFunction<Task<void>(WebWorkerContext&)>(std::forward<Fn>(fn)));
    }

private:
    friend class detail::WebWorkerDispatch;

    WebWorkerHandle(std::shared_ptr<detail::WebWorkerDispatch> dispatch) noexcept;

    [[nodiscard]] WebWorkerPostResult postTask(
        MoveOnlyFunction<Task<void>(WebWorkerContext&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::WebWorkerDispatch> dispatch_;
};

}  // namespace ruvia
