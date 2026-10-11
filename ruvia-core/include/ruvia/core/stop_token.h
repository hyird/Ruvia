#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include "ruvia/core/move_only_function.h"

namespace ruvia {

class stop_token;
class stop_source;

namespace detail {

// Whether a std::stop_token refers to a stop state at all. Unlike
// stop_possible(), this stays true after its last source died unstopped.
[[nodiscard]] inline bool has_stop_state(const std::stop_token& token) noexcept {
    return token != std::stop_token{};
}

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

// A stop_token observes at most this many distinct stop sources. Combination
// flattens its inputs into the token itself, so tokens never own a bridge and
// combining or registering never allocates.
inline constexpr std::size_t max_stop_token_sources = 4;

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

    using source_tokens = std::array<std::stop_token, max_stop_token_sources>;

    stop_registration(const source_tokens& tokens, move_only_function<void()> callback_value) {
        register_callbacks(tokens, std::move(callback_value));
    }

    // `tokens` holds its entries with a stop state as a prefix; an entry whose
    // source died without a stop request stays in place but never fires.
    void register_callbacks(const source_tokens& tokens, move_only_function<void()> callback_value) {
        if (!callback_value) {
            return;
        }
        bool stop_possible = false;
        for (const auto& token : tokens) {
            if (!detail::has_stop_state(token)) {
                break;
            }
            if (token.stop_requested()) {
                detail::stop_callback(std::move(callback_value))();
                return;
            }
            stop_possible = stop_possible || token.stop_possible();
        }
        if (!stop_possible) {
            return;
        }

        auto expected = registration_phase_type::idle;
        if (!phase_.compare_exchange_strong(expected, registration_phase_type::registering,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::terminate();
        }
        callback_state_.emplace(std::move(callback_value));
        for (std::size_t index = 0; index != tokens.size() && detail::has_stop_state(tokens[index]); ++index) {
            if (tokens[index].stop_possible()) {
                callbacks_[index].emplace(tokens[index], detail::stop_callback_ref(*callback_state_));
            }
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

    // Each std::stop_callback destructor synchronizes with a concurrently
    // running callback, so the shared callback state is released last.
    void clear_registration() noexcept {
        for (auto callback = callbacks_.rbegin(); callback != callbacks_.rend(); ++callback) {
            callback->reset();
        }
        callback_state_.reset();
    }

    std::atomic<registration_phase_type> phase_{registration_phase_type::idle};
    std::optional<detail::stop_callback_state> callback_state_;
    std::array<std::optional<std::stop_callback<detail::stop_callback_ref>>, max_stop_token_sources> callbacks_;
};

// A copyable view of up to max_stop_token_sources stop sources. It never owns
// state beyond std::stop_token references, so copies are cheap and independent.
class stop_token final {
public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        for (const auto& token : tokens_) {
            if (!detail::has_stop_state(token)) {
                return false;
            }
            if (token.stop_requested()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool stoppable() const noexcept {
        for (const auto& token : tokens_) {
            if (!detail::has_stop_state(token)) {
                return false;
            }
            if (token.stop_possible()) {
                return true;
            }
        }
        return false;
    }

    // std::stop_callback embeds its registration node in stop_registration, so
    // registering a cancellation callback performs no callback-state allocation.
    // request_stop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] stop_registration register_callback(move_only_function<void()> callback_value) const {
        return stop_registration(tokens_, std::move(callback_value));
    }

    // Reuses caller-owned registration storage. This is for awaiters that can
    // only form their cancellation callback after an operation has published
    // its id/generation in await_suspend; it preserves the embedded, zero-
    // allocation stop_callback representation.
    void register_callback(stop_registration& registration, move_only_function<void()> callback_value) const {
        registration.reset();
        registration.register_callbacks(tokens_, std::move(callback_value));
    }

    // Observes every still-stoppable source of both inputs. Duplicate sources
    // are merged; more than max_stop_token_sources distinct stoppable sources
    // throw std::length_error. An input that is already stopped, or the other
    // one when an input cannot stop, is returned as is.
    [[nodiscard]] friend stop_token combine_stop_tokens(stop_token first, stop_token second) {
        if (!first.stoppable() || second.stop_requested()) {
            return second;
        }
        if (!second.stoppable() || first.stop_requested()) {
            return first;
        }
        stop_token combined;
        combined.append_stoppable(std::move(first));
        combined.append_stoppable(std::move(second));
        return combined;
    }

private:
    friend class stop_source;

    explicit stop_token(std::stop_token token) noexcept {
        tokens_[0] = std::move(token);
    }

    void append_stoppable(stop_token other) {
        for (auto& token : other.tokens_) {
            if (!detail::has_stop_state(token)) {
                return;
            }
            if (token.stop_possible()) {
                append(std::move(token));
            }
        }
    }

    void append(std::stop_token token) {
        for (auto& slot : tokens_) {
            if (!detail::has_stop_state(slot)) {
                slot = std::move(token);
                return;
            }
            if (slot == token) {
                return;
            }
        }
        throw std::length_error("stop_token combines at most max_stop_token_sources distinct sources");
    }

    // Entries with a stop state form a prefix; the rest are empty.
    stop_registration::source_tokens tokens_;
};

[[nodiscard]] stop_token combine_stop_tokens(stop_token first, stop_token second);

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

}  // namespace ruvia
