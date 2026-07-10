#include <ruvia/app/Task.h>
#include <ruvia/memory/MemoryPool.h>
#include <ruvia/memory/PmrObject.h>

int main() {
    ruvia::WorkerMemory worker;
    return worker.resource() == nullptr ? 1 : 0;
}
