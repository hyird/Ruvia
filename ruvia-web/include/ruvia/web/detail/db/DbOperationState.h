#pragma once

#include <concepts>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

// A database stream or transaction owns one pool lease. Active means the lease
// is idle and may admit an operation; Reserved means exactly one cold
// operation owns the lease reservation; Operating means its coroutine drives
// the connection. Closed and Failed are distinct terminal states so a failed
// lease can never be reused accidentally.
template <typename Payload>
class DbOperationState final {
public:
    struct Closed final {};
    struct Failed final {};

    struct Active final {
        Payload payload;
    };

    struct Reserved final {
        Payload payload;
    };

    struct Operating final {
        Payload payload;
    };

    DbOperationState() noexcept = default;

    explicit DbOperationState(Payload payload) noexcept
        : state_(std::in_place_type<Active>, std::move(payload)) {
        static_assert(std::is_nothrow_move_constructible_v<Payload>);
    }

    DbOperationState(const DbOperationState&) = delete;
    DbOperationState& operator=(const DbOperationState&) = delete;
    ~DbOperationState() {
        if (std::holds_alternative<Reserved>(state_) || std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
    }

    DbOperationState(DbOperationState&&) = delete;
    DbOperationState& operator=(DbOperationState&&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<Active>(state_);
    }

    [[nodiscard]] const Payload& activePayload() const {
        if (std::holds_alternative<Reserved>(state_) || std::holds_alternative<Operating>(state_)) {
            throw std::logic_error("database operation is already in progress");
        }
        const auto* active = std::get_if<Active>(&state_);
        if (active == nullptr) {
            throw std::logic_error("database resource is not active");
        }
        return active->payload;
    }

    void reserve() {
        if (std::holds_alternative<Reserved>(state_) || std::holds_alternative<Operating>(state_)) {
            throw std::logic_error("database operation is already in progress");
        }
        auto* active = std::get_if<Active>(&state_);
        if (active == nullptr) {
            throw std::logic_error("database resource is not active");
        }
        Payload payload(std::move(active->payload));
        state_.template emplace<Reserved>(std::move(payload));
    }

    void start() noexcept {
        auto* reserved = std::get_if<Reserved>(&state_);
        if (reserved == nullptr) {
            std::terminate();
        }
        Payload payload(std::move(reserved->payload));
        state_.template emplace<Operating>(std::move(payload));
    }

    void cancelReservation() noexcept {
        auto* reserved = std::get_if<Reserved>(&state_);
        if (reserved == nullptr) {
            std::terminate();
        }
        Payload payload(std::move(reserved->payload));
        state_.template emplace<Active>(std::move(payload));
    }

    [[nodiscard]] Payload& operationPayload() noexcept {
        if (auto* reserved = std::get_if<Reserved>(&state_); reserved != nullptr) {
            return reserved->payload;
        }
        if (auto* operating = std::get_if<Operating>(&state_); operating != nullptr) {
            return operating->payload;
        }
        std::terminate();
    }

    void finishActive() noexcept {
        if (!std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
        auto& operating = std::get<Operating>(state_);
        Payload payload(std::move(operating.payload));
        state_.template emplace<Active>(std::move(payload));
    }

    void finishClosed() noexcept {
        if (!std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
        state_.template emplace<Closed>();
    }

    void finishFailed() noexcept {
        if (!std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
        state_.template emplace<Failed>();
    }

    template <typename Release>
        requires std::is_nothrow_invocable_v<Release&, Payload&>
    void reset(Release&& release) noexcept {
        if (auto* active = std::get_if<Active>(&state_); active != nullptr) {
            release(active->payload);
        } else if (std::holds_alternative<Reserved>(state_) || std::holds_alternative<Operating>(state_)) {
            // Destroying the database owner while its structured operation is
            // pending or running would leave that coroutine borrowing a dead object.
            std::terminate();
        }
        state_.template emplace<Closed>();
    }

private:
    std::variant<Closed, Active, Reserved, Operating, Failed> state_{};
};

// One operation on a DbOperationState. Construction reserves the lease
// immediately; start() marks the point at which the coroutine begins to drive
// it. Destroying a cold guard releases the reservation, while destroying a
// started guard fails the operation unless it named its own ending.
template <typename Payload>
class DbOperationGuard final {
public:
    using State = DbOperationState<Payload>;

    explicit DbOperationGuard(State& state)
        : state_(&state) {
        state_->reserve();
    }

    DbOperationGuard(const DbOperationGuard&) = delete;
    DbOperationGuard& operator=(const DbOperationGuard&) = delete;
    DbOperationGuard(DbOperationGuard&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          started_(std::exchange(other.started_, false)) {}
    DbOperationGuard& operator=(DbOperationGuard&&) = delete;

    ~DbOperationGuard() {
        if (state_ != nullptr) {
            if (started_) {
                state_->finishFailed();
            } else {
                state_->cancelReservation();
            }
        }
    }

    void start() noexcept {
        state_->start();
        started_ = true;
    }

    [[nodiscard]] Payload& lease() noexcept {
        return state_->operationPayload();
    }

    void finishActive() noexcept {
        state_->finishActive();
        state_ = nullptr;
    }

    void finishClosed() noexcept {
        state_->finishClosed();
        state_ = nullptr;
    }

    void finishFailed() noexcept {
        state_->finishFailed();
        state_ = nullptr;
    }

private:
    State* state_;
    bool started_{false};
};

}  // namespace ruvia::detail
