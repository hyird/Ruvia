#include "ruvia/web/ScopedOperation.h"

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
    phase_ = Phase::kExpired;
    if (scope_ != nullptr) {
        scope_->unlink(*this);
    }
}

}  // namespace ruvia::detail
