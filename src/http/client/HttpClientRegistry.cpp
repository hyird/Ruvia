#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientInternal.h"
#include "HttpClientPool.h"
#include "Http2ClientSession.h"
#include "ruvia/memory/PmrResource.h"

#include <chrono>
#include <utility>

namespace ruvia::detail {

namespace {

// Build the transport for one origin: an HTTP/2 multiplexed session when the config opts in,
// otherwise the HTTP/1.1 connection pool. Ownership is a base pointer whose deleter calls the
// concrete type's destroy() so the right size returns to the PMR resource.
HttpClientBackendPtr makeHttpClientBackend(
    asio::io_context& ioContext,
    const HttpClientConfig& config,
    std::pmr::memory_resource* resource) {
    if (config.http2) {
        return HttpClientBackendPtr(
            constructPmrObject<Http2ClientSession>(resource, ioContext, config, resource));
    }
    return HttpClientBackendPtr(
        constructPmrObject<HttpClientPool>(resource, ioContext, config, resource));
}

}  // namespace

HttpClientRegistry::HttpClientRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const HttpClientDefinition> clients)
    : resource_(pmrResourceOrDefault(resource)),
      pools_(resource_) {
    pools_.reserve(clients.size());
    for (const auto& def : clients) {
        auto backend = makeHttpClientBackend(ioContext, def.config, resource_);
        auto* backendRaw = backend.get();
        pools_.push_back(Entry{
            std::pmr::string(def.alias, resource_),
            std::move(backend)});
        if (std::string_view(def.alias) == kDefaultHttpClientAlias && defaultBackend_ == nullptr) {
            defaultBackend_ = backendRaw;
        }
    }
    if (defaultBackend_ == nullptr && !pools_.empty()) {
        defaultBackend_ = pools_.front().backend.get();
    }
}

HttpClientRegistry::~HttpClientRegistry() = default;

Task<void> HttpClientRegistry::connect() {
    for (auto& entry : pools_) {
        co_await entry.backend->connect();
    }
}

void HttpClientRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry.backend->closeNow();
    }
}

bool HttpClientRegistry::empty() const noexcept {
    return pools_.empty();
}

bool HttpClientRegistry::hasAnyTimeout() const noexcept {
    for (const auto& entry : pools_) {
        if (entry.backend->hasAnyTimeout()) return true;
    }
    return false;
}

void HttpClientRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : pools_) {
        entry.backend->scanDeadlines(now);
    }
}

HttpClientBackend* HttpClientRegistry::get(std::string_view alias) const {
    if (alias == kDefaultHttpClientAlias && defaultBackend_ != nullptr) {
        return defaultBackend_;
    }
    for (const auto& entry : pools_) {
        if (std::string_view(entry.alias) == alias) {
            return entry.backend.get();
        }
    }
    return nullptr;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
