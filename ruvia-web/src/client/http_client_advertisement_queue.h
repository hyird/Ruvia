#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/worker_handle.h"
#include "ruvia/web/http_client_advertisement.h"
#include "ruvia/web/http_client_advertisement_config.h"

namespace ruvia::detail {
class http_client_advertisement_memory;

// The pool owns this bounded queue. Returned owners retain their separate
// worker memory domain, not the client pool or its transport lifecycle.
class http_client_advertisement_queue final {
public:
    http_client_advertisement_queue(const worker_handle& worker_value, http_client_advertisement_config config,
        std::pmr::memory_resource* resource);
    http_client_advertisement_queue(worker_handle&&, http_client_advertisement_config,
        std::pmr::memory_resource*) = delete;
    // An explicit accounting upstream must outlive every returned observation.
    http_client_advertisement_queue(const worker_handle& worker_value, http_client_advertisement_config config,
        std::pmr::memory_resource* resource, std::pmr::memory_resource& memory_upstream);
    http_client_advertisement_queue(worker_handle&&, http_client_advertisement_config,
        std::pmr::memory_resource*, std::pmr::memory_resource&) = delete;
    ~http_client_advertisement_queue();
    http_client_advertisement_queue(const http_client_advertisement_queue&) = delete;
    http_client_advertisement_queue& operator=(const http_client_advertisement_queue&) = delete;

    [[nodiscard]] bool retain(std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins);
    [[nodiscard]] bool retain(std::size_t slot, const http_alternative_service_advertisement& service);
    [[nodiscard]] std::optional<http_client_advertisement> next();
    void clear() noexcept;
    void retire() noexcept;
    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_;
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

private:
    void require_current() const noexcept;
    [[nodiscard]] bool admit(std::size_t bytes) noexcept;
    const worker_handle& worker_;
    http_client_advertisement_config config_;
    http_client_advertisement_memory* memory_{};
    std::pmr::list<http_client_advertisement> pending_;
    std::size_t dropped_{};
};
}  // namespace ruvia::detail
