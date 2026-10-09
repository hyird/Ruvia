#pragma once

#include <cstddef>
#include <exception>
#include <limits>

namespace ruvia::detail {

// Worker-affine receive-storage accounting. The stable owner may be shared by
// successive HTTP/3 connections on the same worker; it is deliberately
// independent from the escaped, thread-safe http_client_result_budget_domain.
// Owners must outlive every lease and wake registration.
class http3_client_body_budget final {
public:
    using wake_function_type = void (*)(void*) noexcept;

    class lease_type final {
    public:
        lease_type() noexcept = default;
        lease_type(const lease_type&) = delete;
        lease_type& operator=(const lease_type&) = delete;
        lease_type(lease_type&& other) noexcept
            : budget_(other.budget_),
              bytes_(other.bytes_) {
            other.budget_ = nullptr;
            other.bytes_ = 0;
        }
        lease_type& operator=(lease_type&& other) noexcept {
            if (this != &other) {
                reset();
                budget_ = other.budget_;
                bytes_ = other.bytes_;
                other.budget_ = nullptr;
                other.bytes_ = 0;
            }
            return *this;
        }
        ~lease_type() {
            reset();
        }

        [[nodiscard]] bool attach(http3_client_body_budget& budget, std::size_t bytes_value) noexcept {
            if (budget_ != nullptr || !budget.try_retain(bytes_value)) {
                return false;
            }
            if (budget.active_leases_ == std::numeric_limits<std::size_t>::max()) {
                std::terminate();
            }
            ++budget.active_leases_;
            budget_ = &budget;
            bytes_ = bytes_value;
            return true;
        }
        void reset() noexcept {
            auto* const budget = budget_;
            if (budget == nullptr) {
                return;
            }
            budget_ = nullptr;
            const auto bytes_value = bytes_;
            bytes_ = 0;
            budget->release(bytes_value);
            if (budget->active_leases_ == 0) {
                std::terminate();
            }
            --budget->active_leases_;
        }
        [[nodiscard]] bool attached() const noexcept {
            return budget_ != nullptr;
        }
        [[nodiscard]] std::size_t retained_bytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] std::size_t available() const noexcept {
            return budget_ == nullptr ? std::numeric_limits<std::size_t>::max()
                                      : budget_->available();
        }
        void notify_producer() const noexcept {
            if (budget_ != nullptr) {
                budget_->notify_waiters();
            }
        }
        [[nodiscard]] bool try_retain(std::size_t bytes_value) noexcept {
            if (budget_ == nullptr || !budget_->try_retain(bytes_value)) {
                return false;
            }
            if (bytes_value > std::numeric_limits<std::size_t>::max() - bytes_) {
                std::terminate();
            }
            bytes_ += bytes_value;
            return true;
        }
        void release(std::size_t bytes_value) noexcept {
            if (budget_ == nullptr || bytes_value > bytes_) {
                std::terminate();
            }
            bytes_ -= bytes_value;
            budget_->release(bytes_value);
        }
        [[nodiscard]] bool try_replace(std::size_t bytes_value) noexcept {
            if (budget_ == nullptr || !budget_->try_replace(bytes_, bytes_value)) {
                return false;
            }
            bytes_ = bytes_value;
            return true;
        }

    private:
        http3_client_body_budget* budget_{};
        std::size_t bytes_{};
    };

    // Intrusive worker-local wake registration. A release synchronously visits
    // the registered connection drivers; callbacks must not mutate this list.
    class wake_registration_type final {
    public:
        wake_registration_type() noexcept = default;
        wake_registration_type(http3_client_body_budget& budget, wake_function_type callback_value,
            void* context_value) noexcept {
            budget.register_wake(*this, callback_value, context_value);
        }
        wake_registration_type(const wake_registration_type&) = delete;
        wake_registration_type& operator=(const wake_registration_type&) = delete;
        wake_registration_type(wake_registration_type&&) = delete;
        wake_registration_type& operator=(wake_registration_type&&) = delete;
        ~wake_registration_type() {
            reset();
        }

        void reset() noexcept {
            if (budget_ != nullptr) {
                budget_->unregister_wake(*this);
            }
        }

    private:
        http3_client_body_budget* budget_{};
        wake_registration_type* previous_{};
        wake_registration_type* next_{};
        wake_function_type callback_{};
        void* context_{};
        friend class http3_client_body_budget;
    };

    explicit http3_client_body_budget(std::size_t limit) noexcept
        : limit_(limit) {}
    ~http3_client_body_budget() {
        if (used_ != 0 || active_leases_ != 0 || wake_head_ != nullptr) {
            std::terminate();
        }
    }
    http3_client_body_budget(const http3_client_body_budget&) = delete;
    http3_client_body_budget& operator=(const http3_client_body_budget&) = delete;
    http3_client_body_budget(http3_client_body_budget&&) = delete;
    http3_client_body_budget& operator=(http3_client_body_budget&&) = delete;

    [[nodiscard]] bool try_retain(std::size_t bytes_value) noexcept {
        if (used_ > limit_ || bytes_value > limit_ - used_) {
            return false;
        }
        used_ += bytes_value;
        return true;
    }
    void release(std::size_t bytes_value) noexcept {
        if (bytes_value > used_) {
            std::terminate();
        }
        used_ -= bytes_value;
        if (bytes_value != 0) {
            notify_waiters();
        }
    }
    [[nodiscard]] bool try_replace(std::size_t old_bytes, std::size_t new_bytes) noexcept {
        if (old_bytes > used_) {
            std::terminate();
        }
        const auto without_old = used_ - old_bytes;
        if (without_old > limit_ || new_bytes > limit_ - without_old) {
            return false;
        }
        used_ = without_old + new_bytes;
        if (new_bytes < old_bytes) {
            notify_waiters();
        }
        return true;
    }
    [[nodiscard]] std::size_t used() const noexcept {
        return used_;
    }
    [[nodiscard]] std::size_t available() const noexcept {
        return used_ > limit_ ? 0 : limit_ - used_;
    }

private:
    void register_wake(wake_registration_type& registration, wake_function_type callback_value,
        void* context_value) noexcept {
        if (registration.budget_ != nullptr || callback_value == nullptr) {
            std::terminate();
        }
        registration.budget_ = this;
        registration.callback_ = callback_value;
        registration.context_ = context_value;
        registration.next_ = wake_head_;
        if (wake_head_ != nullptr) {
            wake_head_->previous_ = &registration;
        }
        wake_head_ = &registration;
    }
    void unregister_wake(wake_registration_type& registration) noexcept {
        if (registration.budget_ != this) {
            std::terminate();
        }
        if (registration.previous_ != nullptr) {
            registration.previous_->next_ = registration.next_;
        } else {
            wake_head_ = registration.next_;
        }
        if (registration.next_ != nullptr) {
            registration.next_->previous_ = registration.previous_;
        }
        registration.budget_ = nullptr;
        registration.previous_ = nullptr;
        registration.next_ = nullptr;
        registration.callback_ = nullptr;
        registration.context_ = nullptr;
    }
    void notify_waiters() noexcept {
        for (auto* registration = wake_head_; registration != nullptr;
            registration = registration->next_) {
            registration->callback_(registration->context_);
        }
    }

    const std::size_t limit_;
    std::size_t used_{};
    std::size_t active_leases_{};
    wake_registration_type* wake_head_{};
};

}  // namespace ruvia::detail
