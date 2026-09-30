#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>

#include "ruvia/core/memory/MemoryPool.h"

#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t attempts{};
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t live{};
    std::size_t liveBytes{};
    std::optional<std::size_t> failAt{};
    bool matchedReturns{true};

private:
    struct Allocation final {
        std::size_t bytes;
        std::size_t alignment;
    };

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++attempts;
        if (failAt == attempts) {
            throw std::bad_alloc();
        }
        auto* block = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        try {
            blocks_.emplace(block, Allocation{bytes, alignment});
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(block, bytes, alignment);
            throw;
        }
        ++allocations;
        ++live;
        liveBytes += bytes;
        return block;
    }
    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override {
        const auto entry = blocks_.find(block);
        if (entry == blocks_.end()) {
            std::terminate();
        }
        const auto actual = entry->second;
        matchedReturns = matchedReturns && bytes == actual.bytes && alignment == actual.alignment;
        blocks_.erase(entry);
        ++returns;
        --live;
        liveBytes -= actual.bytes;
        std::pmr::new_delete_resource()->deallocate(block, actual.bytes, actual.alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::pmr::unordered_map<void*, Allocation> blocks_{std::pmr::new_delete_resource()};
};

class DefaultResourceScope final {
public:
    explicit DefaultResourceScope(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}
    ~DefaultResourceScope() {
        std::pmr::set_default_resource(previous_);
    }
    DefaultResourceScope(const DefaultResourceScope&) = delete;
    DefaultResourceScope& operator=(const DefaultResourceScope&) = delete;

private:
    std::pmr::memory_resource* previous_;
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
        RUVIA_CHECK(upstream.allocations > 0);
        RUVIA_CHECK(std::pmr::get_default_resource() == previousDefault);
    }
    RUVIA_CHECK_EQ(upstream.live, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
    RUVIA_CHECK_EQ(upstream.liveBytes, std::size_t{0});
    RUVIA_CHECK(upstream.matchedReturns);
    RUVIA_CHECK(std::pmr::get_default_resource() == previousDefault);
}

RUVIA_TEST(worker_memory_resource_identity_preserves_live_arena_and_pool_storage) {
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream, {.requestInitialBufferBytes = 128});
        const auto& borrowedWorker = worker;
        auto allocator = worker.allocator<std::byte>();
        RUVIA_CHECK(allocator.resource() == worker.resource());
        RUVIA_CHECK(borrowedWorker.resource() == worker.resource());
        RUVIA_CHECK(worker.resource() != &upstream);
        const auto allocationsBeforeRequest = upstream.allocations;
        std::array<std::byte, 256> initial;
        ruvia::RequestMemory request(worker, initial);
        auto* handshake = static_cast<std::byte*>(request.resource()->allocate(128));
        std::fill_n(handshake, 128, std::byte{0x5a});
        RUVIA_CHECK_EQ(upstream.allocations, allocationsBeforeRequest);
        std::pmr::string retained(4096, 'r', worker.resource());
        const auto* retainedAddress = retained.data();
        std::optional<std::size_t> warmedBytes;
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            auto* scratch = allocator.allocate(1024);
            std::fill_n(scratch, 1024, std::byte{0xaa});
            allocator.deallocate(scratch, 1024);
            RUVIA_CHECK(handshake[0] == std::byte{0x5a} && handshake[127] == std::byte{0x5a});
            RUVIA_CHECK(retained.data() == retainedAddress && retained.size() == 4096);
            RUVIA_CHECK(retained.front() == 'r' && retained.back() == 'r');
            if (warmedBytes) {
                RUVIA_CHECK_EQ(upstream.liveBytes, *warmedBytes);
            } else {
                warmedBytes = upstream.liveBytes;
            }
        }
    }
    RUVIA_CHECK_EQ(upstream.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
    RUVIA_CHECK(upstream.matchedReturns);
}

RUVIA_TEST(const_request_memory_resource_allocates_with_initial_buffer_and_returns_overflow) {
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        std::array<std::byte, 256> initial{};
        const ruvia::RequestMemory request(worker, initial);
        auto* const resource = request.resource();
        RUVIA_CHECK(resource == request.resource());
        RUVIA_CHECK(request.upstreamResource() == worker.resource());

        const auto allocationsBeforeInitial = upstream.allocations;
        auto* initialBytes = static_cast<std::byte*>(resource->allocate(128));
        std::fill_n(initialBytes, 128, std::byte{0x5a});
        RUVIA_CHECK(initialBytes[127] == std::byte{0x5a});
        resource->deallocate(initialBytes, 128);
        RUVIA_CHECK_EQ(upstream.allocations, allocationsBeforeInitial);

        auto* overflowBytes = static_cast<std::byte*>(resource->allocate(4096));
        std::fill_n(overflowBytes, 4096, std::byte{0xa5});
        RUVIA_CHECK(overflowBytes[4095] == std::byte{0xa5});
        resource->deallocate(overflowBytes, 4096);
        RUVIA_CHECK(upstream.allocations > allocationsBeforeInitial);
    }
    RUVIA_CHECK_EQ(upstream.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
    RUVIA_CHECK(upstream.matchedReturns);
}

RUVIA_TEST(worker_memory_default_owner_ignores_global_default_resource) {
    CountingResource globalDefault;
    globalDefault.failAt = 1;
    auto* previous = std::pmr::get_default_resource();
    {
        DefaultResourceScope temporaryDefault(globalDefault);
        const ruvia::WorkerMemory worker;
        auto* bytes = static_cast<std::byte*>(worker.resource()->allocate(65536));
        std::fill_n(bytes, 65536, std::byte{0x42});
        RUVIA_CHECK(bytes[65535] == std::byte{0x42});
        worker.resource()->deallocate(bytes, 65536);
        RUVIA_CHECK_EQ(globalDefault.attempts, std::size_t{0});
        RUVIA_CHECK(std::pmr::get_default_resource() == &globalDefault);
    }
    RUVIA_CHECK(std::pmr::get_default_resource() == previous);
}

RUVIA_TEST(worker_memory_construction_and_pool_growth_failures_return_bound_storage) {
    for (const std::size_t failure : std::array{std::size_t{1}, std::size_t{2}}) {
        CountingResource upstream;
        upstream.failAt = failure;
        bool failed = false;
        try {
            ruvia::WorkerMemory worker(upstream);
            auto* bytes = worker.resource()->allocate(65536);
            worker.resource()->deallocate(bytes, 65536);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(upstream.liveBytes, std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
        RUVIA_CHECK(upstream.matchedReturns);
    }
}

RUVIA_TEST(request_memory_fork_survives_parent_and_keeps_its_own_allocations) {
    CountingResource upstream;
    {
        ruvia::WorkerMemory worker(upstream);
        ruvia::RequestMemory child = [&] {
            std::array<std::byte, 256> initial;
            ruvia::RequestMemory parent(worker, initial);
            return parent.fork();
        }();
        RUVIA_CHECK(child.upstreamResource() == worker.resource());
        auto* retained = static_cast<std::byte*>(child.resource()->allocate(8192));
        std::fill_n(retained, 8192, std::byte{0x3c});
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            ruvia::RequestMemory independent(worker);
            auto* scratch = static_cast<std::byte*>(independent.resource()->allocate(8192));
            std::fill_n(scratch, 8192, std::byte{0xaa});
            RUVIA_CHECK(retained[0] == std::byte{0x3c} && retained[8191] == std::byte{0x3c});
        }
    }
    RUVIA_CHECK_EQ(upstream.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations, upstream.returns);
    RUVIA_CHECK(upstream.matchedReturns);
}
