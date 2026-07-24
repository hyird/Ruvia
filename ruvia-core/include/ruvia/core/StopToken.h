#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace ruvia {

namespace detail {
struct StopState final {
    std::atomic_bool requested{false};
};

class StopSource;
}  // namespace detail

class StopToken final {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_ != nullptr && state_->requested.load(std::memory_order_acquire);
    }

private:
    friend class detail::StopSource;

    explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<const detail::StopState> state_;
};

namespace detail {

class StopSource final {
public:
    StopSource()
        : state_(std::make_shared<StopState>()) {}
    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    void requestStop() noexcept {
        state_->requested.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] StopToken token() const noexcept {
        return StopToken(state_);
    }

private:
    std::shared_ptr<StopState> state_;
};

}  // namespace detail

}  // namespace ruvia
