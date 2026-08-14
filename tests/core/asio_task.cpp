#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include <concepts>
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

    auto value = asio::co_spawn(loop.executor(), ruvia::asAwaitable(makeValue(worker)), asio::use_future);
    auto noValue = asio::co_spawn(loop.executor(), ruvia::asAwaitable(completeVoid(worker, voidCompleted)), asio::use_future);
    auto failure = asio::co_spawn(loop.executor(), ruvia::asAwaitable(fail()), asio::use_future);

    loops.start();
    bool valid = false;
    try {
        auto result = value.get();
        noValue.get();
        bool failureObserved = false;
        try {
            failure.get();
        } catch (const std::runtime_error& error) {
            failureObserved = std::string_view(error.what()) == "root task failure";
        }
        valid = result != nullptr && *result == 42 && voidCompleted && failureObserved;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    return valid ? 0 : 1;
}
