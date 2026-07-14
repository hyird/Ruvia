#include "ruvia/core/Task.h"
#include "ruvia/core/Runtime.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <memory_resource>

ruvia::Task<int> smokeTask() {
    co_return 7;
}

int main() {
    ruvia::WorkerMemory worker;
    std::pmr::memory_resource* resource = worker.resource();
    ruvia::Runtime runtime({.workerCount = 1, .mailboxCapacity = 1});
    return resource == nullptr || !runtime.worker(0).valid() ? 1 : 0;
}
