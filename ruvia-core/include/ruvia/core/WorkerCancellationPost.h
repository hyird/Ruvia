#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {

// Owns only a detachable owner pointer and a stable worker handle; queued work
// retains the mailbox so that releasing it never depends on the owner's lifetime.
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
        if (!mailbox) {
            std::terminate();
        }
        return mailbox;
    }
    void dispatch(std::uint64_t operationId) noexcept {
        if (auto* owner = owner_.load(std::memory_order_acquire)) {
            owner->cancelOperationById(operationId);
        }
    }
    void detach(Owner& owner) noexcept {
        auto* previous = owner_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous && previous != &owner) {
            std::terminate();
        }
    }

private:
    std::atomic<Owner*> owner_;
    WorkerHandle worker_;
    std::uint64_t nextOperationId_{0};
};

template <typename Owner>
[[nodiscard]] inline std::shared_ptr<WorkerCancellationMailbox<Owner>>
makeWorkerCancellationMailbox(Owner& owner, const WorkerHandle& worker) {
    using Mailbox = WorkerCancellationMailbox<Owner>;
    return std::allocate_shared<Mailbox>(
        std::pmr::polymorphic_allocator<Mailbox>(detail::processResource()), owner, worker);
}

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
    WorkerCancellationPost(const std::shared_ptr<Mailbox>& mailbox, std::uint64_t operationId) noexcept
        : mailbox_(mailbox.get()),
          operationId_(operationId) {
        if (!mailbox_) {
            std::terminate();
        }
    }
    void operator()() noexcept {
        if (mailbox_->worker().isCurrent()) {
            mailbox_->dispatch(operationId_);
            return;
        }
        auto retained = mailbox_->retain();
        auto result = retained->worker().post(WorkerCancellationDispatch<Mailbox>(retained, operationId_));
        if (result.status() != PostStatus::kAccepted) {
            std::terminate();
        }
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

}  // namespace ruvia
