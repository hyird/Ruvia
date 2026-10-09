#pragma once

#include <array>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "ruvia/core/move_only_function.h"

namespace ruvia {

class stop_token;
class stop_source;
[[nodiscard]] stop_token combine_stop_tokens(stop_token first, stop_token second);

namespace detail {

class stop_callback final {
public:
    explicit stop_callback(move_only_function<void()> callback_value) noexcept
        : callback_(std::move(callback_value)) {}

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
    move_only_function<void()> callback_;
};

class stop_callback_state final {
public:
    explicit stop_callback_state(move_only_function<void()> callback_value) noexcept
        : callback_(std::move(callback_value)) {}

    void invoke() noexcept {
        if (invoked_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        stop_callback(std::move(callback_))();
    }

private:
    std::atomic_flag invoked_ = ATOMIC_FLAG_INIT;
    move_only_function<void()> callback_;
};

class stop_callback_ref final {
public:
    explicit stop_callback_ref(stop_callback_state& state_value) noexcept
        : state_(&state_value) {}

    void operator()() const noexcept {
        state_->invoke();
    }

private:
    stop_callback_state* state_;
};

}  // namespace detail

class stop_registration final {
public:
    stop_registration() noexcept = default;
    ~stop_registration() {
        reset();
    }

    stop_registration(const stop_registration&) = delete;
    stop_registration& operator=(const stop_registration&) = delete;
    stop_registration(stop_registration&&) = delete;
    stop_registration& operator=(stop_registration&&) = delete;

