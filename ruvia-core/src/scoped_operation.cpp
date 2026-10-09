#include "ruvia/core/scoped_operation.h"

#include <exception>

namespace ruvia {

// Keeps a reentrant join from publishing retirement during frame/capability
// cleanup. Its registration is a value, not a frame-owner base class.
struct operation_scope::drain_guard final {
    explicit drain_guard(operation_scope& scope) noexcept
        : registration_(scope) {
        if (registration_.scope_ == nullptr) {
            scope.link(registration_);
        }
        registration_.phase_ = detail::scoped_operation_registration::phase::retiring;
    }

    ~drain_guard() {
        auto* scope = registration_.scope_;
        registration_.phase_ = detail::scoped_operation_registration::phase::expired;
        scope->unlink(registration_);
        // Join may retire this scope's owner inline. No further scope access.
        scope->resume_joiner();
    }

    detail::scoped_operation_registration registration_;
};

struct operation_scope::join_awaiter final {
    explicit join_awaiter(operation_scope& scope) noexcept
        : owner_(scope) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return owner_.head_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (owner_.head_ == nullptr || owner_.join_continuation_ != nullptr) {
            std::terminate();
        }
        owner_.join_continuation_ = continuation;
        return true;
    }

    void await_resume() const noexcept {}

    operation_scope& owner_;
};

void operation_scope::close() noexcept {
    if (head_ == nullptr && capability_head_ == nullptr) {
        active_ = false;
        return;
    }
    for (auto* operation = head_; operation != nullptr; operation = operation->next_) {
        if (operation->phase_ == detail::scoped_operation_registration::phase::running ||
            operation->phase_ == detail::scoped_operation_registration::phase::retiring) {
            std::terminate();
        }
    }
    drain_guard drain(*this);
    active_ = false;
    retire_cold_operations();
    expire_capabilities();
}

task<void> operation_scope::close_and_join() & {
    if (join_started_) {
        if (!join_complete_) {
            throw std::logic_error("scoped operation scope can only be joined once");
        }
        co_return;
    }
    join_started_ = true;
    expire_for_join();
    if (head_ == nullptr) {
        expire_capabilities();
        join_complete_ = true;
        co_return;
    }
    co_await join_awaiter(*this);
}

void operation_scope::retire_cold_operations() noexcept {
    for (;;) {
        auto* operation = head_;
        while (operation != nullptr && operation->phase_ != detail::scoped_operation_registration::phase::cold) {
            operation = operation->next_;
        }
        if (operation == nullptr) {
            return;
        }
        operation->retire_frame();
        // Cleanup may remove sibling cold frames; search the live chain again.
    }
}

void operation_scope::expire_for_join() noexcept {
    drain_guard drain(*this);
    active_ = false;
    retire_cold_operations();
}

void operation_scope::expire_capabilities() noexcept {
    while (capability_head_ != nullptr) {
        capability_head_->expire();
    }
}

void operation_scope::resume_joiner() noexcept {
    if (head_ != nullptr || join_continuation_ == nullptr) {
        return;
    }
    expire_capabilities();
    auto continuation = std::exchange(join_continuation_, {});
    join_complete_ = true;
    continuation.resume();
}

scoped_capability_registration::scoped_capability_registration(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept
    : target_(target),
      cleanup_(cleanup),
      active_(scope.active()) {
    if (active_) {
        link(scope);
    }
}

scoped_capability_registration::scoped_capability_registration(
    const scoped_capability_registration& other, void* target) noexcept
    : target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (active_ && other.scope_ != nullptr) {
        link(*other.scope_);
    }
}

scoped_capability_registration::scoped_capability_registration(
    scoped_capability_registration&& other, void* target) noexcept
    : scope_(other.scope_),
      previous_(other.previous_),
      next_(other.next_),
      target_(target),
      cleanup_(other.cleanup_),
      active_(other.active_) {
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->capability_head_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.target_ = nullptr;
    other.cleanup_ = nullptr;
    other.active_ = false;
}

scoped_capability_registration::~scoped_capability_registration() {
    unlink();
}

void scoped_capability_registration::require_active() const {
    if (!active_) {
        throw std::logic_error("scoped capability lifetime has expired");
    }
}

operation_scope& scoped_capability_registration::scope() const {
    require_active();
    return *scope_;
}

