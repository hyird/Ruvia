#include "client/http_client_result_budget.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

http_client_result_budget_domain::http_client_result_budget_domain(http_client_result_budget_config config)
    : max_retained_bytes_(config.max_retained_bytes_),
      max_in_flight_bytes_(config.max_in_flight_bytes_) {
    if (max_retained_bytes_ == 0 || max_in_flight_bytes_ == 0) {
        throw std::invalid_argument("HTTP client result byte budget must be greater than zero");
    }
}

bool http_client_result_budget_domain::reserve_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > max_in_flight_bytes_ - in_flight_bytes_) {
        return false;
    }
    in_flight_bytes_ += bytes_value;
    return true;
}

void http_client_result_budget_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > in_flight_bytes_) {
        std::terminate();
    }
    in_flight_bytes_ -= bytes_value;
}

bool http_client_result_budget_domain::try_reserve(std::size_t bytes_value) noexcept {
    auto retained = retained_bytes_.load(std::memory_order_relaxed);
    for (;;) {
        if (retained > max_retained_bytes_ || bytes_value > max_retained_bytes_ - retained) {
            return false;
        }
        if (retained_bytes_.compare_exchange_weak(
                retained, retained + bytes_value, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void http_client_result_budget_domain::release(std::size_t bytes_value) noexcept {
    const auto previous = retained_bytes_.fetch_sub(bytes_value, std::memory_order_acq_rel);
    if (previous < bytes_value) {
        std::terminate();
    }
}

std::size_t http_client_result_budget_domain::retained_bytes() const noexcept {
    return retained_bytes_.load(std::memory_order_acquire);
}

http_client_result_budget_lease::http_client_result_budget_lease(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept
    : domain_(domain),
      bytes_(bytes_value) {}

http_client_result_budget_lease::http_client_result_budget_lease(
    http_client_result_budget_lease&& other) noexcept
    : domain_(std::move(other.domain_)),
      bytes_(std::exchange(other.bytes_, 0)) {}

http_client_result_budget_lease& http_client_result_budget_lease::operator=(
    http_client_result_budget_lease&& other) noexcept {
    if (this != &other) {
        reset();
        domain_ = std::move(other.domain_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

http_client_result_budget_lease::~http_client_result_budget_lease() {
    reset();
}

std::optional<http_client_result_budget_lease> http_client_result_budget_lease::try_acquire(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept {
    if (domain == nullptr || !domain->try_reserve(bytes_value)) {
        return std::nullopt;
    }
    return http_client_result_budget_lease(domain, bytes_value);
}

void http_client_result_budget_lease::reset() noexcept {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
        bytes_ = 0;
        domain_.reset();
    }
}

}  // namespace ruvia::detail
