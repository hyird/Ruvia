#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "ruvia/core/move_only_function.h"

namespace ruvia {

class stop_token;
class stop_source;

namespace detail {

class stop_state;
class stop_callback_node;

// Shared ownership of one stop_state. Copies retain it; the last owner, on
// whichever thread it retires, returns the state to its memory resource.
class stop_state_ref final {
public:
    stop_state_ref() noexcept = default;
    stop_state_ref(const stop_state_ref& other) noexcept;
    stop_state_ref(stop_state_ref&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    stop_state_ref& operator=(stop_state_ref other) noexcept {
        std::swap(state_, other.state_);
        return *this;
    }
    ~stop_state_ref();

    [[nodiscard]] stop_state* operator->() const noexcept {
        return state_;
    }
    // Whether this refers to a stop state at all. Unlike stop_possible(), this
    // stays true after the source died unstopped.
    [[nodiscard]] explicit operator bool() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] friend bool operator==(const stop_state_ref&, const stop_state_ref&) noexcept = default;

private:
    friend class stop_state;

    // Adopts a reference the caller already holds.
    explicit stop_state_ref(stop_state* adopted) noexcept
        : state_(adopted) {}

    stop_state* state_{nullptr};
};

// The state one stop_source shares with its tokens and registrations. It is
// allocated from a caller-chosen memory resource instead of the global heap;
// because its last owner may retire on any thread, that resource must be
// thread-safe and outlive every owner.
//
// Callbacks form an intrusive list under a mutex. request_stop() runs each one
// with the mutex released; unlinking a callback that is running on another
// thread waits for it to return, while a callback unlinking itself does not.
class stop_state final {
public:
    stop_state(const stop_state&) = delete;
    stop_state& operator=(const stop_state&) = delete;

    [[nodiscard]] static stop_state_ref create(std::pmr::memory_resource& resource);

    void retain() noexcept {
        references_.fetch_add(1, std::memory_order_relaxed);
    }
    void release() noexcept {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy();
        }
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return (flags_.load(std::memory_order_acquire) & requested_flag) != 0;
    }
    // A stop is possible while the source lives or once it has been requested.
    [[nodiscard]] bool stop_possible() const noexcept {
        return flags_.load(std::memory_order_acquire) != 0;
    }
    void retire_source() noexcept {
        flags_.fetch_and(static_cast<std::uint8_t>(~source_flag), std::memory_order_acq_rel);
    }
    // Returns false when stop was already requested.
    bool request_stop() noexcept;

private:
    friend class stop_callback_node;

    static constexpr std::uint8_t source_flag = 1;
    static constexpr std::uint8_t requested_flag = 2;

    explicit stop_state(std::pmr::memory_resource& resource) noexcept
        : resource_(&resource) {}
    ~stop_state() = default;

    void destroy() noexcept;
    // Returns false, linking nothing, when stop was already requested.
    [[nodiscard]] bool link(stop_callback_node& node) noexcept;
    void unlink(stop_callback_node& node) noexcept;

