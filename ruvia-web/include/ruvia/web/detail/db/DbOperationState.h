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

    DbOperationState(DbOperationState&& other) noexcept {
        if (std::holds_alternative<Reserved>(other.state_) || std::holds_alternative<Operating>(other.state_)) {
            // A pending or operating coroutine borrows both this state and its
            // payload. Moving its owner would invalidate that structured lifetime.
            std::terminate();
        }
        if (auto* active = std::get_if<Active>(&other.state_); active != nullptr) {
            Payload payload(std::move(active->payload));
            state_.template emplace<Active>(std::move(payload));
        } else if (std::holds_alternative<Failed>(other.state_)) {
            state_.template emplace<Failed>();
        }
        other.state_.template emplace<Closed>();
    }
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

// One operation on an owner's DbOperationState. Construction reserves the
// lease immediately; start() marks the point at which the coroutine begins to
// drive it. Destroying a cold guard releases the reservation, while destroying
// a started guard fails the operation unless it named its own ending. Whoever
// owns the state supplies `state_` and a `Lease` payload type, and declares
// this a friend -- a stream and a transaction guard theirs identically.
template <typename Owner>
class DbOperationGuard final {
public:
    explicit DbOperationGuard(Owner& owner)
        : owner_(&owner) {
        owner_->state_.reserve();
    }

    DbOperationGuard(const DbOperationGuard&) = delete;
    DbOperationGuard& operator=(const DbOperationGuard&) = delete;
    DbOperationGuard(DbOperationGuard&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          started_(std::exchange(other.started_, false)) {}
    DbOperationGuard& operator=(DbOperationGuard&&) = delete;

    ~DbOperationGuard() {
        if (owner_ != nullptr) {
            if (started_) {
                owner_->state_.finishFailed();
            } else {
                owner_->state_.cancelReservation();
            }
        }
    }

    void start() noexcept {
        owner_->state_.start();
        started_ = true;
    }

    [[nodiscard]] typename Owner::Lease& lease() noexcept {
        return owner_->state_.operationPayload();
    }

    void finishActive() noexcept {
        owner_->state_.finishActive();
        owner_ = nullptr;
    }

    void finishClosed() noexcept {
        owner_->state_.finishClosed();
        owner_ = nullptr;
    }

    void finishFailed() noexcept {
        owner_->state_.finishFailed();
        owner_ = nullptr;
    }

private:
    Owner* owner_;
    bool started_{false};
};

}  // namespace ruvia::detail
