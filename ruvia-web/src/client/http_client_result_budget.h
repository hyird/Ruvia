#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include "ruvia/web/http_client_types.h"

namespace ruvia::detail {

class http_client_result_budget_domain final {
public:
    explicit http_client_result_budget_domain(http_client_result_budget_config config);

    http_client_result_budget_domain(const http_client_result_budget_domain&) = delete;
    http_client_result_budget_domain& operator=(const http_client_result_budget_domain&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

    // Only the owning worker accesses receive accounting. Escaped results
    // independently use the thread-safe retained-result counter above.
    [[nodiscard]] bool reserve_in_flight(std::size_t bytes) noexcept;
    void release_in_flight(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t in_flight_bytes() const noexcept {
        return in_flight_bytes_;
    }

private:
    const std::size_t max_retained_bytes_;
    std::atomic<std::size_t> retained_bytes_{0};
    const std::size_t max_in_flight_bytes_;
    std::size_t in_flight_bytes_{0};
};

// Move-only reservation carried by an escaped result. It owns the budget domain
// independently of the client and releases its exact charge at destruction.
class http_client_result_budget_lease final {
public:
    http_client_result_budget_lease() noexcept = default;
    http_client_result_budget_lease(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease& operator=(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept;
    http_client_result_budget_lease& operator=(http_client_result_budget_lease&& other) noexcept;
    ~http_client_result_budget_lease();

    [[nodiscard]] static std::optional<http_client_result_budget_lease> try_acquire(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes) noexcept;

private:
    http_client_result_budget_lease(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept;
    void reset() noexcept;

    // Keep the domain alive until after releasing the reservation.
    std::shared_ptr<http_client_result_budget_domain> domain_;
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
