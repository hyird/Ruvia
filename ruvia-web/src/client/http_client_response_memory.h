#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "ruvia/core/worker_ref.h"

namespace ruvia::detail {

class http_client_pool;
class http_client_response_state;
class http_client_result_budget_domain;

// Response storage and in-flight budget accounting of one client pool, plus the
// registry of live response states so the pool can retire their transport
// borrows on join. Worker-affine, reference counted by the pool root and every
// live state.
class http_client_response_memory_domain final {
public:
    class deleter_type final {
    public:
        void operator()(http_client_response_memory_domain* domain) const noexcept;
    };

    using owner = std::unique_ptr<http_client_response_memory_domain, deleter_type>;

    [[nodiscard]] static owner create(const worker_ref& worker,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);

    http_client_response_memory_domain(const http_client_response_memory_domain&) = delete;
    http_client_response_memory_domain& operator=(const http_client_response_memory_domain&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }

    void charge_in_flight(std::size_t bytes_value);
    void release_in_flight(std::size_t bytes_value) noexcept;

    [[nodiscard]] http_client_response_state* create_state(http_client_pool& pool);
    void destroy_state(http_client_response_state* state) noexcept;
    void detach_transport_bindings(http_client_pool& pool) noexcept;

private:
    enum class phase_type : unsigned char { prepared,
        active,
        retired };

    http_client_response_memory_domain(const worker_ref& worker_value,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    ~http_client_response_memory_domain();

    void destroy() noexcept;
    void require_current() const noexcept;
    void attach_state(http_client_response_state& state) noexcept;
    void unlink_state(http_client_response_state& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void release_root() noexcept;

    worker_ref worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_state* state_head_{};
    std::size_t references_{1};
    phase_type phase_{phase_type::prepared};

    friend class deleter_type;
    friend class http_client_response_state;
};

class http_client_in_flight_reservation final {
public:
    constexpr http_client_in_flight_reservation() noexcept = default;
    explicit constexpr http_client_in_flight_reservation(http_client_response_memory_domain* domain) noexcept
        : domain_(domain) {}
    ~http_client_in_flight_reservation() {
        clear();
    }
    http_client_in_flight_reservation(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation& operator=(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation(http_client_in_flight_reservation&& other) noexcept
        : domain_(std::exchange(other.domain_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0)) {}
    http_client_in_flight_reservation& operator=(http_client_in_flight_reservation&& other) noexcept {
        if (this != &other) {
            clear();
            domain_ = std::exchange(other.domain_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    void resize_to(std::size_t target_bytes) {
        if (domain_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            domain_->charge_in_flight(target_bytes - bytes_);
        } else {
            domain_->release_in_flight(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (domain_ != nullptr && target_bytes < bytes_) {
            domain_->release_in_flight(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (domain_ != nullptr && bytes_ != 0) {
            domain_->release_in_flight(bytes_);
            bytes_ = 0;
        }
    }

    void swap(http_client_in_flight_reservation& other) noexcept {
        std::swap(domain_, other.domain_);
        std::swap(bytes_, other.bytes_);
    }

private:
    http_client_response_memory_domain* domain_{nullptr};
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
