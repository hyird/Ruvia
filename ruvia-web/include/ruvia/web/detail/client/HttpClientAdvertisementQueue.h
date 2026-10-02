#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/web/HttpClientAdvertisement.h"
#include "ruvia/web/HttpClientAdvertisementConfig.h"

namespace ruvia::detail {
class HttpClientAdvertisementMemory;

// The pool owns this bounded queue. Returned owners retain their separate
// worker memory domain, not the client pool or its transport lifecycle.
class HttpClientAdvertisementQueue final {
public:
    HttpClientAdvertisementQueue(const WorkerHandle& worker, HttpClientAdvertisementConfig config,
        std::pmr::memory_resource* resource);
    // An explicit accounting upstream must outlive every returned observation.
    HttpClientAdvertisementQueue(const WorkerHandle& worker, HttpClientAdvertisementConfig config,
        std::pmr::memory_resource* resource, std::pmr::memory_resource& memoryUpstream);
    ~HttpClientAdvertisementQueue();
    HttpClientAdvertisementQueue(const HttpClientAdvertisementQueue&) = delete;
    HttpClientAdvertisementQueue& operator=(const HttpClientAdvertisementQueue&) = delete;

    [[nodiscard]] bool retain(std::size_t slot, HttpProtocolVersion protocol, const HttpOriginAdvertisement& origins);
    [[nodiscard]] bool retain(std::size_t slot, const HttpAlternativeServiceAdvertisement& service);
    [[nodiscard]] std::optional<HttpClientAdvertisement> next();
    void clear() noexcept;
    void retire() noexcept;
    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_;
    }
    [[nodiscard]] std::size_t retainedBytes() const noexcept;

private:
    void requireCurrent() const noexcept;
    [[nodiscard]] bool admit(std::size_t bytes) noexcept;
    const WorkerHandle& worker_;
    HttpClientAdvertisementConfig config_;
    HttpClientAdvertisementMemory* memory_{};
    std::pmr::list<HttpClientAdvertisement> pending_;
    std::size_t dropped_{};
};
}  // namespace ruvia::detail
