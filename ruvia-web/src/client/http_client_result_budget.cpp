#include "client/http_client_result_budget.h"

#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/web/http_client.h"

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
        if (bytes_value > max_retained_bytes_ - retained) {
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

http_client_result_budget_lease::http_client_result_budget_lease(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept
    : domain_(domain),
      bytes_(bytes_value) {}

http_client_result_budget_lease::http_client_result_budget_lease(
    http_client_result_budget_lease&& other) noexcept
    : domain_(std::move(other.domain_)),
      bytes_(std::exchange(other.bytes_, 0)) {}

http_client_result_budget_lease::~http_client_result_budget_lease() {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
    }
}

std::optional<http_client_result_budget_lease> http_client_result_budget_lease::try_acquire(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept {
    if (!domain->try_reserve(bytes_value)) {
        return std::nullopt;
    }
    return http_client_result_budget_lease(domain, bytes_value);
}

}  // namespace ruvia::detail

namespace ruvia {

http_client_response_bytes::http_client_response_bytes(
    std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value)
    : data_(reserve_bytes == 0 ? nullptr : std::make_unique_for_overwrite<std::byte[]>(reserve_bytes)) {
    auto [domain, reserved] = std::move(lease_value).release();
    budget_domain_ = std::move(domain);
    reserved_ = reserved;
}

http_client_response_bytes::http_client_response_bytes(http_client_response_bytes&& other) noexcept
    : budget_domain_(std::move(other.budget_domain_)),
      data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      reserved_(std::exchange(other.reserved_, 0)) {}

http_client_response_bytes& http_client_response_bytes::operator=(
    http_client_response_bytes&& other) noexcept {
    if (this != &other) {
        release();
        budget_domain_ = std::move(other.budget_domain_);
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
        reserved_ = std::exchange(other.reserved_, 0);
    }
    return *this;
}

http_client_response_bytes::~http_client_response_bytes() {
    release();
}

void http_client_response_bytes::release() noexcept {
    // Free the owned byte buffer before making its reservation available to another result.
    data_.reset();
    if (budget_domain_ != nullptr) {
        budget_domain_->release(reserved_);
        budget_domain_.reset();
    }
    size_ = 0;
    reserved_ = 0;
}

void http_client_response_bytes::append(std::span<const std::byte> bytes_value) noexcept {
    if (!bytes_value.empty()) {
        std::memcpy(data_.get() + size_, bytes_value.data(), bytes_value.size());
        size_ += bytes_value.size();
    }
}

}  // namespace ruvia
