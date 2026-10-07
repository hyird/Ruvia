#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/RuntimeLifecycle.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerCancellationPost.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/detail/worker/WorkerSelection.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t deallocations{};

    [[nodiscard]] bool owns(const void* pointer, std::size_t bytes) const noexcept {
        const auto block_record = live_blocks_.find(const_cast<void*>(pointer));
        return block_record != live_blocks_.end() && block_record->second >= bytes;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_blocks_.size();
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* const block = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        try {
            live_blocks_.emplace(block, bytes);
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(block, bytes, alignment);
            throw;
        }
        ++allocations;
        return block;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        live_blocks_.erase(pointer);
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::unordered_map<void*, std::size_t> live_blocks_{std::pmr::new_delete_resource()};
};

struct PostPayload final {
    explicit PostPayload(std::pmr::memory_resource* resource)
        : bytes(resource) {
        bytes.resize(4096, 'x');
    }
    PostPayload(PostPayload&&) noexcept = default;
    PostPayload(const PostPayload&) = delete;
    std::pmr::vector<char> bytes;
};

struct ReentrantPostState final {
    ruvia::EventLoop* loop;
    std::unique_ptr<ruvia::EventLoopPool>* pool;
    std::promise<void>* completion;
    std::atomic_bool* payloadValid;

    void shutDownLoop() noexcept {
        *loop = ruvia::EventLoop{};
        pool->reset();
    }
};

struct InlineReentrantPostCallable final {
    explicit InlineReentrantPostCallable(ReentrantPostState& state) noexcept
        : state(&state) {
        payload.fill('x');
    }
    InlineReentrantPostCallable(const InlineReentrantPostCallable& other) noexcept
        : state(other.state),
          payload(other.payload) {
        state->shutDownLoop();
    }
    InlineReentrantPostCallable(InlineReentrantPostCallable&& other) noexcept
        : state(other.state),
          payload(other.payload) {
        state->shutDownLoop();
    }
    ~InlineReentrantPostCallable() {
        payload.fill('\0');
    }

    void operator()() {
        state->payloadValid->store(payload.size() == 16 && payload.front() == 'x' &&
                                       payload.back() == 'x',
            std::memory_order_release);
        state->completion->set_value();
    }

    ReentrantPostState* state;
    std::array<char, 16> payload{};
};

struct HeapReentrantPostCallable final {
    explicit HeapReentrantPostCallable(ReentrantPostState& state,
        std::pmr::memory_resource* resource)
        : state(&state),
          payload(resource) {
        payload.resize(4096, 'x');
    }
    HeapReentrantPostCallable(const HeapReentrantPostCallable& other)
        : state(other.state),
          payload(other.payload, other.payload.get_allocator().resource()) {
        state->shutDownLoop();
    }
    HeapReentrantPostCallable(HeapReentrantPostCallable&& other) noexcept
        : state(other.state),
          payload(std::move(other.payload)) {
        state->shutDownLoop();
    }
    ~HeapReentrantPostCallable() = default;

    void operator()() {
        state->payloadValid->store(payload.size() == 4096 && payload.front() == 'x' &&
                                       payload.back() == 'x',
            std::memory_order_release);
        state->completion->set_value();
    }

    ReentrantPostState* state;
    std::pmr::vector<char> payload;
};

struct ReentrantWorkerPostState final {
    std::optional<ruvia::WorkerHandle> worker;
    std::unique_ptr<ruvia::EventLoopPool> pool;
    bool retired{false};
    bool payloadValid{false};
    bool eraseOnDestroy{false};

    void retire() noexcept {
        if (retired) {
            return;
        }
        retired = true;
        worker.reset();
        pool.reset();
    }
};

struct InlineWorkerCallable final {
    explicit InlineWorkerCallable(ReentrantWorkerPostState& state) noexcept
        : state(&state) {
        payload.fill('i');
    }
    InlineWorkerCallable(const InlineWorkerCallable&) = delete;
    InlineWorkerCallable(InlineWorkerCallable&& other) noexcept
        : state(other.state),
          payload(other.payload) {
        state->retire();
    }
    void operator()() {
        state->payloadValid = payload.front() == 'i' && payload.back() == 'i';
    }
    ReentrantWorkerPostState* state;
    std::array<char, 16> payload{};
};

struct HeapWorkerCallable final {
    explicit HeapWorkerCallable(ReentrantWorkerPostState& state, std::pmr::memory_resource* resource)
        : state(&state),
          payload(resource) {
        payload.resize(4096, 'h');
    }
    HeapWorkerCallable(const HeapWorkerCallable& other)
        : state(other.state),
          payload(other.payload, other.payload.get_allocator().resource()) {
        state->retire();
    }
    HeapWorkerCallable(HeapWorkerCallable&&) noexcept = default;
    void operator()() {
        state->payloadValid = payload.size() == 4096 && payload.front() == 'h' &&
                              payload.back() == 'h';
    }
    ReentrantWorkerPostState* state;
    std::pmr::vector<char> payload;
};

struct ErasedWorkerCallable final {
    explicit ErasedWorkerCallable(ReentrantWorkerPostState& state) noexcept
        : state(&state) {
        payload.fill('e');
    }
    ErasedWorkerCallable(const ErasedWorkerCallable&) = delete;
    ErasedWorkerCallable(ErasedWorkerCallable&& other) noexcept
        : state(other.state),
          payload(other.payload) {}
    ~ErasedWorkerCallable() {
        if (state->eraseOnDestroy) {
            state->retire();
        }
    }
    void operator()() {
        state->payloadValid = payload.front() == 'e' && payload.back() == 'e';
    }
    ReentrantWorkerPostState* state;
    std::array<char, 16> payload{};
};

