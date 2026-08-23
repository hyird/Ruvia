#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>

#include <concepts>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

template <typename T>
concept PubliclyAdaptableTask = requires(ruvia::Task<T> task) {
    { ruvia::asAwaitable(std::move(task)) } -> std::same_as<asio::awaitable<T>>;
};

template <typename T>
concept AdaptableTaskLvalue = requires(ruvia::Task<T>& task) {
    ruvia::asAwaitable(task);
};

template <typename T>
concept InternallyAdaptableTask = requires(ruvia::Task<T> task) {
    ruvia::detail::taskAsAwaitable(std::move(task));
};

struct ThrowingMove final {
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
};

static_assert(PubliclyAdaptableTask<int>);
static_assert(PubliclyAdaptableTask<void>);
static_assert(!PubliclyAdaptableTask<ThrowingMove>);
static_assert(!ruvia::detail::AsioTaskResult<ThrowingMove>);
static_assert(!InternallyAdaptableTask<ThrowingMove>);
static_assert(!AdaptableTaskLvalue<int>);
static_assert(!AdaptableTaskLvalue<void>);
static_assert(std::same_as<decltype(std::declval<const ruvia::EventLoop&>().start(std::declval<ruvia::Task<int>>())), ruvia::RootTask<int>>);

ruvia::Task<std::unique_ptr<int>> makeValue(ruvia::WorkerHandle worker) {
    if (!worker.isCurrent()) {
        throw std::logic_error("task started outside its event loop");
    }
    co_return std::make_unique<int>(42);
}

ruvia::Task<void> completeVoid(ruvia::WorkerHandle worker, bool& completed) {
    if (!worker.isCurrent()) {
        throw std::logic_error("task started outside its event loop");
    }
    completed = true;
    co_return;
}

ruvia::Task<void> fail() {
    throw std::runtime_error("root task failure");
    co_return;
}

}  // namespace

int main() {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    bool voidCompleted = false;

    auto value = loop.start(makeValue(worker));
    auto noValue = loop.start(completeVoid(worker, voidCompleted));
    auto failure = loop.start(fail());
    std::promise<bool> sameLoopGetRejected;
    auto sameLoopGetResult = sameLoopGetRejected.get_future();
    const auto probePosted = loop.post([&] {
        try {
            static_cast<void>(value.get());
            sameLoopGetRejected.set_value(false);
        } catch (const std::logic_error&) {
            sameLoopGetRejected.set_value(value.valid());
        }
    });
    if (!probePosted.accepted()) {
        return 1;
    }

    loops.start();
    bool valid = false;
    try {
        const auto sameLoopGuardHeld = sameLoopGetResult.get();
        auto result = value.get();
        noValue.get();
        bool failureObserved = false;
        try {
            failure.get();
        } catch (const std::runtime_error& error) {
            failureObserved = std::string_view(error.what()) == "root task failure";
        }
        valid = sameLoopGuardHeld && result != nullptr && *result == 42 && voidCompleted && failureObserved;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    if (!valid) {
        return 1;
    }

    ruvia::EventLoopPool abandonedPool({.loopCount = 1});
    {
        auto unobserved = abandonedPool.loop(0).start(fail());
    }
    abandonedPool.start();
    abandonedPool.stop();
    try {
        abandonedPool.join();
    } catch (const std::runtime_error& error) {
        return std::string_view(error.what()) == "root task failure" ? 0 : 2;
    }
    return 3;
}
