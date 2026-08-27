#pragma once

#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include <ruvia/core/MoveOnlyFunction.h>

namespace ruvia {

class StopToken;
class StopSource;
[[nodiscard]] StopToken combineStopTokens(StopToken first, StopToken second);

namespace detail {

class StopCallback final {
public:
    explicit StopCallback(MoveOnlyFunction<void()> callback) noexcept
        : callback_(std::move(callback)) {}

    void operator()() noexcept {
        try {
            if (callback_) {
                callback_();
            }
        } catch (...) {
            std::terminate();
        }
    }

private:
    MoveOnlyFunction<void()> callback_;
};

class StopCallbackState final {
public:
    explicit StopCallbackState(MoveOnlyFunction<void()> callback) noexcept
        : callback_(std::move(callback)) {}

    void invoke() noexcept {
        if (invoked_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        StopCallback(std::move(callback_))();
    }

private:
    std::atomic_flag invoked_ = ATOMIC_FLAG_INIT;
    MoveOnlyFunction<void()> callback_;
};

class StopCallbackRef final {
public:
    explicit StopCallbackRef(StopCallbackState& state) noexcept
        : state_(&state) {}

    void operator()() const noexcept {
        state_->invoke();
    }

private:
    StopCallbackState* state_;
};

}  // namespace detail

class StopRegistration final {
public:
    StopRegistration() noexcept = default;
    ~StopRegistration() {
        reset();
    }

    StopRegistration(const StopRegistration&) = delete;
    StopRegistration& operator=(const StopRegistration&) = delete;
    StopRegistration(StopRegistration&&) = delete;
    StopRegistration& operator=(StopRegistration&&) = delete;