    void reset() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (phase == registration_phase_type::idle || phase == registration_phase_type::reset_pending ||
                phase == registration_phase_type::resetting) {
                return;
            }
            if (phase == registration_phase_type::registering) {
                if (phase_.compare_exchange_weak(phase, registration_phase_type::reset_pending,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if (phase_.compare_exchange_weak(phase, registration_phase_type::resetting,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                clear_registration();
                phase_.store(registration_phase_type::idle, std::memory_order_release);
                return;
            }
        }
    }

    [[nodiscard]] bool registered() const noexcept {
        return phase_.load(std::memory_order_acquire) == registration_phase_type::registered;
    }

private:
    friend class stop_token;

    enum class registration_phase_type : unsigned char {
        idle,
        registering,
        registered,
        reset_pending,
        resetting,
    };

    stop_registration(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner_value, move_only_function<void()> callback_value) {
        register_callbacks(
            std::move(first), std::move(second), std::move(owner_value), std::move(callback_value));
    }

    void register_callbacks(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner_value, move_only_function<void()> callback_value) {
        if ((!first.stop_possible() && !second.stop_possible()) || !callback_value) {
            return;
        }
        if (first.stop_requested() || second.stop_requested()) {
            detail::stop_callback(std::move(callback_value))();
            return;
        }

        auto expected = registration_phase_type::idle;
        if (!phase_.compare_exchange_strong(expected, registration_phase_type::registering,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::terminate();
        }
        owner_ = std::move(owner_value);
        callback_state_.emplace(std::move(callback_value));
        if (first.stop_possible()) {
            first_callback_.emplace(std::move(first), detail::stop_callback_ref(*callback_state_));
        }
        if (second.stop_possible()) {
            second_callback_.emplace(std::move(second), detail::stop_callback_ref(*callback_state_));
        }

        expected = registration_phase_type::registering;
        if (phase_.compare_exchange_strong(expected, registration_phase_type::registered,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        if (expected != registration_phase_type::reset_pending) {
            std::terminate();
        }
        clear_registration();
        phase_.store(registration_phase_type::idle, std::memory_order_release);
    }

    void clear_registration() noexcept {
        second_callback_.reset();
        first_callback_.reset();
        callback_state_.reset();
        owner_.reset();
    }

    // The bridge owner outlives both callback registrations. Destruction and
    // reset run in the reverse order explicitly required by that contract.
    std::atomic<registration_phase_type> phase_{registration_phase_type::idle};
    std::shared_ptr<const void> owner_;
    std::optional<detail::stop_callback_state> callback_state_;
    std::optional<std::stop_callback<detail::stop_callback_ref>> first_callback_;
    std::optional<std::stop_callback<detail::stop_callback_ref>> second_callback_;
};

class stop_token final {
public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return first_token_.stop_requested() || second_token_.stop_requested();
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return first_token_.stop_possible() || second_token_.stop_possible();
    }

    // std::stop_callback embeds its registration node in stop_registration, so
    // registering a cancellation callback performs no callback-state allocation.
    // request_stop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] stop_registration register_callback(move_only_function<void()> callback_value) const {
        return stop_registration(first_token_, second_token_, owner_, std::move(callback_value));
    }

    // Reuses caller-owned registration storage. This is for awaiters that can
    // only form their cancellation callback after an operation has published
    // its id/generation in await_suspend; it preserves the embedded, zero-
    // allocation stop_callback representation.
    void register_callback(stop_registration& registration, move_only_function<void()> callback_value) const {
        registration.reset();
        registration.register_callbacks(first_token_, second_token_, owner_, std::move(callback_value));
    }

private:
    friend class stop_source;
    friend stop_token combine_stop_tokens(stop_token first, stop_token second);

    explicit stop_token(std::stop_token token, std::shared_ptr<const void> owner = {}) noexcept
        : first_token_(std::move(token)),
          owner_(std::move(owner)) {}

    stop_token(std::stop_token first, std::stop_token second,
        std::shared_ptr<const void> owner = {}) noexcept
        : first_token_(std::move(first)),
          second_token_(std::move(second)),
          owner_(std::move(owner)) {}

    // Two ordinary sources fit inline, covering ambient + explicit operation
    // cancellation without allocating a bridge on the request path.
    std::stop_token first_token_;
    std::stop_token second_token_;
    // Non-empty only when a deeper combination overflows the inline pair. It
    // owns the upstream registrations that feed one of the inline tokens.
    std::shared_ptr<const void> owner_;
};

class stop_source final {
public:
    stop_source() = default;
    stop_source(const stop_source&) = delete;
    stop_source& operator=(const stop_source&) = delete;
    stop_source(stop_source&&) = delete;
    stop_source& operator=(stop_source&&) = delete;

    void request_stop() noexcept {
        (void)source_.request_stop();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return source_.stop_requested();
    }

    [[nodiscard]] stop_token token() const noexcept {
        return stop_token(source_.get_token());
    }

private:
    std::stop_source source_;
};

namespace detail {

class combined_stop_state final {
public:
    combined_stop_state(stop_token first, stop_token second)
        : first_token_(std::move(first)),
          second_token_(std::move(second)),
          first_registration_(first_token_.register_callback([this] { request_stop(); })),
          second_registration_(second_token_.register_callback([this] { request_stop(); })) {}

    [[nodiscard]] std::stop_token token() const noexcept {
        return source_.get_token();
    }

private:
    void request_stop() noexcept {
        (void)source_.request_stop();
    }

    // Registrations are destroyed before their input tokens and source. A
    // callback may run concurrently with teardown; std::stop_callback's
    // destructor synchronizes that callback before source_ is destroyed.
    std::stop_source source_;
    stop_token first_token_;
    stop_token second_token_;
    stop_registration first_registration_;
    stop_registration second_registration_;
};

}  // namespace detail

inline stop_token combine_stop_tokens(stop_token first, stop_token second) {
    if (!first.stoppable()) {
        return second;
    }
    if (!second.stoppable() || first.stop_requested()) {
        return first;
    }
    if (second.stop_requested()) {
        return second;
    }
    std::array<std::stop_token, 2> inline_tokens;
    std::size_t inline_count = 0;
    const auto append = [&inline_tokens, &inline_count](const std::stop_token& token) noexcept {
        if (!token.stop_possible()) {
            return true;
        }
        for (std::size_t index = 0; index < inline_count; ++index) {
            if (inline_tokens[index] == token) {
                return true;
            }
        }
        if (inline_count == inline_tokens.size()) {
            return false;
        }
        inline_tokens[inline_count++] = token;
        return true;
    };
    const bool tokens_fit = append(first.first_token_) && append(first.second_token_) &&
                            append(second.first_token_) && append(second.second_token_);
    const bool owners_fit =
        first.owner_ == nullptr || second.owner_ == nullptr || first.owner_ == second.owner_;
    if (tokens_fit && owners_fit) {
        auto owner_value = first.owner_ != nullptr ? std::move(first.owner_) : std::move(second.owner_);
        if (inline_count == 1) {
            return stop_token(std::move(inline_tokens[0]), std::move(owner_value));
        }
        return stop_token(std::move(inline_tokens[0]), std::move(inline_tokens[1]), std::move(owner_value));
    }

    auto state_value = std::make_shared<detail::combined_stop_state>(std::move(first), std::move(second));
    auto token = state_value->token();
    return stop_token(std::move(token), std::move(state_value));
}

}  // namespace ruvia
