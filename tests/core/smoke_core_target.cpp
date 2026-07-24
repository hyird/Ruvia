#include "ruvia/core/Task.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/version.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <array>
#include <concepts>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <utility>

template <typename T>
concept ExposesRvalueRequestMemoryBorrow = requires(T&& memory) { std::move(memory).resource(); } || requires(T&& memory) { std::move(memory).template allocator<>(); };

static_assert(!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>);

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
