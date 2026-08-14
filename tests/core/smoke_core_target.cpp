#include "ruvia/core/Task.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/version.h"
#include "ruvia/core/detail/util/NativePath.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <array>
#include <concepts>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <utility>

struct ReturnsVoid final {
    void operator()() {}
};

struct ReturnsInt final {
    int operator()() {
        return 1;
    }
};

struct ReturnsShort final {
    short operator()() {
        return 1;
    }
};

template <typename Fn>
concept WorkerPostable = requires(const ruvia::WorkerHandle& worker, Fn&& fn) {
    { worker.post(std::forward<Fn>(fn)) } -> std::same_as<ruvia::PostResult>;
};

template <typename Fn>
concept EventLoopPostable = requires(const ruvia::EventLoop& loop, Fn&& fn) {
    { loop.post(std::forward<Fn>(fn)) } -> std::same_as<ruvia::PostResult>;
};

template <typename Fn>
concept EventLoopStopCallback = requires(const ruvia::EventLoop& loop, Fn&& fn) {
    { loop.onStop(std::forward<Fn>(fn)) } -> std::same_as<ruvia::EventLoopStopRegistration>;
};

static_assert(std::constructible_from<ruvia::MoveOnlyFunction<void()>, ReturnsVoid>);
static_assert(!std::constructible_from<ruvia::MoveOnlyFunction<void()>, ReturnsInt>);
static_assert(std::constructible_from<ruvia::MoveOnlyFunction<int()>, ReturnsShort>);
static_assert(WorkerPostable<ReturnsVoid>);
static_assert(!WorkerPostable<ReturnsInt>);
static_assert(EventLoopPostable<ReturnsVoid>);
static_assert(!EventLoopPostable<ReturnsInt>);
static_assert(EventLoopStopCallback<ReturnsVoid>);
static_assert(!EventLoopStopCallback<ReturnsInt>);

template <typename T>
concept ExposesRvalueMemoryBorrow = requires(T&& memory) { std::move(memory).resource(); } || requires(T&& memory) { std::move(memory).template allocator<>(); };

template <typename T>
concept ExposesRvalueNativePathBorrow = requires(T&& path) { ruvia::detail::nativePathView(std::move(path)); };

static_assert(!ExposesRvalueMemoryBorrow<ruvia::WorkerMemory>);
static_assert(!ExposesRvalueMemoryBorrow<ruvia::RequestMemory>);
static_assert(!ExposesRvalueNativePathBorrow<std::filesystem::path>);

static_assert(RUVIA_VERSION_MAJOR == RUVIA_EXPECTED_VERSION_MAJOR);
static_assert(RUVIA_VERSION_MINOR == RUVIA_EXPECTED_VERSION_MINOR);
static_assert(RUVIA_VERSION_PATCH == RUVIA_EXPECTED_VERSION_PATCH);
static_assert(std::string_view(RUVIA_VERSION_STRING) == std::string_view(RUVIA_EXPECTED_VERSION_STRING));

ruvia::Task<int> smokeTask() {
    co_return 7;
}

int main() {
    ruvia::MoveOnlyFunction<int(int)> add([offset = std::make_unique<int>(4)](int value) { return value + *offset; });
    auto moved = std::move(add);
    if (add || !moved || moved(3) != 7) {
        return 1;
    }

    using NullFunction = void (*)();
    ruvia::MoveOnlyFunction<void()> nullFunction(static_cast<NullFunction>(nullptr));
    if (nullFunction) {
        return 1;
    }

    struct LargeCallable final {
        std::array<std::uint64_t, 8> padding{};
        int* calls;

        void operator()() {
            ++*calls;
        }
    };
    int calls = 0;
    ruvia::MoveOnlyFunction<void()> large(LargeCallable{{}, &calls});
    auto movedLarge = std::move(large);
    movedLarge();
    if (large || calls != 1) {
        return 1;
    }

    auto* const defaultResource = std::pmr::get_default_resource();
    ruvia::WorkerMemory worker({.requestInitialBufferBytes = 1024});
    ruvia::WorkerMemory independent({.requestInitialBufferBytes = 8192});
    std::pmr::memory_resource* resource = worker.resource();
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    return resource == nullptr || !loops.loop(0).valid() || worker.requestInitialBufferBytes() != 1024 || independent.requestInitialBufferBytes() != 8192 || std::pmr::get_default_resource() != defaultResource ? 1 : 0;
}
