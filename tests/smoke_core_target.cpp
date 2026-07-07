#include "ruvia/app/Task.h"
#include "ruvia/memory/MemoryPool.h"

#include <memory_resource>

ruvia::Task<int> smokeTask() {
    co_return 7;
}

int main() {
    ruvia::WorkerMemory worker;
    std::pmr::memory_resource* resource = worker.resource();
    return resource == nullptr ? 1 : 0;
}
