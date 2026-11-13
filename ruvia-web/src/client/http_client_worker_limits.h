#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ruvia/web/http_client_types.h"

#include "client/http_client_result_budget.h"

namespace ruvia::detail {

class http_client_pool;

// Worker-affine bounds shared by every HTTP client alias of one owner (a
// standalone http_client, or one application worker): idle connections across
// aliases, ordered by one idle clock, and concurrent background DNS refreshes.
// Pools join with their first connection and leave before retiring them.
class http_client_worker_limits final {
public:
    explicit http_client_worker_limits(const http_client_worker_config& config);
    http_client_worker_limits(const http_client_worker_limits&) = delete;
    http_client_worker_limits& operator=(const http_client_worker_limits&) = delete;

    void join(http_client_pool& pool);
    void leave(http_client_pool& pool) noexcept;
    [[nodiscard]] std::uint64_t next_idle_order() noexcept {
        return ++idle_clock_;
    }
    // Closes the least recently used idle connections of any alias while the
    // worker keeps more than its bound; alias bounds apply first.
    void enforce_idle_limits() noexcept;
    // A background refresh lookup may start: false once the worker runs its
    // bound of them, and the caller defers to its next refresh period.
    [[nodiscard]] bool try_begin_dns_refresh() noexcept;
    void end_dns_refresh() noexcept;

private:
    void enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept;

    std::size_t max_idle_http1_;
    std::size_t max_idle_multiplexed_;
    std::size_t max_dns_refreshes_;
    std::size_t dns_refreshes_{0};
    std::uint64_t idle_clock_{0};
    std::vector<http_client_pool*> pools_{};
};

// The per-worker state every pool of one owner shares.
struct http_client_worker_services final {
    std::shared_ptr<http_client_result_budget_domain> result_budget_;
    std::shared_ptr<http_client_worker_limits> limits_;
};

// Validates `config`; throws std::invalid_argument.
[[nodiscard]] http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config);

}  // namespace ruvia::detail
