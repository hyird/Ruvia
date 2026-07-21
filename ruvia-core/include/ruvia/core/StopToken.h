#pragma once

#include <atomic>

namespace ruvia {

namespace detail {
class StopSource;
}

class StopToken final {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return requested_ != nullptr &&
            requested_->load(std::memory_order_acquire);
    }

private:
    friend class detail::StopSource;

    explicit StopToken(const std::atomic_bool& requested) noexcept
        : requested_(&requested) {}

    const std::atomic_bool* requested_{nullptr};
};

namespace detail {

class StopSource final {
public:
    StopSource() noexcept = default;
    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    void requestStop() noexcept {
        requested_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] StopToken token() const noexcept {
        return StopToken(requested_);
    }

private:
    std::atomic_bool requested_{false};
};

}  // namespace detail

}  // namespace ruvia
