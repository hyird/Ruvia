#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include "ruvia/core/WorkerHandle.h"

namespace ruvia::detail {

// Queued callbacks retain this mailbox rather than the pool. The pool detaches
// itself before tearing down worker-owned state, so both drained and abandoned
// queue entries can safely release the mailbox after the pool is gone.
template <typename Owner>
class WorkerCancellationMailbox final
    : public std::enable_shared_from_this<WorkerCancellationMailbox<Owner>> {
public:
    WorkerCancellationMailbox(Owner& owner, const WorkerHandle& worker) noexcept
        : owner_(&owner),
          worker_(worker) {}

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    // Operation ids are created and consumed on the bound worker. Zero stays
    // reserved for "no cancellable operation", so stale queued callbacks can
    // be rejected by comparing one scalar in the owner.
    [[nodiscard]] std::uint64_t nextOperationId() noexcept {
        if (!worker_.isCurrent()) {
            std::terminate();
        }
        if (++nextOperationId_ == 0) {
            ++nextOperationId_;
        }
        return nextOperationId_;
    }

    [[nodiscard]] std::shared_ptr<WorkerCancellationMailbox> retain() noexcept {
        auto mailbox = this->weak_from_this().lock();
        if (mailbox == nullptr) {
            std::terminate();
        }
        return mailbox;
    }

    void dispatch(std::uint64_t operationId) noexcept {
        if (auto* owner = owner_.load(std::memory_order_acquire); owner != nullptr) {
            owner->cancelOperationById(operationId);
        }
    }

    void detach(Owner& owner) noexcept {
        auto* previous = owner_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous != nullptr && previous != &owner) {
            std::terminate();
        }
    }

private:
    std::atomic<Owner*> owner_;
    WorkerHandle worker_;
    std::uint64_t nextOperationId_{0};
};

// A registered stop callback borrows the pool-owned mailbox and carries only an
// opaque operation id. If cancellation actually happens, it retains the
// mailbox for the queued dispatch. Both functors fit MoveOnlyFunction's inline
// storage, so neither registration nor queueing allocates callback state.
template <typename Mailbox>
class WorkerCancellationDispatch final {
public:
    WorkerCancellationDispatch(std::shared_ptr<Mailbox> mailbox, std::uint64_t operationId) noexcept
        : mailbox_(std::move(mailbox)),
          operationId_(operationId) {}

    void operator()() noexcept {
        mailbox_->dispatch(operationId_);
    }

private:
    std::shared_ptr<Mailbox> mailbox_;
    std::uint64_t operationId_;
};

template <typename Mailbox>
class WorkerCancellationPost final {
public:
    WorkerCancellationPost(
        const std::shared_ptr<Mailbox>& mailbox, std::uint64_t operationId) noexcept
        : mailbox_(mailbox.get()),
          operationId_(operationId) {
        if (mailbox_ == nullptr) {
            std::terminate();
        }
    }

    void operator()() noexcept {
        if (mailbox_->worker().isCurrent()) {
            mailbox_->dispatch(operationId_);
            return;
        }
        auto mailbox = mailbox_->retain();
        const auto& worker = mailbox->worker();
        WorkerCancellationDispatch<Mailbox> dispatch(std::move(mailbox), operationId_);
        WorkerHandleAccess::deferOrTerminate(worker, std::move(dispatch));
    }

private:
    Mailbox* mailbox_;
    std::uint64_t operationId_;
};

template <typename Mailbox>
inline constexpr bool workerCancellationPostIsInline =
    sizeof(WorkerCancellationPost<Mailbox>) <= 3 * sizeof(void*) &&
    sizeof(WorkerCancellationDispatch<Mailbox>) <= 3 * sizeof(void*) &&
    alignof(WorkerCancellationPost<Mailbox>) <= alignof(std::max_align_t) &&
    alignof(WorkerCancellationDispatch<Mailbox>) <= alignof(std::max_align_t) &&
    std::is_nothrow_move_constructible_v<WorkerCancellationPost<Mailbox>> &&
    std::is_nothrow_move_constructible_v<WorkerCancellationDispatch<Mailbox>>;

}  // namespace ruvia::detail
