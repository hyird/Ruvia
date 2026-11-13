#include "client/http_client_worker_limits.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

#include "client/http_client_pool.h"

namespace ruvia::detail {

http_client_worker_limits::http_client_worker_limits(const http_client_worker_config& config)
    : max_idle_http1_(config.max_idle_http1_),
      max_idle_multiplexed_(config.max_idle_multiplexed_),
      max_dns_refreshes_(config.max_concurrent_dns_refreshes_) {
    if (max_dns_refreshes_ == 0) {
        throw std::invalid_argument("HTTP client DNS refresh concurrency must be greater than zero");
    }
}

void http_client_worker_limits::join(http_client_pool& pool) {
    if (std::ranges::find(pools_, &pool) == pools_.end()) {
        pools_.push_back(&pool);
    }
}

void http_client_worker_limits::leave(http_client_pool& pool) noexcept {
    std::erase(pools_, &pool);
}

void http_client_worker_limits::enforce_idle_limits() noexcept {
    enforce_idle_limit(false, max_idle_http1_);
    enforce_idle_limit(true, max_idle_multiplexed_);
}

void http_client_worker_limits::enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept {
    const auto total = [this, multiplexed] {
        std::size_t count = 0;
        for (const auto* pool : pools_) {
            count += pool->idle_count(multiplexed);
        }
        return count;
    };
    if (total() <= maximum) {
        return;
    }
    // Pool counters are upper bounds; recount before closing anything.
    for (auto* pool : pools_) {
        pool->recount_idle();
    }
    for (auto remaining = total(); remaining > maximum; --remaining) {
        http_client_pool* oldest = nullptr;
        std::optional<std::uint64_t> oldest_order;
        for (auto* pool : pools_) {
            const auto order = pool->least_recent_idle(multiplexed);
            if (order && (!oldest_order || *order < *oldest_order)) {
                oldest = pool;
                oldest_order = order;
            }
        }
        if (oldest == nullptr || !oldest->close_least_recent_idle(multiplexed)) {
            return;
        }
    }
}

bool http_client_worker_limits::try_begin_dns_refresh() noexcept {
    if (dns_refreshes_ == max_dns_refreshes_) {
        return false;
    }
    ++dns_refreshes_;
    return true;
}

void http_client_worker_limits::end_dns_refresh() noexcept {
    --dns_refreshes_;
}

http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config) {
    return {.result_budget_ = std::make_shared<http_client_result_budget_domain>(config.result_budget_),
        .limits_ = std::make_shared<http_client_worker_limits>(config)};
}

}  // namespace ruvia::detail
