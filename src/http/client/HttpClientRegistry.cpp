#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientInternal.h"
#include "HttpClientPool.h"

#include <utility>

namespace ruvia::detail {

HttpClientRegistry::HttpClientRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const HttpClientDefinition> clients)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      pools_(resource_) {
    pools_.reserve(clients.size());
    for (const auto& def : clients) {
        auto pool = makePmrObject<HttpClientPool>(resource_, ioContext, def.config, resource_);
        auto* poolRaw = pool.get();
        pools_.push_back(Entry{
            std::pmr::string(def.alias, resource_),
            std::move(pool)});
        if (std::string_view(def.alias) == kDefaultHttpClientAlias && defaultPool_ == nullptr) {
            defaultPool_ = poolRaw;
        }
    }
    if (defaultPool_ == nullptr && !pools_.empty()) {
        defaultPool_ = pools_.front().pool.get();
    }
}

HttpClientRegistry::~HttpClientRegistry() = default;

Task<void> HttpClientRegistry::connect() {
    for (auto& entry : pools_) {
        co_await entry.pool->connect();
    }
}

void HttpClientRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry.pool->closeNow();
    }
}

bool HttpClientRegistry::empty() const noexcept {
    return pools_.empty();
}

bool HttpClientRegistry::hasAnyTimeout() const noexcept {
    for (const auto& entry : pools_) {
        if (entry.pool->hasAnyTimeout()) return true;
    }
    return false;
}

HttpClientPool* HttpClientRegistry::get(std::string_view alias) const {
    if (alias == kDefaultHttpClientAlias && defaultPool_ != nullptr) {
        return defaultPool_;
    }
    for (const auto& entry : pools_) {
        if (std::string_view(entry.alias) == alias) {
            return entry.pool.get();
        }
    }
    return nullptr;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
