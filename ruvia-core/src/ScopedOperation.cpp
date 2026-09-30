#include "ruvia/core/ScopedOperation.h"

#include <exception>

namespace ruvia::detail {

// A stack-owned intrusive obligation keeps a reentrant join from publishing
// retirement while a scope is still draining frames or visiting capabilities.
struct ScopedOperationScope::DrainGuard final : ScopedOperationNode {
    explicit DrainGuard(ScopedOperationScope& scope) noexcept
        : ScopedOperationNode(scope) {
        if (scope_ == nullptr) {
            scope.link(*this);
        }
        phase_ = Phase::kRetiring;
    }

    ~DrainGuard() {
        auto* scope = scope_;
        phase_ = Phase::kExpired;
        scope->unlink(*this);
        // This is the final scope access: join may retire its owner inline.
        scope->resumeJoiner();
    }
};

struct ScopedOperationScope::JoinAwaiter final {
    explicit JoinAwaiter(ScopedOperationScope& owner) noexcept
        : owner(owner) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return owner.head_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (owner.head_ == nullptr || owner.joinContinuation_ != nullptr) {
            std::terminate();
        }
        owner.joinContinuation_ = continuation;
        return true;
    }

    void await_resume() const noexcept {}

    ScopedOperationScope& owner;
};

void ScopedOperationScope::close() noexcept {
    if (head_ == nullptr && capabilityHead_ == nullptr) {
        active_ = false;
        return;
    }
    for (auto* operation = head_; operation != nullptr; operation = operation->next_) {
        if (operation->phase_ == ScopedOperationNode::Phase::kRunning ||
            operation->phase_ == ScopedOperationNode::Phase::kRetiring) {
            std::terminate();
        }
    }
    DrainGuard drain(*this);
    active_ = false;
    retireColdOperations();
    expireCapabilities();
}

Task<void> ScopedOperationScope::closeAndJoin() & {
    if (joinStarted_) {
        if (!joinComplete_) {
            throw std::logic_error("scoped operation scope can only be joined once");
        }
        co_return;
    }
    joinStarted_ = true;
    expireForJoin();
    if (head_ == nullptr) {
        expireCapabilities();
        joinComplete_ = true;
        co_return;
    }
    co_await JoinAwaiter(*this);
}

void ScopedOperationScope::retireColdOperations() noexcept {
    for (;;) {
        auto* operation = head_;
        while (operation != nullptr && operation->phase_ != ScopedOperationNode::Phase::kCold) {
            operation = operation->next_;
        }
        if (operation == nullptr) {
            return;
        }
        operation->retireFrame();
        // A frame destructor can remove other cold nodes. Never carry a raw
        // traversal cursor across cleanup; search the live chain again.
    }
}

void ScopedOperationScope::expireForJoin() noexcept {
    DrainGuard drain(*this);
    active_ = false;
    retireColdOperations();
}

void ScopedOperationScope::expireCapabilities() noexcept {
    while (capabilityHead_ != nullptr) {
        capabilityHead_->expire();
    }
}

void ScopedOperationScope::resumeJoiner() noexcept {
    if (head_ != nullptr || joinContinuation_ == nullptr) {
        return;
    }
    expireCapabilities();
    auto continuation = std::exchange(joinContinuation_, {});
    joinComplete_ = true;
    continuation.resume();
}

