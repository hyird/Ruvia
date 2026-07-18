#include "ruvia/core/Task.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/version.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <concepts>
#include <memory_resource>
#include <string_view>
#include <utility>

template <typename T>
concept ExposesRvalueRequestMemoryBorrow =
    requires(T&& memory) { std::move(memory).resource(); } ||
    requires(T&& memory) { std::move(memory).template allocator<>(); };

static_assert(!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>);

static_assert(RUVIA_VERSION_MAJOR == RUVIA_EXPECTED_VERSION_MAJOR);
static_assert(RUVIA_VERSION_MINOR == RUVIA_EXPECTED_VERSION_MINOR);
static_assert(RUVIA_VERSION_PATCH == RUVIA_EXPECTED_VERSION_PATCH);
static_assert(
    std::string_view(RUVIA_VERSION_STRING) ==
    std::string_view(RUVIA_EXPECTED_VERSION_STRING));

ruvia::Task<int> smokeTask() {
    co_return 7;
}

int main() {
    ruvia::WorkerMemory worker;
    std::pmr::memory_resource* resource = worker.resource();
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    return resource == nullptr || !loops.loop(0).valid() ? 1 : 0;
}