    void reset() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (phase == RegistrationPhase::kIdle || phase == RegistrationPhase::kResetPending ||
                phase == RegistrationPhase::kResetting) {
                return;
            }
            if (phase == RegistrationPhase::kRegistering) {
                if (phase_.compare_exchange_weak(phase, RegistrationPhase::kResetPending,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if (phase_.compare_exchange_weak(phase, RegistrationPhase::kResetting,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                clearRegistration();
                phase_.store(RegistrationPhase::kIdle, std::memory_order_release);
                return;
            }
        }
    }

    [[nodiscard]] bool registered() const noexcept {
        return phase_.load(std::memory_order_acquire) == RegistrationPhase::kRegistered;
    }

private:
    friend class StopToken;

    enum class RegistrationPhase : unsigned char {
        kIdle,
        kRegistering,
        kRegistered,
        kResetPending,
        kResetting,
    };

    StopRegistration(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner, MoveOnlyFunction<void()> callback) {
        registerCallbacks(
            std::move(first), std::move(second), std::move(owner), std::move(callback));
    }

    void registerCallbacks(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner, MoveOnlyFunction<void()> callback) {
        if ((!first.stop_possible() && !second.stop_possible()) || !callback) {
            return;
        }
        if (first.stop_requested() || second.stop_requested()) {
            detail::StopCallback(std::move(callback))();
            return;
        }

        auto expected = RegistrationPhase::kIdle;
        if (!phase_.compare_exchange_strong(expected, RegistrationPhase::kRegistering,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::terminate();
        }
        owner_ = std::move(owner);
        callbackState_.emplace(std::move(callback));
        if (first.stop_possible()) {
            firstCallback_.emplace(std::move(first), detail::StopCallbackRef(*callbackState_));
        }
        if (second.stop_possible()) {
            secondCallback_.emplace(std::move(second), detail::StopCallbackRef(*callbackState_));
        }

        expected = RegistrationPhase::kRegistering;
        if (phase_.compare_exchange_strong(expected, RegistrationPhase::kRegistered,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        if (expected != RegistrationPhase::kResetPending) {
            std::terminate();
        }
        clearRegistration();
        phase_.store(RegistrationPhase::kIdle, std::memory_order_release);
    }

    void clearRegistration() noexcept {
        secondCallback_.reset();
        firstCallback_.reset();
        callbackState_.reset();
        owner_.reset();
    }

    // The bridge owner outlives both callback registrations. Destruction and
    // reset run in the reverse order explicitly required by that contract.
    std::atomic<RegistrationPhase> phase_{RegistrationPhase::kIdle};
    std::shared_ptr<const void> owner_;
    std::optional<detail::StopCallbackState> callbackState_;
    std::optional<std::stop_callback<detail::StopCallbackRef>> firstCallback_;
    std::optional<std::stop_callback<detail::StopCallbackRef>> secondCallback_;
};

class StopToken final {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return firstToken_.stop_requested() || secondToken_.stop_requested();
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return firstToken_.stop_possible() || secondToken_.stop_possible();
    }

    // std::stop_callback embeds its registration node in StopRegistration, so
    // registering a cancellation callback performs no callback-state allocation.
    // requestStop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] StopRegistration registerCallback(MoveOnlyFunction<void()> callback) const {
        return StopRegistration(firstToken_, secondToken_, owner_, std::move(callback));
    }

    // Reuses caller-owned registration storage. This is for awaiters that can
    // only form their cancellation callback after an operation has published
    // its id/generation in await_suspend; it preserves the embedded, zero-
    // allocation stop_callback representation.
    void registerCallback(StopRegistration& registration, MoveOnlyFunction<void()> callback) const {
        registration.reset();
        registration.registerCallbacks(firstToken_, secondToken_, owner_, std::move(callback));
    }

private:
    friend class StopSource;
    friend StopToken combineStopTokens(StopToken first, StopToken second);

    explicit StopToken(std::stop_token token, std::shared_ptr<const void> owner = {}) noexcept
        : firstToken_(std::move(token)),
          owner_(std::move(owner)) {}

    StopToken(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner = {}) noexcept
        : firstToken_(std::move(first)),
          secondToken_(std::move(second)),
          owner_(std::move(owner)) {}

    // Two ordinary sources fit inline, covering ambient + explicit operation
    // cancellation without allocating a bridge on the request path.
    std::stop_token firstToken_;
    std::stop_token secondToken_;
    // Non-empty only when a deeper combination overflows the inline pair. It
    // owns the upstream registrations that feed one of the inline tokens.
    std::shared_ptr<const void> owner_;
};

class StopSource final {
public:
    StopSource() = default;
    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    void requestStop() noexcept {
        (void)source_.request_stop();
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return source_.stop_requested();
    }

    [[nodiscard]] StopToken token() const noexcept {
        return StopToken(source_.get_token());
    }

private:
    std::stop_source source_;
};

namespace detail {

class CombinedStopState final {
public:
    CombinedStopState(StopToken first, StopToken second)
        : firstToken_(std::move(first)),
          secondToken_(std::move(second)),
          firstRegistration_(firstToken_.registerCallback([this] { requestStop(); })),
          secondRegistration_(secondToken_.registerCallback([this] { requestStop(); })) {}

    [[nodiscard]] std::stop_token token() const noexcept {
        return source_.get_token();
    }

private:
    void requestStop() noexcept {
        (void)source_.request_stop();
    }

    // Registrations are destroyed before their input tokens and source. A
    // callback may run concurrently with teardown; std::stop_callback's
    // destructor synchronizes that callback before source_ is destroyed.
    std::stop_source source_;
    StopToken firstToken_;
    StopToken secondToken_;
    StopRegistration firstRegistration_;
    StopRegistration secondRegistration_;
};

}  // namespace detail

inline StopToken combineStopTokens(StopToken first, StopToken second) {
    if (!first.stoppable()) {
        return second;
    }
    if (!second.stoppable() || first.stopRequested()) {
        return first;
    }
    if (second.stopRequested()) {
        return second;
    }
    std::array<std::stop_token, 2> inlineTokens;
    std::size_t inlineCount = 0;
    const auto append = [&inlineTokens, &inlineCount](const std::stop_token& token) noexcept {
        if (!token.stop_possible()) {
            return true;
        }
        for (std::size_t index = 0; index < inlineCount; ++index) {
            if (inlineTokens[index] == token) {
                return true;
            }
        }
        if (inlineCount == inlineTokens.size()) {
            return false;
        }
        inlineTokens[inlineCount++] = token;
        return true;
    };
    const bool tokensFit = append(first.firstToken_) && append(first.secondToken_) &&
                           append(second.firstToken_) && append(second.secondToken_);
    const bool ownersFit =
        first.owner_ == nullptr || second.owner_ == nullptr || first.owner_ == second.owner_;
    if (tokensFit && ownersFit) {
        auto owner = first.owner_ != nullptr ? std::move(first.owner_) : std::move(second.owner_);
        if (inlineCount == 1) {
            return StopToken(std::move(inlineTokens[0]), std::move(owner));
        }
        return StopToken(std::move(inlineTokens[0]), std::move(inlineTokens[1]), std::move(owner));
    }

    auto state = std::make_shared<detail::CombinedStopState>(std::move(first), std::move(second));
    auto token = state->token();
    return StopToken(std::move(token), std::move(state));
}

}  // namespace ruvia
