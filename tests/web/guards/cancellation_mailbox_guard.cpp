#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>

#include <asio/io_context.hpp>

#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/integration/WorkerCancellationPost.h"

namespace {

std::atomic_size_t allocationCount{0};

struct CancellationOwner final {
    void cancelOperationById(std::uint64_t operationId) noexcept {
        lastOperationId = operationId;
    }

    std::uint64_t lastOperationId{0};
};

using CancellationMailbox = ruvia::detail::WorkerCancellationMailbox<CancellationOwner>;

static_assert(ruvia::detail::workerCancellationPostIsInline<CancellationMailbox>);

}  // namespace

void* operator new(std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    asio::io_context ioContext;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    CancellationOwner owner;
    auto mailbox = std::make_shared<CancellationMailbox>(owner, worker);
    ruvia::StopSource source;
    ruvia::StopRegistration registration;

    const auto allocationsBeforeRegistration = allocationCount.load(std::memory_order_relaxed);
    const auto ownersBeforeRegistration = mailbox.use_count();
    source.token().registerCallback(
        registration,
        ruvia::detail::WorkerCancellationPost<CancellationMailbox>(mailbox, 41));
    assert(mailbox.use_count() == ownersBeforeRegistration);
    ruvia::MoveOnlyFunction<void()> queuedDispatch(
        ruvia::detail::WorkerCancellationDispatch<CancellationMailbox>(mailbox, 42));
    assert(allocationCount.load(std::memory_order_relaxed) == allocationsBeforeRegistration);
    registration.reset();

    ruvia::detail::WorkerCancellationPost<CancellationMailbox>(mailbox, 41)();
    ioContext.run();
    assert(owner.lastOperationId == 41);

    queuedDispatch();
    assert(owner.lastOperationId == 42);

    ruvia::StopSource onWorkerSource;
    ruvia::StopRegistration onWorkerRegistration;
    onWorkerSource.token().registerCallback(
        onWorkerRegistration,
        ruvia::detail::WorkerCancellationPost<CancellationMailbox>(mailbox, 44));
    bool observedInline = false;
    ruvia::detail::WorkerHandleAccess::defer(worker, [&] {
        onWorkerSource.requestStop();
        observedInline = owner.lastOperationId == 44;
        ioContext.stop();
    });
    ioContext.restart();
    dispatcher->runContext();
    assert(observedInline);

    mailbox->detach(owner);
    ruvia::detail::WorkerCancellationDispatch<CancellationMailbox>(mailbox, 43)();
    assert(owner.lastOperationId == 44);
    dispatcher->detachContext();
    return 0;
}
