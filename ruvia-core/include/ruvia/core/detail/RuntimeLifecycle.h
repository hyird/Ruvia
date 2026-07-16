#pragma once

#include <atomic>
#include <cstdint>

namespace ruvia::detail {

class RuntimeLifecycle final {
public:
    enum class State : std::uint8_t {
        kReady,
        kRunning,
        kStopping,
        kStopped,
    };

    [[nodiscard]] bool start() noexcept {
        auto expected = State::kReady;
        return state_.compare_exchange_strong(
            expected,
            State::kRunning,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] bool requestStop() noexcept {
        auto observed = state_.load(std::memory_order_acquire);
        while (observed == State::kReady || observed == State::kRunning) {
            if (state_.compare_exchange_weak(
                    observed,
                    State::kStopping,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void completeStop() noexcept {
        auto expected = State::kStopping;
        (void)state_.compare_exchange_strong(
            expected,
            State::kStopped,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] State state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<State> state_{State::kReady};
};

}  // namespace ruvia::detail
