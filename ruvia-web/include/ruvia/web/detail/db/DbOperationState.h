#pragma once

#include <concepts>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

// A database stream or transaction owns one pool lease.  Active means the
// lease is idle and may admit an operation; Operating means exactly one
// coroutine currently drives the connection.  Closed and Failed are distinct
// terminal states so a failed lease can never be reused accidentally.
template <typename Payload>
class DbOperationState final {
public:
    struct Closed final {};
    struct Failed final {};

    struct Active final {
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
        if (std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
    }

    DbOperationState(DbOperationState&& other) noexcept {
        if (std::holds_alternative<Operating>(other.state_)) {
            // An operating coroutine borrows both this state and its payload.
            // Moving its owner would invalidate that structured lifetime.
            std::terminate();
        }
        if (auto* active = std::get_if<Active>(&other.state_);
            active != nullptr) {
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
        if (std::holds_alternative<Operating>(state_)) {
            throw std::logic_error("database operation is already in progress");
        }
        const auto* active = std::get_if<Active>(&state_);
        if (active == nullptr) {
            throw std::logic_error("database resource is not active");
        }
        return active->payload;
    }

    [[nodiscard]] Payload& begin() {
        if (std::holds_alternative<Operating>(state_)) {
            throw std::logic_error("database operation is already in progress");
        }
        auto* active = std::get_if<Active>(&state_);
        if (active == nullptr) {
            throw std::logic_error("database resource is not active");
        }
        Payload payload(std::move(active->payload));
        state_.template emplace<Operating>(std::move(payload));
        return std::get<Operating>(state_).payload;
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
        } else if (std::holds_alternative<Operating>(state_)) {
            // Destroying the database owner while its structured Task is still
            // running would leave that coroutine borrowing a dead object.
            std::terminate();
        }
        state_.template emplace<Closed>();
    }

private:
    std::variant<Closed, Active, Operating, Failed> state_{};
};

// One operation on an owner's DbOperationState. Construction begins the
// operation and takes the lease; destruction fails it, unless the operation
// named its own ending first. Whoever owns the state supplies `state_` and a
// `Lease` payload type, and declares this a friend -- a stream and a
// transaction guard theirs identically.
template <typename Owner>
class DbOperationGuard final {
public:
    explicit DbOperationGuard(Owner& owner)
        : owner_(&owner),
          lease_(&owner.state_.begin()) {}

    DbOperationGuard(const DbOperationGuard&) = delete;
    DbOperationGuard& operator=(const DbOperationGuard&) = delete;

    ~DbOperationGuard() {
        if (owner_ != nullptr) {
            owner_->state_.finishFailed();
        }
    }

    [[nodiscard]] typename Owner::Lease& lease() noexcept { return *lease_; }

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
    typename Owner::Lease* lease_;
};

}  // namespace ruvia::detail
