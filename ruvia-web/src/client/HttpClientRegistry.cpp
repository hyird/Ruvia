
#include "HttpClientInternal.h"
#include "HttpClientPool.h"
#include "Http2ClientSession.h"
#include "ruvia/memory/PmrResource.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
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
            constructHttpPmrObject<Http2ClientSession>(resource, ioContext, config, resource));
    }
    return HttpClientBackendPtr(
        constructHttpPmrObject<HttpClientPool>(resource, ioContext, config, resource));
}

}  // namespace

HttpClientRegistry::HttpClientRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const HttpClientDefinition> clients)
    : ioContext_(ioContext),
      resource_(pmrResourceOrDefault(resource)),
      pools_(resource_),
      adHoc_(resource_),
      retired_(resource_) {
    pools_.reserve(clients.size());
    for (const auto& def : clients) {
        pools_.push_back(Entry{
            std::pmr::string(def.alias, resource_),
            makeHttpClientBackend(ioContext, def.config, resource_)});
    }
    rebuildDefaultBackend();
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
    for (auto& entry : adHoc_) {
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
    for (auto& entry : adHoc_) {
        entry.backend->scanDeadlines(now);
    }
    reapRetired();
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

void HttpClientRegistry::addClient(std::string_view alias, const HttpClientConfig& config) {
    auto backend = makeHttpClientBackend(ioContext_, config, resource_);
    for (auto& entry : pools_) {
        if (std::string_view(entry.alias) == alias) {
            // Replace an existing alias: retire the old backend, swap in the new one.
            entry.backend->closeNow();
            retired_.push_back(std::move(entry.backend));
            entry.backend = std::move(backend);
            rebuildDefaultBackend();
            return;
        }
    }
    pools_.push_back(Entry{std::pmr::string(alias, resource_), std::move(backend)});
    rebuildDefaultBackend();
}

bool HttpClientRegistry::removeClient(std::string_view alias) {
    for (auto it = pools_.begin(); it != pools_.end(); ++it) {
        if (std::string_view(it->alias) == alias) {
            it->backend->closeNow();
            retired_.push_back(std::move(it->backend));
            pools_.erase(it);
            rebuildDefaultBackend();
            return true;
        }
    }
    return false;
}

void HttpClientRegistry::rebuildDefaultBackend() noexcept {
    defaultBackend_ = nullptr;
    for (auto& entry : pools_) {
        if (std::string_view(entry.alias) == kDefaultHttpClientAlias) {
            defaultBackend_ = entry.backend.get();
            return;
        }
    }
    if (!pools_.empty()) {
        defaultBackend_ = pools_.front().backend.get();
    }
}

namespace {

// A canonical identity for an origin config: two configs sharing this key would open identical
// connections, so their ad-hoc client can be reused. Includes every connection-affecting field.
void appendKeyField(std::pmr::string& key, std::string_view value) {
    key.append(value.data(), value.size());
    key.push_back('\x1f');  // unit separator: unambiguous field boundary
}

[[nodiscard]] std::pmr::string makeAdHocKey(
    const HttpClientConfig& config, std::pmr::memory_resource* resource) {
    std::pmr::string key(resource);
    char portBuffer[6];
    const auto [ptr, ec] = std::to_chars(portBuffer, portBuffer + sizeof(portBuffer), config.port);
    appendKeyField(key, std::string_view(config.host));
    appendKeyField(key, ec == std::errc{} ? std::string_view(portBuffer, static_cast<std::size_t>(ptr - portBuffer)) : std::string_view());
    appendKeyField(key, config.tls ? "s" : "-");
    appendKeyField(key, config.http2 ? "2" : "1");
    appendKeyField(key, std::string_view(config.hostHeader));
    appendKeyField(key, std::string_view(config.tlsOptions.sniHost));
    appendKeyField(key, std::string_view(config.tlsOptions.caFile));
    appendKeyField(key, config.tlsOptions.insecureSkipVerify ? "i" : "-");
    appendKeyField(key, std::string_view(config.tlsOptions.certificateChainFile));
    appendKeyField(key, std::string_view(config.tlsOptions.privateKeyFile));
    return key;
}

}  // namespace

HttpClientBackend* HttpClientRegistry::getOrCreate(const HttpClientConfig& config) {
    auto key = makeAdHocKey(config, resource_);
    for (auto& entry : adHoc_) {
        if (entry.key == key) {
            entry.lastUsed = ++adHocClock_;
            return entry.backend.get();
        }
    }
    // At capacity: retire the least-recently-used ad-hoc client (deferred destroy).
    if (adHoc_.size() >= kAdHocClientCap) {
        auto lru = std::min_element(
            adHoc_.begin(), adHoc_.end(),
            [](const AdHocEntry& a, const AdHocEntry& b) { return a.lastUsed < b.lastUsed; });
        lru->backend->closeNow();
        retired_.push_back(std::move(lru->backend));
        adHoc_.erase(lru);
    }
    auto backend = makeHttpClientBackend(ioContext_, config, resource_);
    auto* backendRaw = backend.get();
    adHoc_.push_back(AdHocEntry{std::move(key), ++adHocClock_, std::move(backend)});
    return backendRaw;
}

void HttpClientRegistry::reapRetired() noexcept {
    // Destroy retired backends that have gone quiescent (closed + no in-flight / loops exited);
    // keep the rest for a later tick. Compact in place.
    std::size_t writeIdx = 0;
    for (std::size_t readIdx = 0; readIdx < retired_.size(); ++readIdx) {
        if (retired_[readIdx]->isQuiescent()) {
            retired_[readIdx].reset();  // HttpClientBackendDeleter -> backend->destroy()
        } else if (writeIdx != readIdx) {
            retired_[writeIdx++] = std::move(retired_[readIdx]);
        } else {
            ++writeIdx;
        }
    }
    retired_.resize(writeIdx);
}

}  // namespace ruvia::detail
