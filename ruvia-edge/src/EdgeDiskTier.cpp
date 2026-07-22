#include "ruvia/edge/detail/EdgeDiskTier.h"

#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>

namespace ruvia::edge {

EdgeDiskTier::EdgeDiskTier(
    const std::optional<std::filesystem::path>& directory,
    std::size_t maxBytes) {
    if (!directory) {
        return;  // disabled: no cache, no thread
    }
    cache_.emplace(*directory, maxBytes);
    pool_.emplace(1);  // all disk I/O on one background thread, off the loop
}

EdgeDiskTier::~EdgeDiskTier() = default;

asio::awaitable<std::optional<CachedResponse>> EdgeDiskTier::lookup(std::string key) {
    if (!enabled()) {
        co_return std::nullopt;
    }
    // Run the blocking read on the disk pool; use_awaitable resumes this
    // coroutine back on the event loop with the result.
    co_return co_await asio::co_spawn(
        *pool_,
        [this, key = std::move(key)]()
            -> asio::awaitable<std::optional<CachedResponse>> {
            co_return cache_->lookup(key);
        },
        asio::use_awaitable);
}

void EdgeDiskTier::store(std::string key, CachedResponse entry) {
    if (!enabled()) {
        return;
    }
    asio::post(*pool_, [this, key = std::move(key), entry = std::move(entry)] {
        cache_->store(key, entry);
    });
}

void EdgeDiskTier::purge(std::string key) {
    if (!enabled()) {
        return;
    }
    asio::post(*pool_, [this, key = std::move(key)] { cache_->purge(key); });
}

void EdgeDiskTier::purgePrefix(std::string prefix) {
    if (!enabled()) {
        return;
    }
    asio::post(*pool_, [this, prefix = std::move(prefix)] {
        (void)cache_->purgePrefix(prefix);
    });
}

DiskCache::PurgeResult EdgeDiskTier::purgePrefixSync(std::string prefix) {
    if (!enabled()) {
        return {};
    }
    return runSync([this, prefix = std::move(prefix)] {
        return cache_->purgePrefix(prefix);
    });
}

bool EdgeDiskTier::clearSync() {
    if (!enabled()) {
        return true;
    }
    return runSync([this] { return cache_->clear(); });
}

void EdgeDiskTier::stop() noexcept {
    // Hold the stop lock while the queue drains, so a concurrent synchronous
    // purge/clear either posts before the join or observes the joined state.
    const std::lock_guard lock(stopMutex_);
    if (pool_ && !stopped_) {
        // join() without stop() drains outstanding work. Calling stop() first is
        // explicitly allowed to abandon queued persistence operations.
        pool_->join();
        stopped_ = true;
    }
}

}  // namespace ruvia::edge
