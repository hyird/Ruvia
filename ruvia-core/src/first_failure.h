#pragma once

#include <exception>
#include <utility>

namespace ruvia {

// The first failure of a runtime owner, kept until the owner rethrows it or
// hands it to the unhandled-failure diagnostic. Later failures lose. The owner
// guards it with its own lifecycle lock so recording and waking its waiters
// stay one critical section.
class first_failure final {
public:
    // Returns true for the winning failure. `observed` records that a consumer
    // (such as a failure hook) already owns it.
    [[nodiscard]] bool record(std::exception_ptr failure, bool observed) noexcept {
        if (failure == nullptr || first_ != nullptr) {
            return false;
        }
        first_ = std::move(failure);
        observed_ = observed;
        return true;
    }

    [[nodiscard]] const std::exception_ptr& first() const noexcept {
        return first_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return first_ != nullptr;
    }

    // Returns the first failure, if any, for the caller to rethrow.
    [[nodiscard]] std::exception_ptr observe() noexcept {
        observed_ = true;
        return first_;
    }

    // Releases a first failure nobody observed; the caller must rethrow or
    // report it. A later failure may then become the next first one.
    [[nodiscard]] std::exception_ptr take_unobserved() noexcept {
        if (observed_) {
            return {};
        }
        return std::exchange(first_, nullptr);
    }

private:
    std::exception_ptr first_;
    bool observed_{false};
};

}  // namespace ruvia
