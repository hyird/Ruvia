#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/MemoryPool.h"

#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t live{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* block = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        ++live;
        return block;
    }
    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override {
        ++returns;
        --live;
        std::pmr::new_delete_resource()->deallocate(block, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(worker_memory_uses_bound_upstream_and_reclaims_request_arenas) {
    CountingResource upstream;
    auto* const previousDefault = std::pmr::get_default_resource();
    {
        ruvia::WorkerMemory worker(upstream, {.requestInitialBufferBytes = 128});
        RUVIA_CHECK_EQ(worker.requestInitialBufferBytes(), std::size_t{128});
        std::optional<std::size_t> cached;
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            {
                ruvia::RequestMemory request(worker);
                auto* bytes = static_cast<std::byte*>(request.resource()->allocate(65536));
                std::fill_n(bytes, 65536, std::byte{0x5a});
                RUVIA_CHECK(bytes[65535] == std::byte{0x5a});
            }
            // Live upstream blocks can include pool caches, but finished
            // request arenas must not accumulate new live blocks each cycle.
            if (cached) {
                RUVIA_CHECK_EQ(upstream.live, *cached);
            } else {
                cached = upstream.live;
            }
        }
        RUVIA_CHECK(upstream.allocations > 0 && upstream.returns > 0);
        RUVIA_CHECK(std::pmr::get_default_resource() == previousDefault);
    }
    RUVIA_CHECK_EQ(upstream.live, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
    RUVIA_CHECK(std::pmr::get_default_resource() == previousDefault);
}