    std::atomic<std::uint8_t> flags_{source_flag};
    std::atomic<std::size_t> references_{1};
    std::pmr::memory_resource* resource_;
    std::mutex mutex_;
    stop_callback_node* head_{nullptr};
    // The callback request_stop() is running with the mutex released, and the
    // thread running it.
    stop_callback_node* running_{nullptr};
    std::thread::id requester_;
};

inline stop_state_ref::stop_state_ref(const stop_state_ref& other) noexcept
    : state_(other.state_) {
    if (state_ != nullptr) {
        state_->retain();
    }
}

inline stop_state_ref::~stop_state_ref() {
    if (state_ != nullptr) {
        state_->release();
    }
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

// One registration's link into one stop state. Construction either links the
// node or, when stop was already requested, runs the callback at once.
// Destruction unlinks it; if the callback is running on another thread, it
// waits for that run to return, so the callback state can be released next.
class stop_callback_node final {
public:
    stop_callback_node(const stop_state_ref& state_value, stop_callback_state& callback) noexcept
        : state_(state_value),
          callback_(&callback) {
        if (!state_->link(*this)) {
            callback_->invoke();
        }
    }
    ~stop_callback_node() {
        state_->unlink(*this);
    }

    stop_callback_node(const stop_callback_node&) = delete;
    stop_callback_node& operator=(const stop_callback_node&) = delete;
    stop_callback_node(stop_callback_node&&) = delete;
    stop_callback_node& operator=(stop_callback_node&&) = delete;

private:
    friend class stop_state;

    stop_state_ref state_;
    stop_callback_state* callback_;
    stop_callback_node* previous_{nullptr};
    stop_callback_node* next_{nullptr};
    bool linked_{false};
    // While running: the requester's flag recording that the callback
    // destroyed this node from inside its own run.
    bool* destroyed_{nullptr};
    std::atomic<bool> completed_{false};
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

    using source_tokens = std::array<detail::stop_state_ref, max_stop_token_sources>;

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
            if (!token) {
                break;
            }
            if (token->stop_requested()) {
                detail::stop_callback(std::move(callback_value))();
                return;
            }
            stop_possible = stop_possible || token->stop_possible();
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
        for (std::size_t index = 0; index != tokens.size() && tokens[index]; ++index) {
            if (tokens[index]->stop_possible()) {
                callbacks_[index].emplace(tokens[index], *callback_state_);
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

    // Each node's destructor waits for a callback running on another thread, so
    // the shared callback state is released last.
    void clear_registration() noexcept {
        for (auto callback = callbacks_.rbegin(); callback != callbacks_.rend(); ++callback) {
            callback->reset();
        }
        callback_state_.reset();
    }

    std::atomic<registration_phase_type> phase_{registration_phase_type::idle};
    std::optional<detail::stop_callback_state> callback_state_;
    std::array<std::optional<detail::stop_callback_node>, max_stop_token_sources> callbacks_;
};

// A copyable view of up to max_stop_token_sources stop sources. It owns only
// references to their stop states, so copies are cheap and independent.
class stop_token final {
public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        for (const auto& token : tokens_) {
            if (!token) {
                return false;
            }
            if (token->stop_requested()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool stoppable() const noexcept {
        for (const auto& token : tokens_) {
            if (!token) {
                return false;
            }
            if (token->stop_possible()) {
                return true;
            }
        }
        return false;
    }

    // The registration embeds one intrusive node per observed source, so
    // registering a cancellation callback performs no allocation.
    // request_stop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] stop_registration register_callback(move_only_function<void()> callback_value) const {
        return stop_registration(tokens_, std::move(callback_value));
    }

    // Reuses caller-owned registration storage. This is for awaiters that can
    // only form their cancellation callback after an operation has published
    // its id/generation in await_suspend; it preserves the embedded, zero-
    // allocation node representation.
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

    explicit stop_token(detail::stop_state_ref state_value) noexcept {
        tokens_[0] = std::move(state_value);
    }

    void append_stoppable(stop_token other) {
        for (auto& token : other.tokens_) {
            if (!token) {
                return;
            }
            if (token->stop_possible()) {
                append(std::move(token));
            }
        }
    }

    void append(detail::stop_state_ref token) {
        for (auto& slot : tokens_) {
            if (!slot) {
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

// Owns one stop state and requests its stop. The state comes from the process
// memory pool, never the global heap, and lives until this source and every
// token and registration observing it retire, on whichever thread that is.
class stop_source final {
public:
    stop_source();
    ~stop_source() {
        state_->retire_source();
    }
    stop_source(const stop_source&) = delete;
    stop_source& operator=(const stop_source&) = delete;
    stop_source(stop_source&&) = delete;
    stop_source& operator=(stop_source&&) = delete;

    // Callbacks run on this thread before it returns. One of them may destroy
    // this source, so the request holds its own reference to the state.
    void request_stop() noexcept {
        const auto state_value = state_;
        (void)state_value->request_stop();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_->stop_requested();
    }

    [[nodiscard]] stop_token token() const noexcept {
        return stop_token(state_);
    }

private:
    detail::stop_state_ref state_;
};

}  // namespace ruvia
