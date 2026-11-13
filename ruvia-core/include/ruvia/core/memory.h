#pragma once

namespace ruvia {

// Collects the calling thread's mimalloc heap and forces the shared arenas to
// purge. Live allocations are untouched. A completely idle thread never runs
// mimalloc's deferred collection by itself, so a worker that went quiet calls
// this once.
void collect_thread_heap() noexcept;

}  // namespace ruvia