bool testWorkerHandleCallableLifetime() {
    const auto runRejected = [](ruvia::PostResult rejected, bool& payloadValid) {
        if (rejected != ruvia::PostStatus::kWorkerStopping || rejected.rejected() == nullptr) {
            return false;
        }
        auto task = std::move(rejected).takeRejected();
        ruvia::EventLoopPool recovery({.loopCount = 1, .mailboxCapacity = 1});
        if (recovery.loop(0).post(std::move(task)) != ruvia::PostStatus::kAccepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        return payloadValid;
    };

    {
        ReentrantWorkerPostState state;
        state.pool = std::make_unique<ruvia::EventLoopPool>(
            ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
        state.worker.emplace(state.pool->loop(0).handle());
        std::optional<InlineWorkerCallable> input;
        input.emplace(state);
        auto rejected = state.worker->post(std::move(*input));
        if (!state.retired || state.worker || state.pool || !runRejected(std::move(rejected), state.payloadValid)) {
            return false;
        }
    }

    CountingResource resource;
    {
        ReentrantWorkerPostState state;
        state.pool = std::make_unique<ruvia::EventLoopPool>(
            ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
        state.worker.emplace(state.pool->loop(0).handle());
        std::optional<HeapWorkerCallable> input;
        input.emplace(state, &resource);
        auto rejected = state.worker->post(*input);
        input.reset();
        if (!state.retired || state.worker || state.pool ||
            !runRejected(std::move(rejected), state.payloadValid) ||
            resource.allocations != resource.deallocations) {
            return false;
        }
    }

    {
        ReentrantWorkerPostState state;
        state.pool = std::make_unique<ruvia::EventLoopPool>(
            ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
        state.worker.emplace(state.pool->loop(0).handle());
        ruvia::MoveOnlyFunction<void()> input{ErasedWorkerCallable(state)};
        state.eraseOnDestroy = true;
        auto rejected = state.worker->post(std::move(input));
        if (!state.retired || state.worker || state.pool ||
            !runRejected(std::move(rejected), state.payloadValid)) {
            return false;
        }
    }
    return resource.allocations == resource.deallocations;
}

struct MailboxDestructorState final {
    const ruvia::WorkerHandle* worker{nullptr};
    ruvia::EventLoopAttachment* attachment{nullptr};
    int destroyed{0};
    bool ran{false};
};

class MailboxDestructorCallback final {
public:
    explicit MailboxDestructorCallback(MailboxDestructorState& state) noexcept
        : state_(&state) {}
    MailboxDestructorCallback(const MailboxDestructorCallback&) = delete;
    MailboxDestructorCallback(MailboxDestructorCallback&&) noexcept = default;

    ~MailboxDestructorCallback() {
        static_cast<void>(state_->worker->post([] {}));
        ++state_->destroyed;
    }

    void operator()() const {
        state_->ran = true;
        state_->attachment->stop();
    }

private:
    MailboxDestructorState* state_;
};

struct cancellation_observation final {
    unsigned calls{0};
    std::uint64_t operation_id{0};
    bool on_worker{false};
    bool owner_destroyed{false};
};

struct cancellation_owner final {
    const ruvia::WorkerHandle& worker;
    cancellation_observation& observed;

    ~cancellation_owner() {
        observed.owner_destroyed = true;
    }

    void cancelOperationById(std::uint64_t operation_id) noexcept {
        ++observed.calls;
        observed.operation_id = operation_id;
        observed.on_worker = worker.isCurrent();
    }
};

bool test_cancellation_reaches_worker_with_saturated_or_closed_mailbox(bool close) {
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 1);
    cancellation_observation observed;
    cancellation_owner owner{runtime.handle(), observed};
    auto mailbox = ruvia::makeWorkerCancellationMailbox(owner, runtime.handle());
    ruvia::StopSource source;
    auto registration = source.token().registerCallback(
        ruvia::WorkerCancellationPost(mailbox, 7));
    unsigned normal_calls = 0;
    const auto accepted = runtime.handle().post([&normal_calls] { ++normal_calls; });
    if (!accepted.accepted()) {
        return false;
    }
    if (close) {
        runtime.close();
    }
    const auto rejected = runtime.handle().post([] {});
    if (rejected.status() != (close ? ruvia::PostStatus::kWorkerStopping : ruvia::PostStatus::kQueueFull)) {
        return false;
    }
    source.requestStop();
    if (observed.calls != 0) {
        return false;
    }
    runtime.run();
    mailbox->detach(owner);
    return observed.calls == 1 && observed.operation_id == 7 && observed.on_worker &&
           normal_calls == 1;
}

bool test_queued_cancellation_releases_mailbox_after_owner_retirement() {
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 1);
    cancellation_observation observed;
    auto owner = std::make_unique<cancellation_owner>(runtime.handle(), observed);
    auto mailbox = ruvia::makeWorkerCancellationMailbox(*owner, runtime.handle());
    std::weak_ptr weak_mailbox(mailbox);
    ruvia::StopSource source;
    auto registration = source.token().registerCallback(
        ruvia::WorkerCancellationPost(mailbox, 9));
    source.requestStop();
    registration.reset();
    mailbox->detach(*owner);
    owner.reset();
    mailbox.reset();
    const bool retained_by_post = !weak_mailbox.expired();
    runtime.detach();
    context.run();
    return retained_by_post && observed.owner_destroyed && observed.calls == 0 &&
           weak_mailbox.expired();
}

bool test_cancellation_after_endpoint_detach_does_not_touch_retired_owner() {
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 1);
    cancellation_observation observed;
    auto owner = std::make_unique<cancellation_owner>(runtime.handle(), observed);
    auto mailbox = ruvia::makeWorkerCancellationMailbox(*owner, runtime.handle());
    ruvia::StopSource source;
    auto registration = source.token().registerCallback(
        ruvia::WorkerCancellationPost(mailbox, 11));
    mailbox->detach(*owner);
    owner.reset();
    runtime.detach();
    source.requestStop();
    return observed.owner_destroyed && observed.calls == 0 && context.run() == 0 &&
           mailbox.use_count() == 1;
}

bool testWorkerRuntimeContextOwnsStableDetachedEndpoint() {
    asio::io_context context;
    std::optional<ruvia::WorkerHandle> escapedHandle;
    {
        ruvia::WorkerRuntimeContext runtime(context, 8);
        const auto* handleAddress = &runtime.handle();
        if (&runtime.ioContext() != &context || handleAddress != &runtime.handle() ||
            !runtime.handle().valid()) {
            return false;
        }
        escapedHandle.emplace(runtime.handle());
        runtime.detach();
        if (runtime.handle().valid() || escapedHandle->valid()) {
            return false;
        }
    }
    asio::post(context, [] {});
    context.run();
    return escapedHandle && !escapedHandle->valid();
}

bool testMailboxCallableDestructionCanInspectWorker() {
    asio::io_context context;
    auto attachment = ruvia::attachEventLoop(context);
    const auto worker = attachment.loop().handle();
    MailboxDestructorState state{.worker = &worker, .attachment = &attachment};
    const auto submitted = worker.post(MailboxDestructorCallback(state));
    if (!submitted.accepted()) {
        return false;
    }
    attachment.run();
    return state.ran && state.destroyed > 0;
}

bool testMailboxFactoryRollbackAndDetach() {
    asio::io_context context;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool thrown = false;
    try {
        static_cast<void>((worker).post_factory([]() -> ruvia::MoveOnlyFunction<void()> {
            throw std::runtime_error("factory failed");
        }));
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    bool empty = false;
    try {
        static_cast<void>((worker).post_factory([] { return ruvia::MoveOnlyFunction<void()>(); }));
    } catch (const std::invalid_argument&) {
        empty = true;
    }
    bool recovered = (worker).post_factory([] { return ruvia::MoveOnlyFunction<void()>([] {}); }) ==
                     ruvia::PostStatus::kAccepted;
    context.run();

    bool rawStackRejected = false;
    ruvia::detail::WorkerDispatcher rawStack(context, 1);
    try {
        static_cast<void>(rawStack.post([] {}));
    } catch (const std::bad_weak_ptr&) {
        rawStackRejected = true;
    }

    bool abandonedRan = false;
    bool abandonedDestroyed = false;
    struct Probe final {
        bool* ran;
        bool* destroyed;
        Probe(bool& ranValue, bool& destroyedValue)
            : ran(&ranValue),
              destroyed(&destroyedValue) {}
        Probe(Probe&& other) noexcept
            : ran(other.ran),
              destroyed(std::exchange(other.destroyed, nullptr)) {}
        ~Probe() {
            if (destroyed != nullptr) {
                *destroyed = true;
            }
        }
        void operator()() {
            *ran = true;
        }
    };
    const auto detached = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
    const auto detachedWorker = ruvia::detail::WorkerHandleAccess::make(detached);
    const auto status = (detachedWorker).post_factory([detached, &abandonedRan, &abandonedDestroyed] {
        detached->detachContext();
        return ruvia::MoveOnlyFunction<void()>(
            Probe{abandonedRan, abandonedDestroyed});
    });
    return thrown && empty && recovered && rawStackRejected &&
           status == ruvia::PostStatus::kAccepted && !abandonedRan && abandonedDestroyed;
}

bool testMailboxFactoryCanFinishAfterDetach() {
    asio::io_context context;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    std::promise<void> entered;
    std::promise<void> resume;
    auto resumed = resume.get_future();
    std::atomic_int destroyed{0};
    bool ran = false;
    auto status = ruvia::PostStatus::kWorkerStopping;
    struct Payload final {
        std::atomic_int* destroyed;
        ~Payload() {
            destroyed->fetch_add(1);
        }
    };
    std::jthread producer([&] {
        status = (worker).post_factory([&] {
            auto payload = std::make_unique<Payload>(&destroyed);
            entered.set_value();
            resumed.wait();
            return ruvia::MoveOnlyFunction<void()>(
                [payload = std::move(payload), &ran] { ran = true; });
        });
    });
    entered.get_future().wait();
    dispatcher->detachContext();
    const bool retainedWhileReserved = destroyed.load() == 0;
    resume.set_value();
    producer.join();
    context.run();
    return retainedWhileReserved && status == ruvia::PostStatus::kAccepted && !ran &&
           destroyed.load() == 1;
}

bool testEventLoopPostBorrowAndRejectedCallableOwnership() {
    constexpr std::size_t kRepeatedPosts = 64;
    CountingResource resource;
    std::atomic_bool invalidPayloadValid{false};
    std::atomic_size_t repeatedValid{0};
    std::atomic_size_t repeatedInvalid{0};
    std::promise<void> repeatedCompleted;
    auto repeatedCompletion = repeatedCompleted.get_future();
    ruvia::EventLoopPool accepting({.loopCount = 1, .mailboxCapacity = kRepeatedPosts + 1});
    const auto loop = accepting.loop(0);

    auto invalid = ruvia::EventLoop{}.post(
        [payload = PostPayload(&resource), &invalidPayloadValid]() mutable {
            invalidPayloadValid.store(payload.bytes.size() == 4096 && payload.bytes.front() == 'x' &&
                                          payload.bytes.back() == 'x',
                std::memory_order_release);
        });
    if (invalid != ruvia::PostStatus::kWorkerStopping || invalid.rejected() == nullptr ||
        resource.allocations == resource.deallocations) {
        return false;
    }
    auto invalidTask = std::move(invalid).takeRejected();
    if (resource.allocations == resource.deallocations ||
        loop.post(std::move(invalidTask)) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    for (std::size_t i = 0; i < kRepeatedPosts; ++i) {
        auto posted = accepting.loop(0).post([payload = PostPayload(&resource), i, &repeatedValid,
                                                 &repeatedInvalid, &repeatedCompleted]() mutable {
            if (payload.bytes.size() == 4096 && payload.bytes.front() == 'x' &&
                payload.bytes.back() == 'x') {
                repeatedValid.fetch_add(1, std::memory_order_relaxed);
            } else {
                repeatedInvalid.fetch_add(1, std::memory_order_relaxed);
            }
            if (i + 1 == kRepeatedPosts) {
                repeatedCompleted.set_value();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    accepting.start();
    repeatedCompletion.wait();
    accepting.stop();
    accepting.join();
    if (!invalidPayloadValid.load(std::memory_order_acquire) ||
        repeatedValid.load(std::memory_order_relaxed) != kRepeatedPosts ||
        repeatedInvalid.load(std::memory_order_relaxed) != 0 ||
        resource.allocations != resource.deallocations) {
        return false;
    }

    ruvia::EventLoop closed;
    {
        ruvia::EventLoopPool stopped({.loopCount = 1, .mailboxCapacity = 1});
        closed = stopped.loop(0);
        stopped.join();
    }
    std::atomic_bool closedPayloadValid{false};
    auto rejected = closed.post(
        [payload = PostPayload(&resource), &closedPayloadValid]() mutable {
            closedPayloadValid.store(payload.bytes.size() == 4096 && payload.bytes.front() == 'x' &&
                                         payload.bytes.back() == 'x',
                std::memory_order_release);
        });
    if (rejected != ruvia::PostStatus::kWorkerStopping || rejected.rejected() == nullptr ||
        resource.allocations == resource.deallocations) {
        return false;
    }
    auto retry = std::move(rejected).takeRejected();
    if (resource.allocations == resource.deallocations) {
        return false;
    }
    std::promise<void> recovered;
    auto recoveredFuture = recovered.get_future();
    ruvia::EventLoopPool recovery({.loopCount = 1, .mailboxCapacity = 1});
    auto recoveredPost = recovery.loop(0).post(
        [task = std::move(retry), &recovered]() mutable {
            task();
            recovered.set_value();
        });
    if (!recoveredPost.accepted()) {
        return false;
    }
    recovery.start();
    recoveredFuture.wait();
    recovery.stop();
    recovery.join();
    return closedPayloadValid.load(std::memory_order_acquire) &&
           resource.allocations == resource.deallocations;
}

bool testEventLoopPostProtectsReentrantInlineMove() {
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payloadValid{false};
    auto originalPool = std::make_unique<ruvia::EventLoopPool>(
        ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
    auto loop = originalPool->loop(0);
    ReentrantPostState state{.loop = &loop,
        .pool = &originalPool,
        .completion = &completed,
        .payloadValid = &payloadValid};
    std::optional<InlineReentrantPostCallable> input;
    input.emplace(state);

    auto rejected = loop.post(std::move(*input));
    if (rejected != ruvia::PostStatus::kWorkerStopping || rejected.rejected() == nullptr ||
        originalPool != nullptr || loop.valid()) {
        return false;
    }
    auto retry = std::move(rejected).takeRejected();
    input.reset();
    ruvia::EventLoopPool recovery({.loopCount = 1, .mailboxCapacity = 1});
    if (recovery.loop(0).post(std::move(retry)) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payloadValid.load(std::memory_order_acquire);
}

bool testEventLoopPostProtectsReentrantHeapCopy() {
    CountingResource resource;
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payloadValid{false};
    auto originalPool = std::make_unique<ruvia::EventLoopPool>(
        ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
    auto loop = originalPool->loop(0);
    ReentrantPostState state{.loop = &loop,
        .pool = &originalPool,
        .completion = &completed,
        .payloadValid = &payloadValid};
    std::optional<HeapReentrantPostCallable> input;
    input.emplace(state, &resource);
    const auto callable_storage = resource.allocations - resource.deallocations;
    if (callable_storage == 0) {
        return false;
    }

    auto rejected = loop.post(*input);
    if (rejected != ruvia::PostStatus::kWorkerStopping || rejected.rejected() == nullptr ||
        originalPool != nullptr || loop.valid()) {
        return false;
    }
    input.reset();
    if (resource.allocations - resource.deallocations != callable_storage) {
        return false;
    }
    auto retry = std::move(rejected).takeRejected();
    if (resource.allocations - resource.deallocations != callable_storage) {
        return false;
    }
    ruvia::EventLoopPool recovery({.loopCount = 1, .mailboxCapacity = 1});
    if (recovery.loop(0).post(std::move(retry)) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payloadValid.load(std::memory_order_acquire) &&
           resource.allocations == resource.deallocations;
}

bool testWorkerSubmissionViewLifecycleAndRejection() {
    struct ThrowOnCopy final {
        explicit ThrowOnCopy(std::pmr::memory_resource* resource)
            : bytes(resource) {
            bytes.resize(4096, 'c');
        }
        ThrowOnCopy(const ThrowOnCopy& other)
            : bytes(other.bytes, other.bytes.get_allocator().resource()) {
            throw std::runtime_error("copy failed");
        }
        ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
        void operator()() const {}
        std::pmr::vector<char> bytes;
    };
    struct ThrowOnMove final {
        explicit ThrowOnMove(std::pmr::memory_resource* resource)
            : bytes(resource) {
            bytes.resize(4096, 'm');
        }
        ThrowOnMove(ThrowOnMove&& other)
            : bytes(std::move(other.bytes)) {
            throw std::runtime_error("move failed");
        }
        void operator()() const {}
        std::pmr::vector<char> bytes;
    };

    asio::io_context exceptionContext;
    ruvia::WorkerRuntimeContext exceptionRuntime(exceptionContext, 1);
    const auto exceptionView = exceptionRuntime.submission();
    CountingResource exceptionResource;
    {
        ThrowOnCopy copyInput(&exceptionResource);
        ThrowOnMove moveInput(&exceptionResource);
        bool copyThrew = false;
        bool moveThrew = false;
        try {
            static_cast<void>(exceptionView.post(copyInput));
        } catch (const std::runtime_error&) {
            copyThrew = true;
        }
        try {
            static_cast<void>(exceptionView.post(std::move(moveInput)));
        } catch (const std::runtime_error&) {
            moveThrew = true;
        }
        const auto afterException = exceptionView.post([] {});
        if (!copyThrew || !moveThrew || !exceptionView.valid() || !exceptionView.accepting() ||
            afterException != ruvia::PostStatus::kAccepted) {
            return false;
        }
    }
    if (exceptionResource.allocations != exceptionResource.deallocations) {
        return false;
    }

    struct ViewReentryState final {
        std::optional<ruvia::WorkerSubmissionView>* source;
        ruvia::WorkerRuntimeContext* runtime;
        bool detach;
        bool retired{false};
        bool payloadValid{false};
        CountingResource* resource;

        void retire() noexcept {
            if (retired) {
                return;
            }
            retired = true;
            source->reset();
            if (detach) {
                runtime->detach();
            } else {
                runtime->close();
            }
        }
    };
    struct ReentrantViewCallable final {
        explicit ReentrantViewCallable(ViewReentryState& state)
            : state(&state),
              payload(state.resource) {
            payload.resize(4096, 'v');
        }
        ReentrantViewCallable(const ReentrantViewCallable& other)
            : state(other.state),
              payload(other.payload, other.payload.get_allocator().resource()) {
            state->retire();
        }
        ReentrantViewCallable(ReentrantViewCallable&& other) noexcept
            : state(other.state),
              payload(std::move(other.payload)) {
            state->retire();
        }
        void operator()() {
            state->payloadValid = payload.size() == 4096 && payload.front() == 'v' &&
                                  payload.back() == 'v';
        }
        ViewReentryState* state;
        std::pmr::vector<char> payload;
    };

    const auto exerciseReentry = [](bool detach, bool useCopy) {
        CountingResource resource;
        asio::io_context context;
        ruvia::WorkerRuntimeContext runtime(context, 1);
        std::optional<ruvia::WorkerSubmissionView> source;
        source.emplace(runtime.submission());
        ViewReentryState state{.source = &source, .runtime = &runtime, .detach = detach, .resource = &resource};
        std::optional<ReentrantViewCallable> input;
        input.emplace(state);
        auto rejected = useCopy ? source->post(*input) : source->post(std::move(*input));
        if (!state.retired || source || !runtime.submission().valid() ||
            runtime.submission().accepting() || rejected != ruvia::PostStatus::kWorkerStopping ||
            rejected.rejected() == nullptr) {
            return false;
        }
        auto task = std::move(rejected).takeRejected();
        ruvia::EventLoopPool recovery({.loopCount = 1, .mailboxCapacity = 1});
        if (recovery.loop(0).post(std::move(task)) != ruvia::PostStatus::kAccepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        input.reset();
        return state.payloadValid && resource.allocations == resource.deallocations;
    };
    if (!exerciseReentry(false, true) || !exerciseReentry(true, false)) {
        return false;
    }

    struct ReservedMoveState final {
        ruvia::WorkerRuntimeContext* runtime;
        CountingResource* resource;
        bool detach;
        int moves{0};
        bool reservationObserved{false};
        bool ran{false};
    };
    struct InlineReservedMoveCallable final {
        explicit InlineReservedMoveCallable(ReservedMoveState& state)
            : state(&state),
              payload(static_cast<char*>(state.resource->allocate(16, alignof(char)))) {
            std::fill_n(payload, 16, 'r');
        }
        InlineReservedMoveCallable(InlineReservedMoveCallable&& other) noexcept
            : state(other.state),
              payload(std::exchange(other.payload, nullptr)) {
            // This move transfers the callable into its already reserved node;
            // detach may cause additional cleanup moves after this point.
            if (++state->moves == 3) {
                state->reservationObserved =
                    state->runtime->submission().post([] {}) == ruvia::PostStatus::kQueueFull;
                if (state->detach) {
                    state->runtime->detach();
                } else {
                    state->runtime->close();
                }
            }
        }
        ~InlineReservedMoveCallable() {
            if (payload != nullptr) {
                state->resource->deallocate(payload, 16, alignof(char));
            }
        }
        void operator()() {
            state->ran = payload != nullptr && payload[0] == 'r' && payload[15] == 'r';
        }

        ReservedMoveState* state;
        char* payload;
    };
    static_assert(sizeof(InlineReservedMoveCallable) <= 3 * sizeof(void*));
    static_assert(std::is_nothrow_move_constructible_v<InlineReservedMoveCallable>);

    const auto exerciseReservedMove = [](bool detach) {
        CountingResource resource;
        asio::io_context context;
        ruvia::WorkerRuntimeContext runtime(context, 1);
        ReservedMoveState state{.runtime = &runtime, .resource = &resource, .detach = detach};
        InlineReservedMoveCallable input(state);
        const auto submitted = runtime.submission().post(std::move(input));
        if (submitted != ruvia::PostStatus::kAccepted || !state.reservationObserved ||
            resource.allocations != 1) {
            return false;
        }
        if (detach) {
            if (state.ran || resource.deallocations != 1) {
                return false;
            }
        } else {
            context.run();
            if (!state.ran || resource.deallocations != 1) {
                return false;
            }
        }
        return resource.allocations == resource.deallocations;
    };
    if (!exerciseReservedMove(false) || !exerciseReservedMove(true)) {
        return false;
    }

    constexpr std::size_t kRepeatedPosts = 64;
    CountingResource resource;
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, kRepeatedPosts + 2);
    const auto view = runtime.submission();
    bool acceptedPayloadValid = false;
    if (!view.valid() || !view.accepting() || view.post([] {}) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    auto accepted = view.post([payload = PostPayload(&resource), &acceptedPayloadValid]() mutable {
        acceptedPayloadValid = payload.bytes.size() == 4096 && payload.bytes.front() == 'x' &&
                               payload.bytes.back() == 'x';
    });
    if (accepted != ruvia::PostStatus::kAccepted) {
        return false;
    }
    for (std::size_t index = 0; index < kRepeatedPosts; ++index) {
        auto posted = view.post([payload = PostPayload(&resource)]() mutable {
            if (payload.bytes.size() != 4096 || payload.bytes.front() != 'x') {
                std::terminate();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    context.run();
    if (!acceptedPayloadValid || resource.allocations != resource.deallocations) {
        return false;
    }

    asio::io_context fullContext;
    ruvia::WorkerRuntimeContext fullRuntime(fullContext, 1);
    const auto fullView = fullRuntime.submission();
    CountingResource rejectedResource;
    bool retainedPayloadValid = false;
    if (fullView.post([] {}) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    auto full = fullView.post([payload = PostPayload(&rejectedResource), &retainedPayloadValid]() mutable {
        retainedPayloadValid = payload.bytes.size() == 4096 && payload.bytes.front() == 'x' &&
                               payload.bytes.back() == 'x';
    });
    if (full != ruvia::PostStatus::kQueueFull || full.rejected() == nullptr) {
        return false;
    }
    auto retained = std::move(full).takeRejected();
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto again = fullView.post(std::move(retained));
        if (again != ruvia::PostStatus::kQueueFull || again.rejected() == nullptr ||
            rejectedResource.allocations == rejectedResource.deallocations) {
            return false;
        }
        retained = std::move(again).takeRejected();
    }
    fullContext.run();
    fullContext.restart();
    const auto retried = fullView.post(std::move(retained));
    if (retried != ruvia::PostStatus::kAccepted) {
        return false;
    }
    fullContext.run();
    if (!retainedPayloadValid || rejectedResource.allocations != rejectedResource.deallocations) {
        return false;
    }

    asio::io_context closedContext;
    ruvia::WorkerRuntimeContext closedRuntime(closedContext, 1);
    auto closedView = closedRuntime.submission();
    closedRuntime.close();
    if (!closedView.valid() || closedView.accepting() ||
        closedView.post([] {}) != ruvia::PostStatus::kWorkerStopping) {
        return false;
    }
    closedRuntime.detach();
    if (!closedView.valid() || closedView.accepting()) {
        return false;
    }
    ruvia::WorkerSubmissionView invalid;
    return !invalid.valid() && !invalid.accepting() &&
           invalid.post([] {}) == ruvia::PostStatus::kWorkerStopping && resource.allocations > 0 &&
           resource.allocations == resource.deallocations;
}

bool testPostOutcomeInvariantsAndEmptyCallbacks() {
    bool acceptedTakeRejected = false;
    try {
        static_cast<void>(std::move(ruvia::PostResult::accept()).takeRejected());
    } catch (const std::logic_error&) {
        acceptedTakeRejected = true;
    }

    bool acceptedRejectionStatus = false;
    try {
        static_cast<void>(ruvia::PostResult::reject(ruvia::PostStatus::kAccepted, [] {}));
    } catch (const std::invalid_argument&) {
        acceptedRejectionStatus = true;
    }

    bool emptyRejectedTask = false;
    try {
        static_cast<void>(ruvia::PostResult::reject(ruvia::PostStatus::kQueueFull, {}));
    } catch (const std::invalid_argument&) {
        emptyRejectedTask = true;
    }

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    bool emptyPost = false;
    using NullCallback = void (*)();
    try {
        static_cast<void>(loop.post(static_cast<NullCallback>(nullptr)));
    } catch (const std::invalid_argument&) {
        emptyPost = true;
    }
    bool emptyStopCallback = false;
    using NullStopCallback = ruvia::MoveOnlyFunction<ruvia::Task<void>()>;
    try {
        static_cast<void>(loop.onStop(NullStopCallback{}));
    } catch (const std::invalid_argument&) {
        emptyStopCallback = true;
    }
    loops.join();
    return acceptedTakeRejected && acceptedRejectionStatus && emptyRejectedTask && emptyPost &&
           emptyStopCallback;
}

ruvia::Task<void> waitForSignal(ruvia::WorkerSignal& signal, bool& resumed,
    std::size_t& remaining, ruvia::EventLoopAttachment& attachment) {
    {
        auto discardedColdWait = signal.wait();
        static_cast<void>(discardedColdWait);
    }
    co_await signal.wait();
    resumed = true;
    if (--remaining == 0) {
        attachment.stop();
    }
}

bool testWorkerSignalIsWorkerAffine() {
    bool invalidWorkerRejected = false;
    ruvia::WorkerHandle invalidWorker;
    try {
        ruvia::WorkerSignal invalid(invalidWorker);
    } catch (const std::invalid_argument&) {
        invalidWorkerRejected = true;
    }

    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto workerHandle = attachment.loop().handle();
    ruvia::WorkerSignal firstSignal(workerHandle);
    ruvia::WorkerSignal secondSignal(workerHandle);
    const bool workerBorrowed = &firstSignal.worker() == &workerHandle;
    bool wait_creation_rejected = false;
    try {
        static_cast<void>(firstSignal.wait());
    } catch (const std::logic_error&) {
        wait_creation_rejected = true;
    }
    bool firstResumed = false;
    bool secondResumed = false;
    std::size_t remaining = 2;
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(firstSignal, firstResumed, remaining, attachment)),
        asio::detached);
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(secondSignal, secondResumed, remaining, attachment)),
        asio::detached);
    asio::post(ioContext, [&] {
        firstSignal.notify();
        secondSignal.notify();
    });
    ioContext.run();
    return invalidWorkerRejected && workerBorrowed && wait_creation_rejected && firstResumed && secondResumed;
}

ruvia::Task<void> wait_signal_once(ruvia::WorkerSignal& signal, bool& resumed) {
    co_await signal.wait();
    resumed = true;
}

ruvia::Task<void> exercise_signal_pending_latch(
    ruvia::WorkerSignal& signal, ruvia::EventLoopAttachment& attachment, bool& success) {
    signal.notify();
    signal.notify();
    {
        auto cold_wait = signal.wait();
        static_cast<void>(cold_wait);
    }
    co_await signal.wait();

    bool resumed = false;
    ruvia::TaskScope scope(signal.worker());
    scope.spawn(wait_signal_once(signal, resumed));
    const bool second_wait_suspended = !resumed;
    signal.notify();
    signal.notify();
    co_await scope.join();
    co_await signal.wait();
    success = second_wait_suspended && resumed;
    attachment.stop();
}

bool test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake() {
    asio::io_context context;
    auto attachment = ruvia::attachEventLoop(context);
    const auto worker = attachment.loop().handle();
    ruvia::WorkerSignal signal(worker);
    bool success = false;
    asio::co_spawn(context,
        ruvia::detail::taskAsAwaitable(exercise_signal_pending_latch(signal, attachment, success)),
        asio::detached);
    context.run();
    return success;
}

bool testWorkerSignalHasNoArbitraryWaiterLimit() {
    constexpr std::size_t kWaiterCount = 16;
    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto workerHandle = attachment.loop().handle();
    ruvia::WorkerSignal signal(workerHandle);
    std::array<bool, kWaiterCount> resumed{};
    std::size_t remaining = kWaiterCount;
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(ioContext,
            ruvia::detail::taskAsAwaitable(
                waitForSignal(signal, resumed[index], remaining, attachment)),
            asio::detached);
    }

    asio::post(ioContext, [&] { signal.notify(); });
    ioContext.run();
    for (const bool value : resumed) {
        if (!value) {
            return false;
        }
    }
    return true;
}

ruvia::Task<void> startColdSignalWait(ruvia::Task<void> coldWait, bool& rejected) {
    try {
        co_await std::move(coldWait);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

bool testWorkerSignalRechecksAffinityWhenColdWaitStarts() {
    asio::io_context ownerContext;
    asio::io_context otherContext;
    auto ownerAttachment = ruvia::attachEventLoop(ownerContext);
    auto otherAttachment = ruvia::attachEventLoop(otherContext);
    const auto ownerHandle = ownerAttachment.loop().handle();
    ruvia::WorkerSignal signal(ownerHandle);
    std::optional<ruvia::Task<void>> coldWait;

    asio::post(ownerContext, [&] {
        coldWait.emplace(signal.wait());
        ownerAttachment.stop();
    });
    ownerContext.run();
    if (!coldWait.has_value()) {
        return false;
    }

    bool rejected = false;
    asio::co_spawn(otherContext,
        ruvia::detail::taskAsAwaitable(startColdSignalWait(std::move(*coldWait), rejected)),
        asio::detached);
    asio::post(otherContext, [&] { otherAttachment.stop(); });
    otherContext.run();
    coldWait.reset();
    return rejected;
}

bool testDispatchAndAffinity() {
    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    if (!first.valid() || first.id() == 0 || first.id() == second.id() || first.isCurrent()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (loops.loopFor(key).id() != loops.loopFor(ruvia::detail::workerSelectionHash(key)).id()) {
        return false;
    }
    if (&first.ioContext() != &first.executor().context() || first.handle().id() != first.id()) {
        return false;
    }
    asio::ip::tcp::socket tcp(first.ioContext());
    asio::ip::udp::socket udp(first.ioContext());
    if (&tcp.get_executor().context() != &first.ioContext() ||
        &udp.get_executor().context() != &first.ioContext()) {
        return false;
    }

    std::promise<bool> completed;
    auto result = completed.get_future();
    std::atomic_bool stopCallbackRan{false};
    std::atomic_bool stopCallbackOnLoop{false};
    auto stopRegistration = first.onStop([&]() -> ruvia::Task<void> {
        stopCallbackOnLoop = first.isCurrent();
        stopCallbackRan = true;
        co_return;
    });
    auto moveOnly = std::make_unique<int>(42);
    if (first.post([worker = first, value = std::move(moveOnly),
                       completed = std::move(completed)]() mutable {
            completed.set_value(worker.isCurrent() && *value == 42);
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    const bool success = result.get();
    loops.stop();
    loops.join();
    return success && stopRegistration.valid() && stopCallbackRan && stopCallbackOnLoop &&
           first.post([] {}) == ruvia::PostStatus::kWorkerStopping;
}

bool testBoundedMailbox() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    const auto worker = loops.loop(0);
    std::atomic<int> calls{0};
    std::promise<void> completed;
    auto result = completed.get_future();
    std::promise<void> retried;
    auto retriedResult = retried.get_future();

    if (worker.post([&] { ++calls; }) != ruvia::PostStatus::kAccepted || worker.post([&] {
            ++calls;
            completed.set_value();
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    auto rejected = worker.post([value = std::make_unique<int>(9), &calls, &retried] {
        calls.fetch_add(*value);
        retried.set_value();
    });
    if (rejected != ruvia::PostStatus::kQueueFull || rejected.rejected() == nullptr) {
        return false;
    }

    loops.start();
    result.get();
    if (worker.post(std::move(rejected).takeRejected()) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    retriedResult.get();
    loops.stop();
    loops.join();
    bool recoveredAfterStop = false;
    auto stopped = worker.post([&recoveredAfterStop] { recoveredAfterStop = true; });
    if (stopped != ruvia::PostStatus::kWorkerStopping || stopped.rejected() == nullptr) {
        return false;
    }
    auto stoppedTask = std::move(stopped).takeRejected();
    stoppedTask();
    return calls.load() == 11 && recoveredAfterStop;
}

bool testExternalEventLoopAttachment() {
    asio::io_context ioContext;
    {
        auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 4});
        const auto loop = attachment.loop();
        if (!attachment.valid() || !loop.valid() || &loop.ioContext() != &ioContext) {
            return false;
        }

        asio::ip::tcp::socket tcp(loop.executor());
        asio::ip::udp::socket udp(loop.executor());
        if (&tcp.get_executor().context() != &ioContext ||
            &udp.get_executor().context() != &ioContext) {
            return false;
        }

        bool duplicateRejected = false;
        try {
            auto duplicate = ruvia::attachEventLoop(ioContext);
        } catch (const std::invalid_argument&) {
            duplicateRejected = true;
        }
        if (!duplicateRejected) {
            return false;
        }

        std::promise<bool> completed;
        auto result = completed.get_future();
        std::atomic_bool stopCallbackRan{false};
        std::atomic_bool stopCallbackOnLoop{false};
        auto stopRegistration = loop.onStop([&]() -> ruvia::Task<void> {
            stopCallbackOnLoop = loop.isCurrent();
            stopCallbackRan = true;
            co_return;
        });
        if (loop.post([loop, completed = std::move(completed)]() mutable {
                completed.set_value(loop.isCurrent());
            }) != ruvia::PostStatus::kAccepted) {
            return false;
        }

        std::thread externalThread([&] { attachment.run(); });
        bool dispatchedOnExternalThread = false;
        try {
            dispatchedOnExternalThread = result.get();
            attachment.stop();
            externalThread.join();
        } catch (...) {
            attachment.stop();
            if (externalThread.joinable()) {
                externalThread.join();
            }
            throw;
        }
        if (!dispatchedOnExternalThread || !stopRegistration.valid() || !stopCallbackRan ||
            !stopCallbackOnLoop || loop.valid() ||
            loop.post([] {}) != ruvia::PostStatus::kWorkerStopping) {
            return false;
        }
    }

    ioContext.restart();
    {
        auto attachment = ruvia::attachEventLoop(ioContext);
        attachment.stop();
        attachment.run();
    }

    ioContext.restart();
    try {
        auto invalid = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 0});
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

ruvia::Task<void> finishShutdownCleanup(
    ruvia::WorkerHandle worker, std::atomic_bool& finished) {
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)));
    finished.store(true, std::memory_order_release);
    co_return;
}

bool testPoolReportedFailureStopsEveryLoopAndJoinRethrows() {
    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic_bool firstCleanupFinished{false};
    std::atomic_bool secondCleanupFinished{false};
    auto firstCleanup = first.onStop([&]() -> ruvia::Task<void> {
        co_await finishShutdownCleanup(first.handle(), firstCleanupFinished);
    });
    auto secondCleanup = second.onStop([&]() -> ruvia::Task<void> {
        co_await finishShutdownCleanup(second.handle(), secondCleanupFinished);
    });
    std::promise<void> reported;
    auto reportedResult = reported.get_future();
    if (first.post([first, &reported] {
            first.reportFailure(std::make_exception_ptr(std::runtime_error("reported runtime failure")));
            reported.set_value();
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    loops.start();
    if (reportedResult.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    const bool allClosed = !first.accepting() && !second.accepting();
    bool originalFailureRethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        originalFailureRethrown = std::string_view(error.what()) == "reported runtime failure";
    }
    return firstCleanup.valid() && secondCleanup.valid() && allClosed &&
           firstCleanupFinished.load(std::memory_order_acquire) &&
           secondCleanupFinished.load(std::memory_order_acquire) && originalFailureRethrown;
}

struct FailureReportGate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool released{false};
    std::atomic_size_t calls{0};
};

class GatedFailure final : public std::exception {
public:
    explicit GatedFailure(FailureReportGate& gate) noexcept
        : gate_(gate) {}

    const char* what() const noexcept override {
        gate_.calls.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(gate_.mutex);
        gate_.entered = true;
        gate_.changed.notify_all();
        gate_.changed.wait(lock, [this] { return gate_.released; });
        return "gated first pool failure";
    }

private:
    FailureReportGate& gate_;
};

class CountedFailure final : public std::exception {
public:
    explicit CountedFailure(std::shared_ptr<std::atomic_size_t> calls) noexcept
        : calls_(std::move(calls)) {}

    const char* what() const noexcept override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        return "handed-off pool failure";
    }

private:
    std::shared_ptr<std::atomic_size_t> calls_;
};

ruvia::Task<void> reportPoolRootFailure(ruvia::EventLoop loop, FailureReportGate& gate) {
    std::exception_ptr failure;
    try {
        throw GatedFailure(gate);
    } catch (...) {
        failure = std::current_exception();
    }
    loop.reportFailure(std::move(failure));
    co_return;
}

bool testPoolFailureHandoffDuringDestructorReport() {
    FailureReportGate gate;
    auto secondFailureCalls = std::make_shared<std::atomic_size_t>(0);
    std::exception_ptr secondFailure;
    try {
        throw CountedFailure(secondFailureCalls);
    } catch (...) {
        secondFailure = std::current_exception();
    }
    auto pool = std::make_unique<ruvia::EventLoopPool>(
        ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = pool->loop(0);
    auto root = loop.start(reportPoolRootFailure(loop, gate));
    pool->start();
    root.wait();
    root.get();

    std::thread destroyer([&] { pool.reset(); });
    bool firstReportEntered = false;
    {
        std::unique_lock lock(gate.mutex);
        firstReportEntered = gate.changed.wait_for(
            lock, std::chrono::seconds(5), [&] { return gate.entered; });
    }
    if (firstReportEntered) {
        std::thread reporter([&] {
            loop.reportFailure(secondFailure);
        });
        reporter.join();
    }
    {
        const std::lock_guard lock(gate.mutex);
        gate.released = true;
    }
    gate.changed.notify_all();
    destroyer.join();

    return firstReportEntered && !pool && gate.calls.load(std::memory_order_relaxed) == 1 &&
           secondFailureCalls->load(std::memory_order_relaxed) == 1;
}

bool testPoolFailureReportWorksAfterMailboxClosure() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool queuedWorkRan{false};
    if (loop.post([&] { queuedWorkRan.store(true, std::memory_order_release); }) !=
        ruvia::PostStatus::kAccepted) {
        return false;
    }
    loops.stop();
    loop.reportFailure(std::make_exception_ptr(std::runtime_error("late runtime failure")));
    bool failureRethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        failureRethrown = std::string_view(error.what()) == "late runtime failure";
    }
    return failureRethrown && queuedWorkRan.load(std::memory_order_acquire) && !loop.valid();
}

bool testAttachmentReportFailureKeepsNativeRunOwnership() {
    const auto run = [](bool wrappedRun) {
        asio::io_context ioContext;
        auto attachment = ruvia::attachEventLoop(ioContext);
        const auto loop = attachment.loop();
        std::atomic_bool cleanupFinished{false};
        auto cleanup = loop.onStop([&]() -> ruvia::Task<void> {
            co_await finishShutdownCleanup(loop.handle(), cleanupFinished);
        });
        bool reportReturned = false;
        bool unrelatedWorkRan = false;
        asio::post(ioContext, [&] {
            loop.reportFailure(std::make_exception_ptr(std::runtime_error("attached report")));
            reportReturned = true;
            asio::post(ioContext, [&] { unrelatedWorkRan = true; });
        });
        bool nativeRunThrew = false;
        try {
            if (wrappedRun) {
                attachment.run();
            } else {
                ioContext.run();
            }
        } catch (...) {
            nativeRunThrew = true;
        }
        return cleanup.valid() && reportReturned && unrelatedWorkRan &&
               cleanupFinished.load(std::memory_order_acquire) && !nativeRunThrew && !loop.valid();
    };
    return run(true) && run(false);
}

ruvia::Task<void> failedAbandonedAttachmentRoot() {
    throw std::runtime_error("abandoned attachment root failed");
    co_return;
}

bool testAbandonedAttachmentRootFailureRetires(bool wrappedRun) {
    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto loop = attachment.loop();
    std::atomic_bool unrelatedWorkRan{false};
    asio::post(ioContext, [&] { unrelatedWorkRan.store(true, std::memory_order_release); });
    {
        auto root = loop.start(failedAbandonedAttachmentRoot());
    }

    if (wrappedRun) {
        attachment.run();
    } else {
        ioContext.run();
    }

    bool contextAccessRejected = false;
    try {
        static_cast<void>(loop.ioContext());
    } catch (const std::logic_error&) {
        contextAccessRejected = true;
    }
    return unrelatedWorkRan.load(std::memory_order_acquire) && !attachment.valid() &&
           !loop.valid() && contextAccessRejected;
}

bool testAttachmentRunFailureRetiresThroughAsyncCleanup() {
    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto loop = attachment.loop();
    bool cleanupRan = false;
    auto cleanup = loop.onStop([&]() -> ruvia::Task<void> {
        cleanupRan = true;
        co_return;
    });
    asio::post(ioContext, [] { throw std::runtime_error("attached handler failed"); });
    bool failurePropagated = false;
    try {
        attachment.run();
    } catch (const std::runtime_error& error) {
        failurePropagated = std::string_view(error.what()) == "attached handler failed";
    }
    return cleanup.valid() && cleanupRan && failurePropagated && !loop.valid();
}

bool testExternalAttachmentRetainsStateUntilContextCleanup() {
    asio::io_context ioContext;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool entered = false;
    bool release = false;
    std::atomic_bool stopCallbackRan{false};
    ruvia::EventLoopStopRegistration stopRegistration;
    std::thread externalThread;

    {
        auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 4});
        stopRegistration = attachment.loop().onStop([&]() -> ruvia::Task<void> {
            stopCallbackRan.store(true, std::memory_order_release);
            co_return;
        });
        asio::post(ioContext, [&] {
            {
                const std::lock_guard lock(gateMutex);
                entered = true;
            }
            gateChanged.notify_one();
            std::unique_lock lock(gateMutex);
            gateChanged.wait(lock, [&] { return release; });
        });
        externalThread = std::thread([&] { ioContext.run(); });
        {
            std::unique_lock lock(gateMutex);
            gateChanged.wait(lock, [&] { return entered; });
        }
    }

    bool duplicateRejectedWhileCleanupIsPending = false;
    try {
        auto duplicate = ruvia::attachEventLoop(ioContext);
    } catch (const std::invalid_argument&) {
        duplicateRejectedWhileCleanupIsPending = true;
    }

    {
        const std::lock_guard lock(gateMutex);
        release = true;
    }
    gateChanged.notify_one();
    externalThread.join();
    stopRegistration.reset();

    bool reattachedAfterCleanup = false;
    try {
        ioContext.restart();
        auto replacement = ruvia::attachEventLoop(ioContext);
        replacement.stop();
        replacement.run();
        reattachedAfterCleanup = true;
    } catch (...) {
    }
    return duplicateRejectedWhileCleanupIsPending &&
           stopCallbackRan.load(std::memory_order_acquire) && reattachedAfterCleanup;
}

bool testExternalAttachmentHandlesContextDestruction() {
    auto ioContext = std::make_unique<asio::io_context>();
    {
        // Register the timer service before Ruvia's external-context service;
        // shutdown must remain safe regardless of Asio service registration
        // order.
        asio::steady_timer timer(*ioContext);
    }
    auto attachment = ruvia::attachEventLoop(*ioContext);
    const auto loop = attachment.loop();
    ioContext.reset();

    bool ioContextRejected = false;
    try {
        static_cast<void>(loop.ioContext());
    } catch (const std::logic_error&) {
        ioContextRejected = true;
    }
    bool executorRejected = false;
    try {
        static_cast<void>(loop.executor());
    } catch (const std::logic_error&) {
        executorRejected = true;
    }
    const bool postRejected = loop.post([] {}) == ruvia::PostStatus::kWorkerStopping;
    attachment.stop();
    return ioContextRejected && executorRejected && postRejected && !attachment.valid() &&
           !loop.valid();
}

bool testJoinStopsRunningPool() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    loops.start();
    loops.join();
    return !loop.accepting() && loop.post([] {}) == ruvia::PostStatus::kWorkerStopping;
}

bool testFailurePropagation() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stopCallbackRan{false};
    std::atomic_bool stopCallbackOnLoop{false};
    auto stopRegistration = loop.onStop([&]() -> ruvia::Task<void> {
        stopCallbackOnLoop = loop.isCurrent();
        stopCallbackRan = true;
        co_return;
    });
    struct Listener final : ruvia::detail::WorkerShutdownListener {
        void workerStopping() noexcept override {
            notified = true;
        }
        bool notified{false};
    };
    const auto listener = std::make_shared<Listener>();
    ruvia::detail::WorkerHandleAccess::registerShutdownListener(loop.handle(), listener);
    if (loop.post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::PostStatus::kAccepted) {
        return false;
    }
    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stopRegistration.valid() && stopCallbackRan && stopCallbackOnLoop &&
               listener->notified && std::string_view(error.what()) == "posted task failed";
    }
    return false;
}

bool testJoinBeforeStartDrainsOnOwners() {
    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 1});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic<unsigned> taskCalls{0};
    std::atomic<unsigned> stopCalls{0};
    std::atomic_bool tasksOnOwners{true};
    std::atomic_bool stopsOnOwners{true};

    auto firstStop = first.onStop([&]() -> ruvia::Task<void> {
        if (!first.isCurrent()) {
            stopsOnOwners.store(false, std::memory_order_relaxed);
        }
        stopCalls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    auto secondStop = second.onStop([&]() -> ruvia::Task<void> {
        if (!second.isCurrent()) {
            stopsOnOwners.store(false, std::memory_order_relaxed);
        }
        stopCalls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    if (first.post([&] {
            if (!first.isCurrent()) {
                tasksOnOwners.store(false, std::memory_order_relaxed);
            }
            taskCalls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::PostStatus::kAccepted ||
        second.post([&] {
            if (!second.isCurrent()) {
                tasksOnOwners.store(false, std::memory_order_relaxed);
            }
            taskCalls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.join();
    const bool rejectedAfterJoin = first.post([] {}) == ruvia::PostStatus::kWorkerStopping &&
                                   second.post([] {}) == ruvia::PostStatus::kWorkerStopping;
    return firstStop.valid() && secondStop.valid() && rejectedAfterJoin &&
           taskCalls.load(std::memory_order_relaxed) == 2 &&
           stopCalls.load(std::memory_order_relaxed) == 2 &&
           tasksOnOwners.load(std::memory_order_relaxed) &&
           stopsOnOwners.load(std::memory_order_relaxed);
}

bool testStopBeforeStartPropagatesFailure() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stopOnOwner{false};
    auto stopRegistration = loop.onStop([&]() -> ruvia::Task<void> {
        stopOnOwner.store(loop.isCurrent(), std::memory_order_release);
        co_return;
    });
    if (loop.post([] { throw std::runtime_error("pre-start task failed"); }) !=
        ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.stop();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stopRegistration.valid() && stopOnOwner.load(std::memory_order_acquire) &&
               std::string_view(error.what()) == "pre-start task failed";
    }
    return false;
}

bool testJoinRejectsPoolWorker() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::promise<bool> completed;
    auto result = completed.get_future();
    if (loop.post([&] {
            bool rejected = false;
            try {
                loops.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join an event loop pool from one of its workers";
            }
            completed.set_value(rejected && loop.isCurrent());
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    const bool rejected = result.get();
    loops.stop();
    loops.join();
    return rejected;
}

bool testExecutorFailureDrainsShutdownOnOwners() {
    struct AbandonProbe final {
        explicit AbandonProbe(std::atomic_bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
        ~AbandonProbe() {
            destroyed_->store(true, std::memory_order_release);
        }
        std::atomic_bool* destroyed_;
    };

    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 2});
    const auto failedLoop = loops.loop(0);
    const auto peerLoop = loops.loop(1);
    std::atomic<unsigned> failedStopCalls{0};
    std::atomic<unsigned> peerStopCalls{0};
    std::atomic_bool failedStopOnOwner{false};
    std::atomic_bool peerStopOnOwner{false};
    std::atomic_bool shutdownContinuationDrained{false};
    std::atomic_bool abandonedMailboxRan{false};
    std::atomic_bool abandonedMailboxDestroyed{false};

    auto failedStop = failedLoop.onStop([&]() -> ruvia::Task<void> {
        failedStopOnOwner.store(failedLoop.isCurrent(), std::memory_order_release);
        failedStopCalls.fetch_add(1, std::memory_order_relaxed);
        asio::post(failedLoop.ioContext(),
            [] { throw std::runtime_error("secondary shutdown handler failed"); });
        asio::post(failedLoop.ioContext(), [&] {
            shutdownContinuationDrained.store(failedLoop.isCurrent(), std::memory_order_release);
        });
        co_return;
    });
    auto peerStop = peerLoop.onStop([&]() -> ruvia::Task<void> {
        peerStopOnOwner.store(peerLoop.isCurrent(), std::memory_order_release);
        peerStopCalls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    asio::post(failedLoop.ioContext(), [] { throw std::runtime_error("executor handler failed"); });
    if (failedLoop.post([probe = std::make_unique<AbandonProbe>(abandonedMailboxDestroyed),
                            &abandonedMailboxRan] {
            abandonedMailboxRan.store(true, std::memory_order_release);
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return failedStop.valid() && peerStop.valid() &&
               std::string_view(error.what()) == "executor handler failed" &&
               failedStopCalls.load(std::memory_order_relaxed) == 1 &&
               peerStopCalls.load(std::memory_order_relaxed) == 1 &&
               failedStopOnOwner.load(std::memory_order_acquire) &&
               peerStopOnOwner.load(std::memory_order_acquire) &&
               shutdownContinuationDrained.load(std::memory_order_acquire) &&
               !abandonedMailboxRan.load(std::memory_order_acquire) &&
               abandonedMailboxDestroyed.load(std::memory_order_acquire);
    }
    return false;
}

bool testExpiredHandle() {
    ruvia::EventLoop loop;
    {
        ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
        loop = loops.loop(0);
    }
    bool contextRejected = false;
    try {
        static_cast<void>(loop.ioContext());
    } catch (const std::logic_error&) {
        contextRejected = true;
    }
    return !loop.valid() && !loop.accepting() && contextRejected &&
           loop.post([] {}) == ruvia::PostStatus::kWorkerStopping;
}

bool testEscapedWorkerHandleBecomesDetachedEndpoint() {
    ruvia::WorkerHandle worker;
    ruvia::WorkerId liveId = 0;
    {
        ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
        worker = loops.loop(0).handle();
        liveId = worker.id();
        if (!worker.valid() || liveId == 0) {
            return false;
        }
    }

    bool internalDeferRejected = false;
    try {
        ruvia::detail::WorkerHandleAccess::defer(worker, [] {});
    } catch (const std::runtime_error&) {
        internalDeferRejected = true;
    }
    return !worker.valid() && !worker.accepting() && !worker.isCurrent() && worker.id() == 0 &&
           worker.post([] {}) == ruvia::PostStatus::kWorkerStopping && internalDeferRejected;
}

bool testFailureDestroysAbandonedMailboxTasks() {
    struct DestructionProbe final {
        explicit DestructionProbe(bool& value) noexcept
            : destroyed(&value) {}
        bool* destroyed;
        ~DestructionProbe() {
            *destroyed = true;
        }
    };

    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool queuedTaskDestroyed = false;
    if (worker.post([] { throw std::runtime_error("stop mailbox drain"); }) !=
            ruvia::PostStatus::kAccepted ||
        worker.post([probe = std::make_unique<DestructionProbe>(queuedTaskDestroyed)] {}) !=
            ruvia::PostStatus::kAccepted) {
        return false;
    }
    try {
        ioContext.run();
    } catch (const std::runtime_error&) {
    }
    if (!queuedTaskDestroyed) {
        return false;
    }
    dispatcher->detachContext();
    return queuedTaskDestroyed && !worker.valid();
}

bool testAbandonedRootTaskCompletesWhenMailboxDrainFails() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 8});
    const auto loop = loops.loop(0);
    if (loop.post([] { throw std::runtime_error("boom"); }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    auto root = loop.start([]() -> ruvia::Task<int> { co_return 42; }());
    loops.start();

    bool joinThrew = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        joinThrew = std::string_view(error.what()) == "boom";
    } catch (...) {
    }
    if (!joinThrew) {
        return false;
    }

    try {
        static_cast<void>(root.get());
        return false;
    } catch (const std::runtime_error& error) {
        return std::string_view(error.what()) == "event loop is stopping";
    } catch (...) {
        return false;
    }
}

bool testDispatcherLifecycleHooksAreWorkerAffine() {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 1);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool startupOnWorker = false;
    bool failureOnWorker = false;
    bool shutdownOnWorker = false;
    bool receivedStartupFailure = false;

    dispatcher->runContext(
        [&] {
            startupOnWorker = worker.isCurrent();
            throw std::runtime_error("worker startup failed");
        },
        [&](std::exception_ptr failure) noexcept {
            failureOnWorker = worker.isCurrent();
            try {
                std::rethrow_exception(failure);
            } catch (const std::runtime_error& error) {
                receivedStartupFailure = std::string_view(error.what()) == "worker startup failed";
            } catch (...) {
            }
        },
        [&]() noexcept { shutdownOnWorker = worker.isCurrent(); });
    dispatcher->detachContext();
    return startupOnWorker && failureOnWorker && shutdownOnWorker && receivedStartupFailure;
}

// A stop callback runs after every caller that could have received its
// exception is gone. Dropping it would make a failed cleanup invisible, so the
// pool records it as its first failure and join() rethrows it.
bool testStopCallbackFactoryFailureDoesNotSkipOtherCallbacks() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);
    std::atomic_bool coroutineCallbackRan{false};
    auto factoryFailure = loop.onStop([]() -> ruvia::Task<void> {
        throw std::runtime_error("stop factory failed");
    });
    auto coroutineCallback = loop.onStop([&]() -> ruvia::Task<void> {
        coroutineCallbackRan.store(true, std::memory_order_release);
        co_return;
    });

    loops.start();
    loops.stop();
    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop factory failed";
    } catch (...) {
    }
    return rethrown && coroutineCallbackRan.load(std::memory_order_acquire) &&
           factoryFailure.valid() && coroutineCallback.valid();
}

bool testStopCallbackFailureReachesJoin() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    const auto loop = loops.loop(0);
    std::atomic<unsigned> stopCalls{0};
    auto stopRegistration = loop.onStop([&]() -> ruvia::Task<void> {
        stopCalls.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("stop callback failed");
        co_return;
    });

    loops.start();
    loops.stop();

    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop callback failed";
    } catch (...) {
    }
    return rethrown && stopCalls.load(std::memory_order_relaxed) == 1;
}

bool testStopListenerArrivalDuringShutdownNotificationBatch() {
    struct BlockingListener final : ruvia::detail::WorkerShutdownListener {
        BlockingListener(std::promise<void>& enteredPromise,
            std::shared_future<void> releaseSignal) noexcept
            : entered(&enteredPromise),
              release(std::move(releaseSignal)) {}

        std::promise<void>* entered;
        std::shared_future<void> release;

        void workerStopping() noexcept override {
            entered->set_value();
            release.wait();
        }
    };

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);
    std::promise<void> listenerEntered;
    auto listenerEnteredResult = listenerEntered.get_future();
    std::promise<void> releaseListener;
    auto blockingListener = std::make_shared<BlockingListener>(
        listenerEntered, releaseListener.get_future().share());
    ruvia::detail::WorkerHandleAccess::registerShutdownListener(loop.handle(), blockingListener);

    std::atomic_bool stopCallbackRan{false};
    auto stopCallback = loop.onStop([&]() -> ruvia::Task<void> {
        stopCallbackRan.store(true, std::memory_order_release);
        co_return;
    });
    asio::post(loop.ioContext(), [] { throw std::runtime_error("first shutdown trigger"); });
    loops.start();
    if (listenerEnteredResult.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        releaseListener.set_value();
        loops.join();
        return false;
    }

    loops.stop();
    const bool loopStillAttachedDuringBatch = loop.valid();
    bool rootAdmissionClosed = false;
    try {
        static_cast<void>(loop.start(failedAbandonedAttachmentRoot()));
    } catch (const std::runtime_error&) {
        rootAdmissionClosed = true;
    }
    releaseListener.set_value();
    bool failureRethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        failureRethrown = std::string_view(error.what()) == "first shutdown trigger";
    } catch (...) {
    }
    return loopStillAttachedDuringBatch && rootAdmissionClosed && failureRethrown &&
           stopCallbackRan.load(std::memory_order_acquire) && stopCallback.valid() && !loop.valid();
}

bool testAsyncStopCallbacksStartTogetherAndKeepCapturesAlive() {
    struct Callback final {
        std::atomic<unsigned>* started;
        std::atomic<unsigned>* finished;
        ruvia::WorkerSignal* signal;
        bool notifier;
        int value;
        int* observed;

        ruvia::Task<void> operator()() {
            started->fetch_add(1, std::memory_order_release);
            if (notifier) {
                signal->notify();
            } else {
                co_await signal->wait();
            }
            *observed += value;
            finished->fetch_add(1, std::memory_order_release);
            co_return;
        }
    };

    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> finished{0};
    ruvia::WorkerSignal signal(worker);
    int observed = 0;
    auto first = loop.onStop(Callback{&started, &finished, &signal, false, 1, &observed});
    auto second = loop.onStop(Callback{&started, &finished, &signal, true, 2, &observed});
    loops.start();
    loops.stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    first.reset();
    second.reset();
    loops.join();
    return started.load(std::memory_order_acquire) == 2 &&
           finished.load(std::memory_order_acquire) == 2 && observed == 3;
}

ruvia::Task<void> waitForShutdownTimer(const ruvia::WorkerHandle& worker, bool& childDone) {
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::hours(1)));
    childDone = true;
    co_return;
}

ruvia::Task<void> nestedShutdownScope(const ruvia::WorkerHandle& worker, bool& childDone,
    bool& middleDone) {
    ruvia::TaskScope scope(worker);
    scope.spawn(waitForShutdownTimer(worker, childDone));
    co_await scope.join();
    middleDone = true;
}

ruvia::Task<void> rootShutdownScope(const ruvia::WorkerHandle& worker,
    std::promise<void>& started, bool& childDone, bool& middleDone, bool& rootDone) {
    ruvia::TaskScope scope(worker);
    scope.spawn(nestedShutdownScope(worker, childDone, middleDone));
    started.set_value();
    co_await scope.join();
    rootDone = true;
}

struct RootGetMoveProbe final {
    std::optional<ruvia::RootTask<RootGetMoveProbe>>* owner{};
    std::optional<ruvia::RootTask<RootGetMoveProbe>>* replacement{};
    std::atomic_bool* trigger{};
    bool* validDuringMove{};

    RootGetMoveProbe() = default;
    RootGetMoveProbe(std::optional<ruvia::RootTask<RootGetMoveProbe>>* rootOwner,
        std::optional<ruvia::RootTask<RootGetMoveProbe>>* rootReplacement,
        std::atomic_bool* moveTrigger, bool* observedValidity)
        : owner(rootOwner),
          replacement(rootReplacement),
          trigger(moveTrigger),
          validDuringMove(observedValidity) {}
    RootGetMoveProbe(RootGetMoveProbe&& other) noexcept
        : owner(other.owner),
          replacement(other.replacement),
          trigger(other.trigger),
          validDuringMove(other.validDuringMove) {
        if (trigger != nullptr && trigger->exchange(false, std::memory_order_acq_rel)) {
            *validDuringMove = owner->value().valid();
            if (replacement != nullptr) {
                *owner = std::move(*replacement);
            }
        }
    }
    RootGetMoveProbe& operator=(RootGetMoveProbe&&) = delete;
    RootGetMoveProbe(const RootGetMoveProbe&) = delete;
    RootGetMoveProbe& operator=(const RootGetMoveProbe&) = delete;
};

ruvia::Task<RootGetMoveProbe> returnRootGetMoveProbe(RootGetMoveProbe value) {
    co_return std::move(value);
}

struct RootPmrResult final {
    explicit RootPmrResult(std::pmr::memory_resource* resource)
        : values(resource) {
        values.resize(32, 42);
    }
    RootPmrResult(RootPmrResult&&) noexcept = default;
    RootPmrResult(const RootPmrResult&) = delete;
    std::pmr::vector<int> values;
};

ruvia::Task<RootPmrResult> makeRootPmrResult(std::pmr::memory_resource* resource) {
    co_return RootPmrResult(resource);
}

bool testRootTaskResultMovesOutsideLockAndPreservesReentrantOwner() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 8});
    const auto loop = loops.loop(0);
    std::optional<ruvia::RootTask<RootGetMoveProbe>> root;
    std::optional<ruvia::RootTask<RootGetMoveProbe>> replacement;
    std::atomic_bool trigger{false};
    bool validDuringMove = true;
    root.emplace(loop.start(returnRootGetMoveProbe(
        RootGetMoveProbe{&root, &replacement, &trigger, &validDuringMove})));
    replacement.emplace(loop.start(returnRootGetMoveProbe(
        RootGetMoveProbe{&root, nullptr, &trigger, &validDuringMove})));
    loops.start();
    root->wait();
    replacement->wait();
    trigger.store(true, std::memory_order_release);
    static_cast<void>(root->get());
    const bool replacementSurvived = root->valid();
    if (replacementSurvived) {
        static_cast<void>(root->get());
    }
    loops.join();
    return !validDuringMove && replacementSurvived && !root->valid();
}

ruvia::Task<const int> immutable_root_result(const ruvia::WorkerHandle& worker) {
    co_return worker.isCurrent() ? 17 : -1;
}

bool test_root_task_delivers_const_result() {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto worker = loops.loop(0).handle();
    auto root = loops.loop(0).start(immutable_root_result(worker));
    loops.start();
    const auto value = root.get();
    loops.stop();
    loops.join();
    return value == 17 && !root.valid();
}

bool testRootTaskResultOwnsPmrStoragePastPoolRetirement() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 4});
    const auto loop = loops.loop(0);
    CountingResource resource;
    auto root = loop.start(makeRootPmrResult(&resource));
    loops.start();
    root.wait();
    loops.stop();
    loops.join();
    bool retained = false;
    {
        auto result = root.get();
        const auto payload_bytes = result.values.capacity() * sizeof(int);
        const bool payload_retained = resource.owns(result.values.data(), payload_bytes);
        const bool values_valid = result.values.size() == 32 &&
                                  std::all_of(result.values.begin(), result.values.end(),
                                      [](int value) { return value == 42; });
        retained = values_valid && result.values.get_allocator().resource() == &resource &&
                   payload_retained && resource.live_allocations() > 0;
    }
    return retained && resource.live_allocations() == 0 &&
           resource.allocations == resource.deallocations;
}

bool testRootTasksJoinNestedScopesDuringStop() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 8});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    std::promise<void> started;
    auto startedResult = started.get_future();
    bool childDone = false;
    bool middleDone = false;
    bool rootDone = false;
    auto root = loop.start(rootShutdownScope(
        worker, started, childDone, middleDone, rootDone));
    loops.start();
    if (startedResult.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    loops.stop();
    loops.join();
    try {
        root.get();
    } catch (...) {
        return false;
    }
    return childDone && middleDone && rootDone;
}

bool testLifecycleTransitionsAreMonotonic() {
    using Lifecycle = ruvia::RuntimeLifecycle;
    Lifecycle lifecycle;
    lifecycle.completeStop();
    if (lifecycle.state() != Lifecycle::State::kReady || !lifecycle.start()) {
        return false;
    }
    lifecycle.completeStop();
    if (lifecycle.state() != Lifecycle::State::kRunning || lifecycle.start() ||
        !lifecycle.requestStop() || lifecycle.state() != Lifecycle::State::kStopping ||
        lifecycle.requestStop()) {
        return false;
    }

    lifecycle.completeStop();
    return lifecycle.state() == Lifecycle::State::kStopped && !lifecycle.requestStop() &&
           lifecycle.state() == Lifecycle::State::kStopped && !lifecycle.start();
}

bool testConcurrentStopHasOneInitiator() {
    using Lifecycle = ruvia::RuntimeLifecycle;
    constexpr std::size_t kThreadCount = 16;
    Lifecycle lifecycle;
    if (!lifecycle.start()) {
        return false;
    }

    std::atomic<std::size_t> initiators{0};
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool start = false;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    try {
        for (std::size_t i = 0; i < kThreadCount; ++i) {
            threads.emplace_back([&] {
                {
                    std::unique_lock lock(gateMutex);
                    gateChanged.wait(lock, [&] { return start; });
                }
                if (lifecycle.requestStop()) {
                    initiators.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    } catch (...) {
        {
            std::lock_guard lock(gateMutex);
            start = true;
        }
        gateChanged.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }
    {
        std::lock_guard lock(gateMutex);
        start = true;
    }
    gateChanged.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    lifecycle.completeStop();
    return initiators.load(std::memory_order_relaxed) == 1 &&
           lifecycle.state() == Lifecycle::State::kStopped && !lifecycle.requestStop();
}

}  // namespace

int main() {
    const auto run = [](const char* name, bool (*test)()) {
        std::printf("[ RUN ] %s\n", name);
        std::fflush(stdout);
        const bool passed = test();
        std::printf("[%s] %s\n", passed ? " ok " : "FAIL", name);
        std::fflush(stdout);
        return passed;
    };
    return run("event_loop_post_borrow_and_rejected_callable_ownership",
               testEventLoopPostBorrowAndRejectedCallableOwnership) &&
                   run("event_loop_post_protects_reentrant_inline_move",
                       testEventLoopPostProtectsReentrantInlineMove) &&
                   run("event_loop_post_protects_reentrant_heap_copy",
                       testEventLoopPostProtectsReentrantHeapCopy) &&
                   run("worker_handle_callable_lifetime", testWorkerHandleCallableLifetime) &&
                   run("worker_submission_view_lifecycle_and_rejection",
                       testWorkerSubmissionViewLifecycleAndRejection) &&
                   run("post_outcome_invariants_and_empty_callbacks",
                       testPostOutcomeInvariantsAndEmptyCallbacks) &&
                   run("worker_runtime_context_owns_stable_detached_endpoint",
                       testWorkerRuntimeContextOwnsStableDetachedEndpoint) &&
                   run("mailbox_callable_destruction_can_inspect_worker",
                       testMailboxCallableDestructionCanInspectWorker) &&
                   run("mailbox_factory_rollback_and_detach",
                       testMailboxFactoryRollbackAndDetach) &&
                   run("mailbox_factory_can_finish_after_detach",
                       testMailboxFactoryCanFinishAfterDetach) &&
                   run("worker_signal_is_worker_affine", testWorkerSignalIsWorkerAffine) &&
                   run("worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake",
                       test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake) &&
                   run("worker_signal_has_no_waiter_limit",
                       testWorkerSignalHasNoArbitraryWaiterLimit) &&
                   run("worker_signal_rechecks_cold_wait_affinity",
                       testWorkerSignalRechecksAffinityWhenColdWaitStarts) &&
                   run("dispatch_and_affinity", testDispatchAndAffinity) &&
                   run("bounded_mailbox", testBoundedMailbox) &&
                   run("cancellation_with_full_mailbox", [] { return test_cancellation_reaches_worker_with_saturated_or_closed_mailbox(false); }) &&
                   run("cancellation_with_closed_mailbox", [] { return test_cancellation_reaches_worker_with_saturated_or_closed_mailbox(true); }) &&
                   run("queued_cancellation_after_owner_retirement", test_queued_cancellation_releases_mailbox_after_owner_retirement) &&
                   run("cancellation_after_endpoint_detach", test_cancellation_after_endpoint_detach_does_not_touch_retired_owner) &&
                   run("external_event_loop_attachment", testExternalEventLoopAttachment) &&
                   run("attachment_run_failure_retires_through_async_cleanup",
                       testAttachmentRunFailureRetiresThroughAsyncCleanup) &&
                   run("abandoned_attachment_root_failure_retires_native_run",
                       [] { return testAbandonedAttachmentRootFailureRetires(false); }) &&
                   run("abandoned_attachment_root_failure_retires_attachment_run",
                       [] { return testAbandonedAttachmentRootFailureRetires(true); }) &&
                   run("pool_reported_failure_stops_every_loop_and_join_rethrows",
                       testPoolReportedFailureStopsEveryLoopAndJoinRethrows) &&
                   run("pool_failure_handoff_during_destructor_report",
                       testPoolFailureHandoffDuringDestructorReport) &&
                   run("pool_failure_report_works_after_mailbox_closure",
                       testPoolFailureReportWorksAfterMailboxClosure) &&
                   run("attachment_report_failure_preserves_native_run_ownership",
                       testAttachmentReportFailureKeepsNativeRunOwnership) &&
                   run("external_attachment_retains_state_until_cleanup",
                       testExternalAttachmentRetainsStateUntilContextCleanup) &&
                   run("external_attachment_handles_context_destruction",
                       testExternalAttachmentHandlesContextDestruction) &&
                   run("join_stops_running_pool", testJoinStopsRunningPool) &&
                   run("failure_propagation", testFailurePropagation) &&
                   run("join_before_start_drains_on_owners", testJoinBeforeStartDrainsOnOwners) &&
                   run("stop_before_start_propagates_failure",
                       testStopBeforeStartPropagatesFailure) &&
                   run("join_rejects_pool_worker", testJoinRejectsPoolWorker) &&
                   run("executor_failure_drains_shutdown_on_owners",
                       testExecutorFailureDrainsShutdownOnOwners) &&
                   run("expired_handle", testExpiredHandle) &&
                   run("escaped_worker_handle_detaches",
                       testEscapedWorkerHandleBecomesDetachedEndpoint) &&
                   run("failure_destroys_abandoned_mailbox_tasks",
                       testFailureDestroysAbandonedMailboxTasks) &&
                   run("abandoned_root_task_completes_when_mailbox_drain_fails",
                       testAbandonedRootTaskCompletesWhenMailboxDrainFails) &&
                   run("dispatcher_lifecycle_hooks_are_worker_affine",
                       testDispatcherLifecycleHooksAreWorkerAffine) &&
                   run("stop_callback_failure_reaches_join", testStopCallbackFailureReachesJoin) &&
                   run("stop_callback_factory_failure_keeps_other_callbacks_running",
                       testStopCallbackFactoryFailureDoesNotSkipOtherCallbacks) &&
                   run("stop_listener_arrival_waits_for_shutdown_notification_batch",
                       testStopListenerArrivalDuringShutdownNotificationBatch) &&
                   run("async_stop_callbacks_start_together_and_keep_captures_alive",
                       testAsyncStopCallbacksStartTogetherAndKeepCapturesAlive) &&
                   run("root_tasks_join_nested_scopes_during_stop",
                       testRootTasksJoinNestedScopesDuringStop) &&
                   run("root_result_moves_are_reentrant_and_preserve_owner",
                       testRootTaskResultMovesOutsideLockAndPreservesReentrantOwner) &&
                   run("root_result_owns_pmr_storage_past_pool_retirement",
                       testRootTaskResultOwnsPmrStoragePastPoolRetirement) &&
                   run("root_task_delivers_const_result",
                       test_root_task_delivers_const_result) &&
                   run("lifecycle_transitions_are_monotonic",
                       testLifecycleTransitionsAreMonotonic) &&
                   run("concurrent_stop_has_one_initiator", testConcurrentStopHasOneInitiator)
               ? 0
               : 1;
}
