#pragma once

#include <atomic>
#include <cstdint>

namespace ruvia {

class runtime_lifecycle final {
public:
    enum class state_type : std::uint8_t {
        ready,
        running,
        stopping,
        stopped,
    };

    [[nodiscard]] bool start() noexcept {
        auto expected = state_type::ready;
        return state_.compare_exchange_strong(
            expected, state_type::running, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool request_stop() noexcept {
        auto observed_value = state_.load(std::memory_order_acquire);
        while (observed_value == state_type::ready || observed_value == state_type::running) {
            if (state_.compare_exchange_weak(observed_value, state_type::stopping, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void complete_stop() noexcept {
        auto expected = state_type::stopping;
        (void)state_.compare_exchange_strong(
            expected, state_type::stopped, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] state_type state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<state_type> state_{state_type::ready};
};

}  // namespace ruvia
