#include "ruvia/core/Task.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <memory_resource>

ruvia::Task<int> smokeTask() {
    co_return 7;
}

int main() {
    ruvia::WorkerMemory worker;
    std::pmr::memory_resource* resource = worker.resource();
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    return resource == nullptr || !loops.loop(0).valid() ? 1 : 0;
}
