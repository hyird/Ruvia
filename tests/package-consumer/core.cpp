#include <ruvia/core/Task.h>
#include <ruvia/core/memory/MemoryPool.h>
#include <ruvia/core/memory/PmrObject.h>

int main() {
    ruvia::WorkerMemory worker;
    return worker.resource() == nullptr ? 1 : 0;
}
