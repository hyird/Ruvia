#include <ruvia/core/Task.h>
#include <ruvia/core/detail/io/AsioAwait.h>

#include <asio/bind_executor.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/io_context.hpp>

#include <cstddef>
#include <concepts>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

struct AllocationCounts final {
    std::size_t allocations{0};
    std::size_t deallocations{0};
};

template <typename T>
class CountingAllocator {
public:
    using value_type = T;

    explicit CountingAllocator(AllocationCounts& counts) noexcept
        : counts_(&counts) {}

    template <typename U>
    CountingAllocator(const CountingAllocator<U>& other) noexcept
        : counts_(other.counts()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        ++counts_->allocations;
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        ++counts_->deallocations;
        std::allocator<T>{}.deallocate(pointer, count);
    }

    [[nodiscard]] AllocationCounts* counts() const noexcept {
        return counts_;
    }

    template <typename U>
    [[nodiscard]] bool operator==(const CountingAllocator<U>& other) const noexcept {
        return counts_ == other.counts();
    }

private:
    AllocationCounts* counts_;
};

template <typename Result>
concept HasLooseCompletionFields = requires(Result& result) {
    result.exception;
    result.value;
};

template <typename Result>
concept HasRvalueCompletionBorrow = requires(Result&& result) { std::move(result).success(); } || requires(Result&& result) { std::move(result).failure(); };

static_assert(!std::default_initializable<ruvia::detail::TaskCompletionResult<int>>);
static_assert(!std::default_initializable<ruvia::detail::TaskCompletionResult<void>>);
static_assert(!HasLooseCompletionFields<ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasRvalueCompletionBorrow<ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasRvalueCompletionBorrow<ruvia::detail::TaskCompletionResult<void>>);

ruvia::Task<std::unique_ptr<int>> makeValue() {
    co_return std::make_unique<int>(42);
}

ruvia::Task<int> failValue() {
    throw std::runtime_error("value failure");
    co_return 0;
}

ruvia::Task<void> completeVoid() {
    co_return;
}

ruvia::Task<void> failVoid() {
    throw std::runtime_error("void failure");
    co_return;
}

bool hasFailureMessage(const ruvia::detail::TaskCompletionFailure& failure, std::string_view expected) {
    try {
        std::rethrow_exception(failure.exception());
    } catch (const std::runtime_error& error) {
        return error.what() == expected;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main() {
    asio::io_context ioContext(1);
    int completed = 0;
    bool valid = true;

    ruvia::detail::asyncStartTask(makeValue(), asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
        auto* success = result.success();
        valid = valid && success != nullptr && result.failure() == nullptr;
        if (success != nullptr) {
            auto value = std::move(*success).takeValue();
            valid = valid && value != nullptr && *value == 42;
        }
        ++completed;
    }));
    ruvia::detail::asyncStartTask(failValue(), asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
        const auto* failure = result.failure();
        valid = valid && result.success() == nullptr && failure != nullptr;
        if (failure != nullptr) {
            valid = valid && hasFailureMessage(*failure, "value failure");
        }
        ++completed;
    }));
    ruvia::detail::asyncStartTask(completeVoid(), asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
        valid = valid && result.success() != nullptr && result.failure() == nullptr;
        ++completed;
    }));
    ruvia::detail::asyncStartTask(failVoid(), asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
        const auto* failure = result.failure();
        valid = valid && result.success() == nullptr && failure != nullptr;
        if (failure != nullptr) {
            valid = valid && hasFailureMessage(*failure, "void failure");
        }
        ++completed;
    }));

    ioContext.run();

    // The Task-to-Asio bridge is itself an asynchronous operation. Its state
    // and queued delivery must therefore use the completion handler's associated
    // allocator, just like the surrounding co_spawn operation does.
    AllocationCounts successCounts;
    asio::io_context allocatedContext(1);
    ruvia::detail::asyncStartTask(completeVoid(), asio::bind_executor(allocatedContext.get_executor(), asio::bind_allocator(CountingAllocator<std::byte>(successCounts), [&completed](auto result) {
        if (result.success() != nullptr) {
            ++completed;
        }
    })));
    valid = valid && successCounts.allocations != 0;
    allocatedContext.run();
    valid = valid && successCounts.allocations == successCounts.deallocations;

    // User completion code is allowed to propagate through io_context::run().
    // The adapter must still release its state and completed Task frame while
    // that exception unwinds.
    AllocationCounts throwingCounts;
    asio::io_context throwingContext(1);
    ruvia::detail::asyncStartTask(completeVoid(), asio::bind_executor(throwingContext.get_executor(), asio::bind_allocator(CountingAllocator<std::byte>(throwingCounts), [](auto) { throw std::runtime_error("completion failed"); })));
    bool throwingHandlerObserved = false;
    try {
        throwingContext.run();
    } catch (const std::runtime_error& error) {
        throwingHandlerObserved = std::string_view(error.what()) == "completion failed";
    }
    valid = valid && throwingHandlerObserved && throwingCounts.allocations != 0 && throwingCounts.allocations == throwingCounts.deallocations;

    return valid && completed == 5 ? 0 : 1;
}
