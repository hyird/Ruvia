#include "ruvia/web/detail/client/HttpClientResponseMemory.h"

#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"

namespace ruvia::detail {

HttpClientResponseMemoryDomain::HttpClientResponseMemoryDomain(const WorkerHandle& worker,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain,
    std::pmr::memory_resource& upstream)
    : memory_(upstream),
      worker_(worker),
      resultBudgetDomain_(resultBudgetDomain) {
    if (!worker_.valid() || !resultBudgetDomain_) {
        throw std::invalid_argument("HTTP client response memory requires worker and result budget");
    }
}

HttpClientResponseMemoryDomain::~HttpClientResponseMemoryDomain() {
    if (phase_ != Phase::kPrepared) {
        requireCurrent();
    }
    if (stateHead_ != nullptr || references_ != 0) {
        std::terminate();
    }
}

HttpClientResponseMemoryDomain::Owner HttpClientResponseMemoryDomain::create(
    const WorkerHandle& worker,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain) {
    return create(worker, resultBudgetDomain, *processResource());
}

HttpClientResponseMemoryDomain::Owner HttpClientResponseMemoryDomain::create(
    const WorkerHandle& worker,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain,
    std::pmr::memory_resource& upstream) {
    auto* const resource = processResource();
    auto* const storage = resource->allocate(sizeof(HttpClientResponseMemoryDomain),
        alignof(HttpClientResponseMemoryDomain));
    try {
        return Owner(::new (storage) HttpClientResponseMemoryDomain(
                         worker, resultBudgetDomain, upstream),
            Deleter{});
    } catch (...) {
        resource->deallocate(storage, sizeof(HttpClientResponseMemoryDomain),
            alignof(HttpClientResponseMemoryDomain));
        throw;
    }
}

void HttpClientResponseMemoryDomain::Deleter::operator()(
    HttpClientResponseMemoryDomain* domain) const noexcept {
    if (domain != nullptr) {
        domain->releaseRoot();
    }
}

void HttpClientResponseMemoryDomain::requireCurrent() const noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
}

void HttpClientResponseMemoryDomain::retain() noexcept {
    requireCurrent();
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void HttpClientResponseMemoryDomain::release() noexcept {
    requireCurrent();
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        auto* const resource = processResource();
        this->~HttpClientResponseMemoryDomain();
        resource->deallocate(this, sizeof(HttpClientResponseMemoryDomain),
            alignof(HttpClientResponseMemoryDomain));
    }
}

void HttpClientResponseMemoryDomain::releaseRoot() noexcept {
    // Startup rollback has never published worker-affine response storage.
    // No worker scheduling is necessary (or possible after failed preparation).
    if (phase_ == Phase::kPrepared) {
        if (references_ != 1 || stateHead_ != nullptr) {
            std::terminate();
        }
        references_ = 0;
        auto* const resource = processResource();
        this->~HttpClientResponseMemoryDomain();
        resource->deallocate(this, sizeof(HttpClientResponseMemoryDomain),
            alignof(HttpClientResponseMemoryDomain));
        return;
    }
    // Active storage is retired explicitly by the pool on its owning worker;
    // do not conceal a missing join with unstructured deferred destruction.
    release();
}

HttpClientResponseState* HttpClientResponseMemoryDomain::createState(HttpClientPool& pool) {
    requireCurrent();
    if (phase_ == Phase::kRetired) {
        throw std::logic_error("HTTP client response transport is retired");
    }
    phase_ = Phase::kActive;
    auto* state = constructPmrObject<HttpClientResponseState>(
        resource(), *this);
    state->pool = &pool;
    state->resultBudgetDomain = &resultBudgetDomain_;
    attachState(*state);
    return state;
}

void HttpClientResponseMemoryDomain::attachState(HttpClientResponseState& state) noexcept {
    requireCurrent();
    state.nextMemoryState_ = stateHead_;
    if (stateHead_ != nullptr) {
        stateHead_->previousMemoryState_ = &state;
    }
    stateHead_ = &state;
    retain();
}

void HttpClientResponseMemoryDomain::destroyState(HttpClientResponseState* state) noexcept {
    requireCurrent();
    if (state == nullptr || state->memoryDomain_ != this) {
        std::terminate();
    }
    if (state->pool != nullptr) {
        unlinkState(*state);
    } else if (state->previousMemoryState_ != nullptr || state->nextMemoryState_ != nullptr) {
        std::terminate();
    }
    destroyPmrObject(state, resource());
    release();
}

void HttpClientResponseMemoryDomain::unlinkState(HttpClientResponseState& state) noexcept {
    if (state.previousMemoryState_ != nullptr) {
        state.previousMemoryState_->nextMemoryState_ = state.nextMemoryState_;
    } else if (stateHead_ == &state) {
        stateHead_ = state.nextMemoryState_;
    } else {
        std::terminate();
    }
    if (state.nextMemoryState_ != nullptr) {
        state.nextMemoryState_->previousMemoryState_ = state.previousMemoryState_;
    }
    state.previousMemoryState_ = nullptr;
    state.nextMemoryState_ = nullptr;
}

void HttpClientResponseMemoryDomain::detachTransportBindings(HttpClientPool& pool) noexcept {
    requireCurrent();
    phase_ = Phase::kRetired;
    while (stateHead_ != nullptr) {
        auto* state = stateHead_;
        if (state->pool != &pool) {
            std::terminate();
        }
        // Revoke registration before notifying consumers. A continuation may
        // release this state or a sibling; never cache a next pointer across it.
        unlinkState(*state);
        state->detachTransportBindings();
    }
}

HttpClientResponseState::HttpClientResponseState(HttpClientResponseMemoryDomain& memoryDomain)
    : HttpClientResponseState(memoryDomain.worker(), memoryDomain.resource()) {
    memoryDomain_ = &memoryDomain;
}

void HttpClientResponseState::detachTransportBindings() noexcept {
    if (http2DataCredit) {
        http2DataCredit.reset();
    }
    http3BodyBudget.reset();
    http3Connection = nullptr;
    http3RequestId = 0;
    connectionIndex = 0;
    requestId = 0;
    cancellationId = 0;
    streamId = 0;
    pool = nullptr;
    transport = HttpClientResponseTransport::kUnassigned;
    if (!complete) {
        errorCode = static_cast<std::uint8_t>(HttpClientError::Code::kClosing);
        complete = true;
    }
    headSignal.notify();
    dataSignal.notify();
    spaceSignal.notify();
}

}  // namespace ruvia::detail
