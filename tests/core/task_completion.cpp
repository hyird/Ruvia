#include <concepts>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/AsioAwait.h"

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

struct FrameAllocation final {
    explicit FrameAllocation(AllocationCounts& counts)
        : values(256, 0, CountingAllocator<int>(counts)) {}

    std::vector<int, CountingAllocator<int>> values;
};

ruvia::Task<void> completeVoidWithFrame(FrameAllocation frame) {
    frame.values.resize(256);
    co_return;
}

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

bool hasFailureMessage(
    const ruvia::detail::TaskCompletionFailure& failure, std::string_view expected) {
    try {
        std::rethrow_exception(failure.exception());
    } catch (const std::runtime_error& error) {
        return error.what() == expected;
    } catch (...) {
        return false;
    }
}

template <typename T>
ruvia::Task<T> finishWithFrame(FrameAllocation frame, bool fail) {
    frame.values.front() = 42;
    if (fail) {
        throw std::runtime_error("frame failure");
    }
    if constexpr (std::is_void_v<T>) {
        co_return;
    } else {
        co_return frame.values.front();
    }
}

template <typename T>
bool checkFrameCompletion(bool fail) {
    AllocationCounts counts;
    asio::io_context context(1);
    bool called = false;
    bool valid = false;
    ruvia::detail::asyncStartTask(
        finishWithFrame<T>(FrameAllocation(counts), fail),
        asio::bind_executor(context.get_executor(),
            asio::bind_allocator(CountingAllocator<std::byte>(counts),
                [&](auto result) {
                    called = true;
                    valid = counts.allocations != 0 && counts.allocations == counts.deallocations;
                    if (fail) {
                        valid = valid && result.failure() != nullptr &&
                                hasFailureMessage(*result.failure(), "frame failure");
                    } else {
                        valid = valid && result.success() != nullptr;
                        if constexpr (!std::is_void_v<T>) {
                            if (result.success() != nullptr) {
                                valid = valid && std::move(*result.success()).takeValue() == 42;
                            }
                        }
                    }
                })));
    context.run();
    return called && valid && counts.allocations == counts.deallocations;
}

struct ValueDeleter final {
    AllocationCounts* counts{nullptr};

    void operator()(int* value) const noexcept {
        std::destroy_at(value);
        CountingAllocator<int>(*counts).deallocate(value, 1);
    }
};

using CountedValue = std::unique_ptr<int, ValueDeleter>;

ruvia::Task<CountedValue> makeRetainedValue(FrameAllocation frame, AllocationCounts& values) {
    auto* storage = CountingAllocator<int>(values).allocate(1);
    std::construct_at(storage, static_cast<int>(frame.values.size()));
    co_return CountedValue(storage, ValueDeleter{&values});
}

bool checkRetainedValue() {
    AllocationCounts frames;
    AllocationCounts values;
    asio::io_context context(1);
    CountedValue retained;
    bool valid = true;
    ruvia::detail::asyncStartTask(makeRetainedValue(FrameAllocation(frames), values),
        asio::bind_executor(context.get_executor(),
            asio::bind_allocator(CountingAllocator<std::byte>(frames),
                [&](auto result) {
                    valid = frames.allocations == frames.deallocations && result.success() != nullptr;
                    if (result.success() != nullptr) {
                        retained = std::move(*result.success()).takeValue();
                    }
                    valid = valid && values.allocations == 1 && values.deallocations == 0;
                })));
    context.run();
    valid = checkFrameCompletion<int>(false) && valid;
    valid = valid && retained != nullptr && *retained == 256 && values.deallocations == 0;
    retained.reset();
    return valid && frames.allocations == frames.deallocations && values.allocations == values.deallocations;
}

}  // namespace

int main() {
    asio::io_context ioContext(1);
    int completed = 0;
    bool valid = true;

    ruvia::detail::asyncStartTask(makeValue(),
        asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
            auto* success = result.success();
            valid = valid && success != nullptr && result.failure() == nullptr;
            if (success != nullptr) {
                auto value = std::move(*success).takeValue();
                valid = valid && value != nullptr && *value == 42;
            }
            ++completed;
        }));
    ruvia::detail::asyncStartTask(failValue(),
        asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
            const auto* failure = result.failure();
            valid = valid && result.success() == nullptr && failure != nullptr;
            if (failure != nullptr) {
                valid = valid && hasFailureMessage(*failure, "value failure");
            }
            ++completed;
        }));
    ruvia::detail::asyncStartTask(completeVoid(),
        asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
            valid = valid && result.success() != nullptr && result.failure() == nullptr;
            ++completed;
        }));
    ruvia::detail::asyncStartTask(failVoid(),
        asio::bind_executor(ioContext.get_executor(), [&completed, &valid](auto result) {
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
    ruvia::detail::asyncStartTask(
        completeVoidWithFrame(FrameAllocation(successCounts)),
        asio::bind_executor(allocatedContext.get_executor(),
            asio::bind_allocator(CountingAllocator<std::byte>(successCounts),
                [&completed, &valid, &successCounts](auto result) {
                    const bool released = successCounts.allocations == successCounts.deallocations;
                    if (!released) {
                        std::fprintf(stderr, "completion retained %zu intermediate allocations\n",
                            successCounts.allocations - successCounts.deallocations);
                    }
                    valid = valid && released;
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
    ruvia::detail::asyncStartTask(
        completeVoidWithFrame(FrameAllocation(throwingCounts)), asio::bind_executor(throwingContext.get_executor(),
                                                                    asio::bind_allocator(CountingAllocator<std::byte>(throwingCounts),
                                                                        [&throwingCounts, &valid](auto) {
                                                                            valid = valid && throwingCounts.allocations == throwingCounts.deallocations;
                                                                            throw std::runtime_error("completion failed");
                                                                        })));
    bool throwingHandlerObserved = false;
    try {
        throwingContext.run();
    } catch (const std::runtime_error& error) {
        throwingHandlerObserved = std::string_view(error.what()) == "completion failed";
    }
    valid = valid && throwingHandlerObserved && throwingCounts.allocations != 0 &&
            throwingCounts.allocations == throwingCounts.deallocations;

    valid = checkFrameCompletion<int>(false) && valid;
    valid = checkFrameCompletion<int>(true) && valid;
    valid = checkFrameCompletion<void>(true) && valid;
    valid = checkRetainedValue() && valid;

    AllocationCounts coldCounts;
    {
        auto cold = completeVoidWithFrame(FrameAllocation(coldCounts));
        valid = valid && coldCounts.allocations > coldCounts.deallocations;
    }
    valid = valid && coldCounts.allocations == coldCounts.deallocations;

    return valid && completed == 5 ? 0 : 1;
}