void scoped_capability_registration::bind(
    operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept {
    if (scope_ != nullptr || active_) {
        std::terminate();
    }
    target_ = target;
    cleanup_ = cleanup;
    active_ = scope.active();
    if (active_) {
        link(scope);
    }
}

void scoped_capability_registration::link(operation_scope& scope) noexcept {
    scope_ = &scope;
    next_ = scope.capability_head_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    scope.capability_head_ = this;
}

void scoped_capability_registration::unlink() noexcept {
    if (scope_ == nullptr) {
        return;
    }
    if (previous_ != nullptr) {
        previous_->next_ = next_;
    } else {
        scope_->capability_head_ = next_;
    }
    if (next_ != nullptr) {
        next_->previous_ = previous_;
    }
    scope_ = nullptr;
    previous_ = nullptr;
    next_ = nullptr;
}

void scoped_capability_registration::expire() noexcept {
    const auto cleanup = cleanup_;
    auto* target = target_;
    active_ = false;
    unlink();
    // Cleanup may destroy this registration or remove sibling capabilities.
    // Never access the registration after calling its typed owner.
    if (cleanup != nullptr) {
        cleanup(target);
    }
}

void operation_scope::link(detail::scoped_operation_registration& operation) noexcept {
    operation.scope_ = this;
    operation.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &operation;
    }
    head_ = &operation;
}

void operation_scope::unlink(detail::scoped_operation_registration& operation) noexcept {
    if (operation.previous_ != nullptr) {
        operation.previous_->next_ = operation.next_;
    } else if (head_ == &operation) {
        head_ = operation.next_;
    }
    if (operation.next_ != nullptr) {
        operation.next_->previous_ = operation.previous_;
    }
    operation.scope_ = nullptr;
    operation.previous_ = nullptr;
    operation.next_ = nullptr;
}

namespace detail {

scoped_operation_registration::scoped_operation_registration(operation_scope& scope) noexcept {
    if (scope.active()) {
        scope.link(*this);
    } else {
        phase_ = phase::expired;
    }
}

scoped_operation_registration::~scoped_operation_registration() {
    if (scope_ != nullptr || phase_ == phase::running || phase_ == phase::retiring) {
        std::terminate();
    }
}

void scoped_operation_registration::bind_frame(void* target, void (*retire_cold)(void*) noexcept,
    void (*check_affinity)(void*) noexcept, void* affinity_target) noexcept {
    if (retire_cold == nullptr || retire_cold_ != nullptr) {
        std::terminate();
    }
    if (phase_ == phase::expired) {
        phase_ = phase::retiring;
        retire_cold(target);
        phase_ = phase::expired;
        return;
    }
    if (phase_ != phase::cold) {
        std::terminate();
    }
    frame_target_ = target;
    retire_cold_ = retire_cold;
    check_affinity_ = check_affinity;
    affinity_target_ = affinity_target;
}

void scoped_operation_registration::clear_frame_binding() noexcept {
    frame_target_ = nullptr;
    retire_cold_ = nullptr;
    check_affinity_ = nullptr;
    affinity_target_ = nullptr;
}

void scoped_operation_registration::begin() {
    if (phase_ == phase::expired) {
        throw std::logic_error("capability operation scope has expired");
    }
    if (phase_ != phase::cold) {
        throw std::logic_error("capability operation can only be awaited once");
    }
    if (check_affinity_ != nullptr) {
        check_affinity_(affinity_target_);
    }
    phase_ = phase::running;
}

void scoped_operation_registration::prepare_completion() const noexcept {
    if (phase_ != phase::running) {
        std::terminate();
    }
    if (check_affinity_ != nullptr) {
        check_affinity_(affinity_target_);
    }
}

void scoped_operation_registration::complete() noexcept {
    if (phase_ != phase::running) {
        std::terminate();
    }
    phase_ = phase::complete;
    clear_frame_binding();
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        scope->resume_joiner();
    }
}

void scoped_operation_registration::retire_frame() noexcept {
    if (phase_ == phase::running || phase_ == phase::retiring) {
        std::terminate();
    }
    if (phase_ == phase::cold) {
        if (check_affinity_ != nullptr) {
            check_affinity_(affinity_target_);
        }
        phase_ = phase::retiring;
        const auto retire_cold = retire_cold_;
        auto* target = frame_target_;
        clear_frame_binding();
        retire_cold(target);
    }
    phase_ = phase::expired;
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        // Join may retire the owner inline; no further registration access.
        scope->resume_joiner();
    }
}

}  // namespace detail
}  // namespace ruvia
