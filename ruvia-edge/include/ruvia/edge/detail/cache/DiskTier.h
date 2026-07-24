#pragma once

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include "ruvia/edge/detail/cache/DiskCache.h"

namespace ruvia::edge {

// The threading policy around the persistent tier: it owns a DiskCache and the
// single background thread every blocking filesystem call runs on, so the event
// loop never touches the disk itself. DiskCache stays synchronous and
// loop-agnostic; this is the only place that decides where its work executes.
//
// Constructed without a directory the tier is disabled: no thread is started,
// every operation is a no-op and a lookup always misses. That keeps the callers
// free of "is there a disk?" branching on the request path.
//
// Fire-and-forget operations (store, purge) are queued and may still be pending
// when the tier is stopped. The synchronous ones (purgePrefixSync, clearSync)
// block for their result and remain correct after stop(): they then run inline
// on the calling thread, ordered after everything the pool already drained.
class DiskTier final {
public:
    // What a queued operation reports when it ends by throwing. It runs on the
    // disk thread, concurrently with the Edge worker.
    using FailureSink = std::function<void(std::exception_ptr)>;

    // Throws std::filesystem::filesystem_error if a directory is given but
    // cannot be created, scanned, or exclusively leased (see DiskCache).
    DiskTier(const std::optional<std::filesystem::path>& directory, std::size_t maxBytes, FailureSink onFailure = {});
    ~DiskTier();

    DiskTier(const DiskTier&) = delete;
    DiskTier& operator=(const DiskTier&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return cache_.has_value();
    }

    // Reads the entry off the loop and resumes the caller with the result.
    [[nodiscard]] asio::awaitable<std::optional<CachedResponse>> lookup(std::string key);

    void store(std::string key, CachedResponse entry);
    void purge(std::string key);
    void purgePrefix(std::string prefix);

    [[nodiscard]] DiskCache::PurgeResult purgePrefixSync(std::string prefix);
    [[nodiscard]] bool clearSync();

    // Drain the queue and join the thread. Idempotent; further synchronous
    // operations run inline.
    void stop() noexcept;

private:
    // Run `work` on the pool and wait for its result. Holding the stop lock
    // across the post is what orders a concurrent stop(): the work is either
    // queued before the join (and therefore drains with everything ahead of it)
    // or observes the joined tier and runs inline.
    template <typename Work>
    [[nodiscard]] auto runSync(Work work) -> decltype(work()) {
        using Result = decltype(work());
        std::unique_lock lock(stopMutex_);
        if (stopped_) {
            lock.unlock();
            return work();
        }
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(work));
        auto result = task->get_future();
        asio::post(*pool_, [task = std::move(task)] { (*task)(); });
        lock.unlock();
        return result.get();
    }

    // Runs `work` on the disk thread as a queued operation. Nothing awaits the
    // result, so this wrapper is the only place an exception from it can be
    // seen; without it the throw would leave the pool's thread and terminate.
    template <typename Work>
    void runQueued(Work work) {
        asio::post(*pool_, [this, work = std::move(work)]() mutable {
            try {
                work();
            } catch (...) {
                if (onFailure_) {
                    onFailure_(std::current_exception());
                }
            }
        });
    }

    std::optional<DiskCache> cache_;
    std::optional<asio::thread_pool> pool_;
    FailureSink onFailure_;
    std::mutex stopMutex_;
    bool stopped_{false};
};

}  // namespace ruvia::edge
