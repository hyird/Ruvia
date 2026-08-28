#include "ruvia/core/ScopedOperation.h"

#include <exception>

namespace ruvia::detail {

void ScopedOperationScope::close() noexcept {
    active_ = false;
    while (head_ != nullptr) {
        auto* operation = head_;
        if (operation->phase_ == ScopedOperationNode::Phase::kRunning) {
            std::terminate();
        }
        operation->expire();
    }
    while (capabilityHead_ != nullptr) {
        capabilityHead_->expire();
    }
}

ScopedCapabilityNode::ScopedCapabilityNode(ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept
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

void ScopedCapabilityNode::bind(ScopedOperationScope& scope, void (*expire)(ScopedCapabilityNode&) noexcept) noexcept {
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
    if (phase_ == Phase::kRunning) {
        std::terminate();
    }
    if (scope_ != nullptr) {
        scope_->unlink(*this);
    }
}

void ScopedOperationNode::begin() {
    if (phase_ == Phase::kExpired) {
        throw std::logic_error("capability operation scope has expired");
    }
    if (phase_ != Phase::kCold) {
        throw std::logic_error("capability operation can only be awaited once");
    }
    phase_ = Phase::kRunning;
}

void ScopedOperationNode::complete() noexcept {
    if (phase_ != Phase::kRunning) {
        std::terminate();
    }
    phase_ = Phase::kComplete;
    if (scope_ != nullptr) {
        scope_->unlink(*this);
    }
}

void ScopedOperationNode::expire() noexcept {
    if (phase_ == Phase::kRunning) {
        std::terminate();
    }
    if (phase_ == Phase::kCold && expireCold_ != nullptr) {
        expireCold_(*this);
    }
    phase_ = Phase::kExpired;
    if (scope_ != nullptr) {
        scope_->unlink(*this);
    }
}

}  // namespace ruvia::detail
