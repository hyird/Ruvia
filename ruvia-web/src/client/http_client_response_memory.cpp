#include "client/http_client_response_memory.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/web/http_client.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "client/http_client_worker_limits.h"

namespace ruvia::detail {

void http_client_response_memory_domain::charge_in_flight(std::size_t bytes_value) {
    if (bytes_value != 0 && !result_budget_domain_->reserve_in_flight(bytes_value)) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded),
            "HTTP client in-flight response memory budget is exhausted");
    }
}

void http_client_response_memory_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value != 0) {
        result_budget_domain_->release_in_flight(bytes_value);
    }
}

http_client_response_memory_domain::http_client_response_memory_domain(const worker_ref& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain)
    : worker_(worker_value),
      result_budget_domain_(result_budget_domain) {
    if (!worker_.valid()) {
        throw std::invalid_argument("HTTP client response memory requires a worker");
    }
}

http_client_response_memory_domain::~http_client_response_memory_domain() {
    if (phase_ != phase_type::prepared) {
        require_current();
    }
    if (state_head_ != nullptr || references_ != 0) {
        std::terminate();
    }
}

http_client_response_memory_domain::owner http_client_response_memory_domain::create(
    const worker_ref& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    return owner(new http_client_response_memory_domain(worker_value, result_budget_domain), deleter_type{});
}

void http_client_response_memory_domain::destroy() noexcept {
    delete this;
}

void http_client_response_memory_domain::deleter_type::operator()(
    http_client_response_memory_domain* domain) const noexcept {
    if (domain != nullptr) {
        domain->release_root();
    }
}

void http_client_response_memory_domain::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}

void http_client_response_memory_domain::retain() noexcept {
    require_current();
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void http_client_response_memory_domain::release() noexcept {
    require_current();
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        destroy();
    }
}

void http_client_response_memory_domain::release_root() noexcept {
    // Startup rollback has never published worker-affine response storage.
    // No worker scheduling is necessary (or possible after failed preparation).
    if (phase_ == phase_type::prepared) {
        if (references_ != 1 || state_head_ != nullptr) {
            std::terminate();
        }
        references_ = 0;
        destroy();
        return;
    }
    // Active storage is retired explicitly by the pool on its owning worker;
    // do not conceal a missing join with unstructured deferred destruction.
    release();
}

http_client_response_state* http_client_response_memory_domain::create_state(http_client_pool& pool) {
    require_current();
    if (phase_ == phase_type::retired) {
        throw std::logic_error("HTTP client response transport is retired");
    }
    phase_ = phase_type::active;
    charge_in_flight(sizeof(http_client_response_state));
    http_client_response_state* state_value = nullptr;
    try {
        state_value = new http_client_response_state(*this);
    } catch (...) {
        release_in_flight(sizeof(http_client_response_state));
        throw;
    }
    state_value->pool_ = &pool;
    attach_state(*state_value);
    return state_value;
}

void http_client_response_memory_domain::attach_state(http_client_response_state& state_value) noexcept {
    require_current();
    state_value.next_memory_state_ = state_head_;
    if (state_head_ != nullptr) {
        state_head_->previous_memory_state_ = &state_value;
    }
    state_head_ = &state_value;
    retain();
}

void http_client_response_memory_domain::destroy_state(http_client_response_state* state_value) noexcept {
    require_current();
    if (state_value == nullptr || &state_value->memory_domain_ != this) {
        std::terminate();
    }
    if (state_value->pool_ != nullptr) {
        unlink_state(*state_value);
    } else if (state_value->previous_memory_state_ != nullptr || state_value->next_memory_state_ != nullptr) {
        std::terminate();
    }
    delete state_value;
    release_in_flight(sizeof(http_client_response_state));
    release();
}

void http_client_response_memory_domain::unlink_state(http_client_response_state& state_value) noexcept {
    if (state_value.previous_memory_state_ != nullptr) {
        state_value.previous_memory_state_->next_memory_state_ = state_value.next_memory_state_;
    } else if (state_head_ == &state_value) {
        state_head_ = state_value.next_memory_state_;
    } else {
        std::terminate();
    }
    if (state_value.next_memory_state_ != nullptr) {
        state_value.next_memory_state_->previous_memory_state_ = state_value.previous_memory_state_;
    }
    state_value.previous_memory_state_ = nullptr;
    state_value.next_memory_state_ = nullptr;
}

