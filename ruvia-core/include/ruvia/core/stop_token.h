#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

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
    // Whether this refers to a stop state at all. Unlike stoppable(), this
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
// allocated from the process-lifetime mimalloc resource, which supports
// releasing its last owner on any thread, even after the allocating thread exits.
//
// Callbacks form an intrusive list under a mutex. request_stop() runs each one
// with the mutex released; unlinking a callback that is running on another
// thread waits for it to return, while a callback unlinking itself does not.
class stop_state final {
public:
    stop_state(const stop_state&) = delete;
    stop_state& operator=(const stop_state&) = delete;

    [[nodiscard]] static stop_state_ref create();
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
    void request_stop() noexcept;

private:
    friend class stop_callback_node;

    static constexpr std::uint8_t source_flag = 1;
    static constexpr std::uint8_t requested_flag = 2;

    stop_state() noexcept = default;
    ~stop_state() = default;

    void destroy() noexcept;
    // Returns false, linking nothing, when stop was already requested.
    [[nodiscard]] bool link(stop_callback_node& node) noexcept;
    void unlink(stop_callback_node& node) noexcept;

    std::atomic<std::uint8_t> flags_{source_flag};
    std::atomic<std::size_t> references_{1};
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

    // Callers only wrap a non-empty callback: registration returns early on an
    // empty one, and invoke() runs the stored callback at most once.
    void operator()() noexcept {
        try {
            callback_();
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

    [[nodiscard]] std::optional<stop_callback> take() noexcept {
        if (invoked_.test_and_set(std::memory_order_acq_rel)) {
            return std::nullopt;
        }
        return stop_callback(std::move(callback_));
    }

    void invoke() noexcept {
        if (auto callback = take()) {
            (*callback)();
        }
    }

private:
    std::atomic_flag invoked_ = ATOMIC_FLAG_INIT;
    move_only_function<void()> callback_;
};

// One registration's link into one stop state. Construction records whether
// it linked before stop; the registration invokes an already-stopped callback.
// Destruction unlinks it; if the callback is running on another thread, it
// waits for that run to return, so the callback state can be released next.
class stop_callback_node final {
public:
    stop_callback_node(const stop_state_ref& state_value, stop_callback_state& callback) noexcept
        : state_(state_value),
          callback_(&callback),
          linked_at_registration_(state_->link(*this)) {}

    [[nodiscard]] bool linked_at_registration() const noexcept {
        return linked_at_registration_;
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
    // Initialize last: link() publishes every field request_stop() may access.
    // Its result stays stable even if another thread immediately unlinks us.
    const bool linked_at_registration_;
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

    stop_registration(const detail::stop_state_ref& state, move_only_function<void()> callback_value) {
        register_callback(state, std::move(callback_value));
    }

    void register_callback(const detail::stop_state_ref& state, move_only_function<void()> callback_value) {
        if (!callback_value || !state || !state->stop_possible()) {
            return;
        }
        if (state->stop_requested()) {
            detail::stop_callback(std::move(callback_value))();
            return;
        }

        auto expected = registration_phase_type::idle;
        if (!phase_.compare_exchange_strong(expected, registration_phase_type::registering,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::terminate();
        }
        callback_state_.emplace(std::move(callback_value));
        callback_.emplace(state, *callback_state_);
        if (!callback_->linked_at_registration()) {
            callback_.reset();
            auto callback = callback_state_->take();
            callback_state_.reset();
            const auto previous_phase =
                phase_.exchange(registration_phase_type::idle, std::memory_order_acq_rel);
            if (previous_phase != registration_phase_type::reset_pending && callback.has_value()) {
                (*callback)();
            }
            return;
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

    // Unlink waits for an invocation on another thread before retiring its state.
    void clear_registration() noexcept {
        callback_.reset();
        callback_state_.reset();
    }

    std::atomic<registration_phase_type> phase_{registration_phase_type::idle};
    std::optional<detail::stop_callback_state> callback_state_;
    std::optional<detail::stop_callback_node> callback_;
};

// A copyable reference to one cancellation state. Combining cancellation requires
// an explicit address-stable stop_group owner, not additional state in a token.
class stop_token final {
public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return state_ && state_->stop_possible();
    }

    // Distinguishes an unspecified token from a state whose source has retired.
    [[nodiscard]] bool has_state() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] friend bool operator==(const stop_token&, const stop_token&) noexcept = default;

    // The registration embeds one intrusive node; registration adds no allocation.
    // Registration may race with request_stop(). If stop was already requested,
    // the callback runs synchronously before registration returns.
    // request_stop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    [[nodiscard]] stop_registration register_callback(move_only_function<void()> callback_value) const {
        return stop_registration(state_, std::move(callback_value));
    }

    // Reuses caller-owned registration storage, replacing any earlier
    // registration. This is for awaiters that can only form their cancellation
    // callback after an operation has published its id/generation in
    // await_suspend; it preserves the embedded, zero-allocation node
    // representation.
    void register_callback(stop_registration& registration, move_only_function<void()> callback_value) const {
        registration.reset();
        registration.register_callback(state_, std::move(callback_value));
    }

private:
    friend class stop_source;
    friend class stop_group;

    explicit stop_token(detail::stop_state_ref state_value) noexcept
        : state_(std::move(state_value)) {}

    detail::stop_state_ref state_;
};

// Owns one stop state and requests its stop. The state comes from
// the process-lifetime mimalloc resource and lives until this source and every
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
        state_value->request_stop();
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

// Address-stable owner of parent registrations and one downstream cancellation
// state. Keep it alive until all operations using token() have completed/joined.
// Zero/one distinct stoppable parent is passed through without a new state.
// Multiple parents share one output state, so duplicate/nested sources notify
// each downstream registration once. Only parent counts above two allocate
// registration storage; ordinary stop_registration always contains one node.
// Destruction first unregisters parents (waiting for callbacks on other threads).
class stop_group final {
public:
    explicit stop_group(std::span<const stop_token> parents);
    stop_group(std::initializer_list<stop_token> parents)
        : stop_group(std::span<const stop_token>(parents.begin(), parents.size())) {}
    stop_group(stop_token first, stop_token second)
        : stop_group({std::move(first), std::move(second)}) {}
    ~stop_group() = default;

    stop_group(const stop_group&) = delete;
    stop_group& operator=(const stop_group&) = delete;
    stop_group(stop_group&&) = delete;
    stop_group& operator=(stop_group&&) = delete;

    [[nodiscard]] stop_token token() const noexcept {
        return output_;
    }

private:
    std::optional<stop_source> source_;
    stop_token output_;
    std::array<stop_registration, 2> parents_;
    std::pmr::vector<stop_registration> extra_parents_;
};

}  // namespace ruvia