ScopedCapabilityNode::ScopedCapabilityNode(
    ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept
    : expire_(expire) {
    if (scope.active()) {
        link(scope);
    } else {
        active_ = false;
    }
}

ScopedCapabilityNode::ScopedCapabilityNode(const ScopedCapabilityNode& other) noexcept
    : expire_(other.expire_),
      active_(other.active_) {
    if (active_ && other.scope_ != nullptr) {
        link(*other.scope_);
    }
}

ScopedCapabilityNode::ScopedCapabilityNode(ScopedCapabilityNode&& other) noexcept
    : scope_(other.scope_),
      previous_(other.previous_),
      next_(other.next_),
      expire_(other.expire_),
      active_(other.active_) {
    if (scope_ != nullptr) {
        if (previous_ != nullptr) {
            previous_->next_ = this;
        } else {
            scope_->capabilityHead_ = this;
        }
        if (next_ != nullptr) {
            next_->previous_ = this;
        }
    }
    other.scope_ = nullptr;
    other.previous_ = nullptr;
    other.next_ = nullptr;
    other.active_ = false;
}

ScopedCapabilityNode::~ScopedCapabilityNode() {
    unlink();
}

void ScopedCapabilityNode::requireActive() const {
    if (!active_) {
        throw std::logic_error("scoped capability lifetime has expired");
    }
}

ScopedOperationScope& ScopedCapabilityNode::operationScope() const {
    requireActive();
    return *scope_;
}

void ScopedCapabilityNode::bind(
    ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept {
    if (scope_ != nullptr || active_) {
        std::terminate();
    }
    expire_ = expire;
    active_ = scope.active();
    if (active_) {
        link(scope);
    }
}

void ScopedCapabilityNode::link(ScopedOperationScope& scope) noexcept {
    scope_ = &scope;
    next_ = scope.capabilityHead_;
    if (next_ != nullptr) {
        next_->previous_ = this;
    }
    scope.capabilityHead_ = this;
}

void ScopedCapabilityNode::unlink() noexcept {
    if (scope_ == nullptr) {
        return;
    }
    if (previous_ != nullptr) {
        previous_->next_ = next_;
    } else {
        scope_->capabilityHead_ = next_;
    }
    if (next_ != nullptr) {
        next_->previous_ = previous_;
    }
    scope_ = nullptr;
    previous_ = nullptr;
    next_ = nullptr;
}

void ScopedCapabilityNode::expire() noexcept {
    if (expire_ != nullptr) {
        expire_(*this);
    }
    active_ = false;
    unlink();
}

void ScopedOperationScope::link(ScopedOperationNode& operation) noexcept {
    operation.scope_ = this;
    operation.next_ = head_;
    if (head_ != nullptr) {
        head_->previous_ = &operation;
    }
    head_ = &operation;
}

void ScopedOperationScope::unlink(ScopedOperationNode& operation) noexcept {
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

ScopedOperationNode::ScopedOperationNode(ScopedOperationScope& scope) noexcept {
    if (scope.active()) {
        scope.link(*this);
    } else {
        phase_ = Phase::kExpired;
    }
}

ScopedOperationNode::~ScopedOperationNode() {
    if (scope_ != nullptr || phase_ == Phase::kRunning || phase_ == Phase::kRetiring) {
        std::terminate();
    }
}

void ScopedOperationNode::bindFrame(void (*retireCold)(ScopedOperationNode&) noexcept,
    void (*checkAffinity)(void*) noexcept, void* affinityTarget) noexcept {
    if (retireCold == nullptr || retireCold_ != nullptr) {
        std::terminate();
    }
    if (phase_ == Phase::kExpired) {
        phase_ = Phase::kRetiring;
        retireCold(*this);
        phase_ = Phase::kExpired;
        return;
    }
    if (phase_ != Phase::kCold) {
        std::terminate();
    }
    retireCold_ = retireCold;
    checkAffinity_ = checkAffinity;
    affinityTarget_ = affinityTarget;
}

void ScopedOperationNode::clearFrameBinding() noexcept {
    retireCold_ = nullptr;
    checkAffinity_ = nullptr;
    affinityTarget_ = nullptr;
}

void ScopedOperationNode::begin() {
    if (phase_ == Phase::kExpired) {
        throw std::logic_error("capability operation scope has expired");
    }
    if (phase_ != Phase::kCold) {
        throw std::logic_error("capability operation can only be awaited once");
    }
    if (checkAffinity_ != nullptr) {
        checkAffinity_(affinityTarget_);
    }
    phase_ = Phase::kRunning;
}

void ScopedOperationNode::prepareCompletion() const noexcept {
    if (phase_ != Phase::kRunning) {
        std::terminate();
    }
    if (checkAffinity_ != nullptr) {
        checkAffinity_(affinityTarget_);
    }
}

void ScopedOperationNode::complete() noexcept {
    if (phase_ != Phase::kRunning) {
        std::terminate();
    }
    phase_ = Phase::kComplete;
    clearFrameBinding();
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        scope->resumeJoiner();
    }
}

void ScopedOperationNode::retireFrame() noexcept {
    if (phase_ == Phase::kRunning || phase_ == Phase::kRetiring) {
        std::terminate();
    }
    if (phase_ == Phase::kCold) {
        if (checkAffinity_ != nullptr) {
            checkAffinity_(affinityTarget_);
        }
        phase_ = Phase::kRetiring;
        const auto retireCold = retireCold_;
        clearFrameBinding();
        retireCold(*this);
    }
    phase_ = Phase::kExpired;
    if (scope_ != nullptr) {
        auto* scope = scope_;
        scope->unlink(*this);
        // No owner/frame access is allowed after this publication point.
        scope->resumeJoiner();
    }
}

}  // namespace ruvia::detail