void http_client_response_memory_domain::detach_transport_bindings(http_client_pool& pool) noexcept {
    require_current();
    phase_ = phase_type::retired;
    while (state_head_ != nullptr) {
        auto* state_value = state_head_;
        if (state_value->pool_ != &pool) {
            std::terminate();
        }
        // Revoke registration before notifying consumers. A continuation may
        // release this state or a sibling; never cache a next pointer across it.
        unlink_state(*state_value);
        state_value->detach_transport_bindings();
    }
}

http_client_response_state::http_client_response_state(http_client_response_memory_domain& memory_domain)
    : head_signal_(memory_domain.worker()),
      data_signal_(memory_domain.worker()),
      space_signal_(memory_domain.worker()),
      memory_domain_(memory_domain) {
    body_operation_scope_.bind_worker(memory_domain.worker());
}

void http_client_response_state::require_owner_worker(const char* message) const {
    if (!memory_domain_.worker().is_current()) {
        throw std::logic_error(message);
    }
}

void http_client_response_state::detach_transport_bindings() noexcept {
    if (tunnel_) {
        tunnel_->output_.stop();
    }
    if (upload_) {
        upload_->output_.stop();
    }
#ifdef RUVIA_ENABLE_HTTP2
    http2_data_credit_.reset();
#endif
#ifdef RUVIA_ENABLE_HTTP3
    http3_data_credit_.reset();
#endif
    connection_index_ = 0;
    connection_serial_ = 0;
    request_id_ = 0;
    cancellation_id_ = 0;
    stream_id_ = 0;
    pool_ = nullptr;
    transport_ = http_client_response_transport::unassigned;
    if (!complete_) {
        pool_closed_ = true;
        complete_ = true;
    }
    head_signal_.notify();
    data_signal_.notify();
    space_signal_.notify();
}

}  // namespace ruvia::detail

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

namespace ruvia::detail {

http_client_worker_limits::http_client_worker_limits(const http_client_worker_config& config)
    : max_idle_http1_(config.max_idle_http1_),
      max_idle_multiplexed_(config.max_idle_multiplexed_),
      max_dns_refreshes_(config.max_concurrent_dns_refreshes_) {
    if (max_dns_refreshes_ == 0) {
        throw std::invalid_argument("HTTP client DNS refresh concurrency must be greater than zero");
    }
}

void http_client_worker_limits::join(http_client_pool& pool) {
    if (std::ranges::find(pools_, &pool) == pools_.end()) {
        pools_.push_back(&pool);
    }
}

void http_client_worker_limits::leave(http_client_pool& pool) noexcept {
    std::erase(pools_, &pool);
}

void http_client_worker_limits::enforce_idle_limits() noexcept {
    enforce_idle_limit(false, max_idle_http1_);
    enforce_idle_limit(true, max_idle_multiplexed_);
}

void http_client_worker_limits::enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept {
    const auto total = [this, multiplexed] {
        std::size_t count = 0;
        for (const auto* pool : pools_) {
            count += pool->idle_count(multiplexed);
        }
        return count;
    };
    if (total() <= maximum) {
        return;
    }
    // Pool counters are upper bounds; recount before closing anything.
    for (auto* pool : pools_) {
        pool->recount_idle();
    }
    for (auto remaining = total(); remaining > maximum; --remaining) {
        http_client_pool* oldest = nullptr;
        std::optional<std::uint64_t> oldest_order;
        for (auto* pool : pools_) {
            const auto order = pool->least_recent_idle(multiplexed);
            if (order && (!oldest_order || *order < *oldest_order)) {
                oldest = pool;
                oldest_order = order;
            }
        }
        if (oldest == nullptr || !oldest->close_least_recent_idle(multiplexed)) {
            return;
        }
    }
}

bool http_client_worker_limits::try_begin_dns_refresh() noexcept {
    if (dns_refreshes_ == max_dns_refreshes_) {
        return false;
    }
    ++dns_refreshes_;
    return true;
}

void http_client_worker_limits::end_dns_refresh() noexcept {
    --dns_refreshes_;
}

http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config) {
    return {.result_budget_ = std::make_shared<http_client_result_budget_domain>(config.result_budget_),
        .limits_ = std::make_shared<http_client_worker_limits>(config)};
}

}  // namespace ruvia::detail
