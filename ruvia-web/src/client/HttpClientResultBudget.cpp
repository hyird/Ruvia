#include "ruvia/web/detail/client/HttpClientResultBudget.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

HttpClientResultBudgetDomain::HttpClientResultBudgetDomain(HttpClientResultBudgetConfig config)
    : maxRetainedBytes_(config.maxRetainedBytes),
      max_in_flight_bytes_(config.max_in_flight_bytes) {
    if (maxRetainedBytes_ == 0 || max_in_flight_bytes_ == 0) {
        throw std::invalid_argument("HTTP client result byte budget must be greater than zero");
    }
}

bool HttpClientResultBudgetDomain::reserve_in_flight(std::size_t bytes) noexcept {
    if (bytes > max_in_flight_bytes_ - in_flight_bytes_) {
        return false;
    }
    in_flight_bytes_ += bytes;
    return true;
}

void HttpClientResultBudgetDomain::release_in_flight(std::size_t bytes) noexcept {
    if (bytes > in_flight_bytes_) {
        std::terminate();
    }
    in_flight_bytes_ -= bytes;
}

bool HttpClientResultBudgetDomain::tryReserve(std::size_t bytes) noexcept {
    auto retained = retainedBytes_.load(std::memory_order_relaxed);
    for (;;) {
        if (retained > maxRetainedBytes_ || bytes > maxRetainedBytes_ - retained) {
            return false;
        }
        if (retainedBytes_.compare_exchange_weak(
                retained, retained + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void HttpClientResultBudgetDomain::release(std::size_t bytes) noexcept {
    const auto previous = retainedBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) {
        std::terminate();
    }
}

std::size_t HttpClientResultBudgetDomain::retainedBytes() const noexcept {
    return retainedBytes_.load(std::memory_order_acquire);
}

HttpClientResultBudgetLease::HttpClientResultBudgetLease(
    const std::shared_ptr<HttpClientResultBudgetDomain>& domain, std::size_t bytes) noexcept
    : domain_(domain),
      bytes_(bytes) {}

HttpClientResultBudgetLease::HttpClientResultBudgetLease(
    HttpClientResultBudgetLease&& other) noexcept
    : domain_(std::move(other.domain_)),
      bytes_(std::exchange(other.bytes_, 0)) {}

HttpClientResultBudgetLease& HttpClientResultBudgetLease::operator=(
    HttpClientResultBudgetLease&& other) noexcept {
    if (this != &other) {
        reset();
        domain_ = std::move(other.domain_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

HttpClientResultBudgetLease::~HttpClientResultBudgetLease() {
    reset();
}

std::optional<HttpClientResultBudgetLease> HttpClientResultBudgetLease::tryAcquire(
    const std::shared_ptr<HttpClientResultBudgetDomain>& domain, std::size_t bytes) noexcept {
    if (domain == nullptr || !domain->tryReserve(bytes)) {
        return std::nullopt;
    }
    return HttpClientResultBudgetLease(domain, bytes);
}

void HttpClientResultBudgetLease::reset() noexcept {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
        bytes_ = 0;
        domain_.reset();
    }
}

}  // namespace ruvia::detail
