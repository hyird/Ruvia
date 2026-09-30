#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {

// A database stream or transaction owns one pool lease. Cold guards only
// register lifetime obligations that forbid destroying their owner; they do
// not reserve the operation right. Operating is the sole state that grants
// that right. Closed and Failed are terminal states so a failed lease can
// never be reused accidentally.
template <typename Payload>
class DbOperationGuard;

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
        if (coldBorrows_ != 0 || std::holds_alternative<Operating>(state_)) {
            std::terminate();
        }
    }

    DbOperationState(DbOperationState&&) = delete;
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

private:
    friend class DbOperationGuard<Payload>;

    void registerColdBorrow() noexcept {
        ++coldBorrows_;
    }

    void start() {
        auto* active = std::get_if<Active>(&state_);
        if (active == nullptr) {
            throw std::logic_error(std::holds_alternative<Operating>(state_)
                                       ? "database operation is already in progress"
                                       : "database resource is not active");
        }
        if (coldBorrows_ == 0) {
            std::terminate();
        }
        --coldBorrows_;
        Payload payload(std::move(active->payload));
        state_.template emplace<Operating>(std::move(payload));
    }

    void releaseColdBorrow() noexcept {
        if (coldBorrows_ == 0) {
            std::terminate();
        }
        --coldBorrows_;
    }

    [[nodiscard]] Payload& operationPayload(bool started) {
        if (started) {
            auto* operating = std::get_if<Operating>(&state_);
            if (operating == nullptr) {
                std::terminate();
            }
            return operating->payload;
        }
        if (auto* active = std::get_if<Active>(&state_); active != nullptr) {
            return active->payload;
        }
        throw std::logic_error(std::holds_alternative<Operating>(state_)
                                   ? "database operation is already in progress"
                                   : "database resource is not active");
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

public:
    template <typename Release>
        requires std::is_nothrow_invocable_v<Release&, Payload&>
    void reset(Release&& release) noexcept {
        if (coldBorrows_ != 0 || std::holds_alternative<Operating>(state_)) {
            // Destroying the database owner while any operation borrows it would
            // leave that coroutine with a dangling owner, even before start().
            std::terminate();
        }
        if (auto* active = std::get_if<Active>(&state_); active != nullptr) {
            release(active->payload);
        }
        state_.template emplace<Closed>();
    }

private:
    std::variant<Closed, Active, Operating, Failed> state_{};
    std::size_t coldBorrows_{0};
};

// One operation on a DbOperationState. Construction only registers a cold
// lifetime borrow. start() acquires the exclusive lease when the coroutine
// actually begins. Dropping a cold guard unregisters only its own borrow;
// destroying a started guard without naming an ending fails the lease.
template <typename Payload>
class DbOperationGuard final {
public:
    using State = DbOperationState<Payload>;

    explicit DbOperationGuard(State& state) noexcept
        : state_(&state) {
        state_->registerColdBorrow();
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
                state_->releaseColdBorrow();
            }
        }
    }

    void start() {
        state_->start();
        started_ = true;
    }

    [[nodiscard]] Payload& lease() {
        return state_->operationPayload(started_);
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
