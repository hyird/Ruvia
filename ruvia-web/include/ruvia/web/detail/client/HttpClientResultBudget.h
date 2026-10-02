#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include "ruvia/web/HttpClientTypes.h"

namespace ruvia::detail {

class HttpClientResultBudgetDomain final {
public:
    explicit HttpClientResultBudgetDomain(HttpClientResultBudgetConfig config);

    HttpClientResultBudgetDomain(const HttpClientResultBudgetDomain&) = delete;
    HttpClientResultBudgetDomain& operator=(const HttpClientResultBudgetDomain&) = delete;

    [[nodiscard]] bool tryReserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t retainedBytes() const noexcept;

    // Only the owning worker accesses receive accounting. Escaped results
    // independently use the thread-safe retained-result counter above.
    [[nodiscard]] bool reserve_in_flight(std::size_t bytes) noexcept;
    void release_in_flight(std::size_t bytes) noexcept;
    [[nodiscard]] std::size_t in_flight_bytes() const noexcept {
        return in_flight_bytes_;
    }

private:
    const std::size_t maxRetainedBytes_;
    std::atomic<std::size_t> retainedBytes_{0};
    const std::size_t max_in_flight_bytes_;
    std::size_t in_flight_bytes_{0};
};

// Move-only reservation carried by an escaped result. It owns the budget domain
// independently of the client and releases its exact charge at destruction.
class HttpClientResultBudgetLease final {
public:
    HttpClientResultBudgetLease() noexcept = default;
    HttpClientResultBudgetLease(const HttpClientResultBudgetLease&) = delete;
    HttpClientResultBudgetLease& operator=(const HttpClientResultBudgetLease&) = delete;
    HttpClientResultBudgetLease(HttpClientResultBudgetLease&& other) noexcept;
    HttpClientResultBudgetLease& operator=(HttpClientResultBudgetLease&& other) noexcept;
    ~HttpClientResultBudgetLease();

    [[nodiscard]] static std::optional<HttpClientResultBudgetLease> tryAcquire(
        const std::shared_ptr<HttpClientResultBudgetDomain>& domain, std::size_t bytes) noexcept;

private:
    HttpClientResultBudgetLease(
        const std::shared_ptr<HttpClientResultBudgetDomain>& domain, std::size_t bytes) noexcept;
    void reset() noexcept;

    // Keep the domain alive until after releasing the reservation.
    std::shared_ptr<HttpClientResultBudgetDomain> domain_;
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
