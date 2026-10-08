#pragma once

#include <cstddef>
#include <exception>
#include <limits>

namespace ruvia::detail {

// Worker-affine receive-storage accounting. The stable owner may be shared by
// successive HTTP/3 connections on the same worker; it is deliberately
// independent from the escaped, thread-safe HttpClientResultBudgetDomain.
// Owners must outlive every lease and wake registration.
class Http3ClientBodyBudget final {
public:
    using WakeFunction = void (*)(void*) noexcept;

    class Lease final {
    public:
        Lease() noexcept = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : budget_(other.budget_),
              bytes_(other.bytes_) {
            other.budget_ = nullptr;
            other.bytes_ = 0;
        }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                budget_ = other.budget_;
                bytes_ = other.bytes_;
                other.budget_ = nullptr;
                other.bytes_ = 0;
            }
            return *this;
        }
        ~Lease() {
            reset();
        }

        [[nodiscard]] bool attach(Http3ClientBodyBudget& budget, std::size_t bytes) noexcept {
            if (budget_ != nullptr || !budget.tryRetain(bytes)) {
                return false;
            }
            if (budget.activeLeases_ == std::numeric_limits<std::size_t>::max()) {
                std::terminate();
            }
            ++budget.activeLeases_;
            budget_ = &budget;
            bytes_ = bytes;
            return true;
        }
        void reset() noexcept {
            auto* const budget = budget_;
            if (budget == nullptr) {
                return;
            }
            budget_ = nullptr;
            const auto bytes = bytes_;
            bytes_ = 0;
            budget->release(bytes);
            if (budget->activeLeases_ == 0) {
                std::terminate();
            }
            --budget->activeLeases_;
        }
        [[nodiscard]] bool attached() const noexcept {
            return budget_ != nullptr;
        }
        [[nodiscard]] std::size_t retainedBytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] std::size_t available() const noexcept {
            return budget_ == nullptr ? std::numeric_limits<std::size_t>::max()
                                      : budget_->available();
        }
        void notifyProducer() const noexcept {
            if (budget_ != nullptr) {
                budget_->notifyWaiters();
            }
        }
        [[nodiscard]] bool tryRetain(std::size_t bytes) noexcept {
            if (budget_ == nullptr || !budget_->tryRetain(bytes)) {
                return false;
            }
            if (bytes > std::numeric_limits<std::size_t>::max() - bytes_) {
                std::terminate();
            }
            bytes_ += bytes;
            return true;
        }
        void release(std::size_t bytes) noexcept {
            if (budget_ == nullptr || bytes > bytes_) {
                std::terminate();
            }
            bytes_ -= bytes;
            budget_->release(bytes);
        }
        [[nodiscard]] bool tryReplace(std::size_t bytes) noexcept {
            if (budget_ == nullptr || !budget_->tryReplace(bytes_, bytes)) {
                return false;
            }
            bytes_ = bytes;
            return true;
        }

    private:
        Http3ClientBodyBudget* budget_{};
        std::size_t bytes_{};
    };

    // Intrusive worker-local wake registration. A release synchronously visits
    // the registered connection drivers; callbacks must not mutate this list.
    class WakeRegistration final {
    public:
        WakeRegistration() noexcept = default;
        WakeRegistration(Http3ClientBodyBudget& budget, WakeFunction callback,
            void* context) noexcept {
            budget.registerWake(*this, callback, context);
        }
        WakeRegistration(const WakeRegistration&) = delete;
        WakeRegistration& operator=(const WakeRegistration&) = delete;
        WakeRegistration(WakeRegistration&&) = delete;
        WakeRegistration& operator=(WakeRegistration&&) = delete;
        ~WakeRegistration() {
            reset();
        }

        void reset() noexcept {
            if (budget_ != nullptr) {
                budget_->unregisterWake(*this);
            }
        }

    private:
        Http3ClientBodyBudget* budget_{};
        WakeRegistration* previous_{};
        WakeRegistration* next_{};
        WakeFunction callback_{};
        void* context_{};
        friend class Http3ClientBodyBudget;
    };

    explicit Http3ClientBodyBudget(std::size_t limit) noexcept
        : limit_(limit) {}
    ~Http3ClientBodyBudget() {
        if (used_ != 0 || activeLeases_ != 0 || wakeHead_ != nullptr) {
            std::terminate();
        }
    }
    Http3ClientBodyBudget(const Http3ClientBodyBudget&) = delete;
    Http3ClientBodyBudget& operator=(const Http3ClientBodyBudget&) = delete;
    Http3ClientBodyBudget(Http3ClientBodyBudget&&) = delete;
    Http3ClientBodyBudget& operator=(Http3ClientBodyBudget&&) = delete;

    [[nodiscard]] bool tryRetain(std::size_t bytes) noexcept {
        if (used_ > limit_ || bytes > limit_ - used_) {
            return false;
        }
        used_ += bytes;
        return true;
    }
    void release(std::size_t bytes) noexcept {
        if (bytes > used_) {
            std::terminate();
        }
        used_ -= bytes;
        if (bytes != 0) {
            notifyWaiters();
        }
    }
    [[nodiscard]] bool tryReplace(std::size_t oldBytes, std::size_t newBytes) noexcept {
        if (oldBytes > used_) {
            std::terminate();
        }
        const auto withoutOld = used_ - oldBytes;
        if (withoutOld > limit_ || newBytes > limit_ - withoutOld) {
            return false;
        }
        used_ = withoutOld + newBytes;
        if (newBytes < oldBytes) {
            notifyWaiters();
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
    void registerWake(WakeRegistration& registration, WakeFunction callback,
        void* context) noexcept {
        if (registration.budget_ != nullptr || callback == nullptr) {
            std::terminate();
        }
        registration.budget_ = this;
        registration.callback_ = callback;
        registration.context_ = context;
        registration.next_ = wakeHead_;
        if (wakeHead_ != nullptr) {
            wakeHead_->previous_ = &registration;
        }
        wakeHead_ = &registration;
    }
    void unregisterWake(WakeRegistration& registration) noexcept {
        if (registration.budget_ != this) {
            std::terminate();
        }
        if (registration.previous_ != nullptr) {
            registration.previous_->next_ = registration.next_;
        } else {
            wakeHead_ = registration.next_;
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
    void notifyWaiters() noexcept {
        for (auto* registration = wakeHead_; registration != nullptr;
            registration = registration->next_) {
            registration->callback_(registration->context_);
        }
    }

    const std::size_t limit_;
    std::size_t used_{};
    std::size_t activeLeases_{};
    WakeRegistration* wakeHead_{};
};

}  // namespace ruvia::detail
