#pragma once

#include <asio/io_context.hpp>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "ruvia/web/detail/client/HttpClientBackend.h"

namespace ruvia::detail {

class HttpClientRegistry final {
public:
    HttpClientRegistry(
        asio::io_context& ioContext,
        std::pmr::memory_resource* resource,
        std::span<const HttpClientDefinition> clients);
    ~HttpClientRegistry();

    HttpClientRegistry(const HttpClientRegistry&) = delete;
    HttpClientRegistry& operator=(const HttpClientRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    [[nodiscard]] HttpClientBackend* get(std::string_view alias = kDefaultHttpClientAlias) const;
    void scanDeadlines() noexcept;

    // The worker io_context this registry runs on; used to spawn detached background work
    // (Context::defer) on the same single-threaded executor as request handling.
    [[nodiscard]] asio::io_context& ioContext() const noexcept { return ioContext_; }

    // Runtime add/remove. MUST be called on this registry's io_context thread (the App posts them
    // there). addClient replaces any existing client of the same alias. removeClient closeNow()s
    // the backend and defers its destruction until it is quiescent (reaped by scanDeadlines).
    void addClient(std::string_view alias, const HttpClientConfig& config);
    bool removeClient(std::string_view alias);

    // Get-or-create a client for an ad-hoc origin config (Context::client().proxy to an arbitrary upstream),
    // pooled and reused across requests to the same origin. Bounded: the least-recently-used ad-hoc
    // client is retired when the cap is reached. Never returns nullptr. Worker thread only.
    [[nodiscard]] HttpClientBackend* getOrCreate(const HttpClientConfig& config);

private:
    struct Entry final {
        std::pmr::string alias;
        HttpClientBackendPtr backend;
    };
    struct AdHocEntry final {
        std::pmr::string key;
        std::uint64_t lastUsed;
        HttpClientBackendPtr backend;
    };

    void rebuildDefaultBackend() noexcept;
    void reapRetired() noexcept;

    static constexpr std::size_t kAdHocClientCap = 256;

    asio::io_context& ioContext_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> pools_;
    std::pmr::vector<AdHocEntry> adHoc_;              // origin-keyed ad-hoc clients (LRU-bounded)
    std::pmr::vector<HttpClientBackendPtr> retired_;  // removed, awaiting quiescent destruction
    std::uint64_t adHocClock_{0};                     // monotonic tick for LRU ordering
    HttpClientBackend* defaultBackend_{nullptr};
};

}  // namespace ruvia::detail
