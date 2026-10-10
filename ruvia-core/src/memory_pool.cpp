#include "ruvia/core/memory/memory_pool.h"

#include <array>
#include <stdexcept>

#include "ruvia/core/detail/task/task_promise.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"

namespace ruvia {

namespace detail {

std::pmr::memory_resource* process_resource() noexcept {
    // Explicit process-lifetime storage for startup metadata and shutdown
    // cross-thread completions. It is never installed as std::pmr's default
    // resource: embedding Ruvia must not alter unrelated PMR allocations in the
    // host process.
    //
    // This resource intentionally has no C++ static destructor. blocking_pool
    // destructors may detach already-running callables, and those callables can
    // still destroy PMR-backed completion state while the process is leaving
    // main(). A function-local static resource would be released during static
    // teardown before those detached threads necessarily finish.
    // Bind the upstream explicitly: first use may occur while an embedding
    // application has temporarily replaced the global PMR default resource.
    // Pool small reusable objects, but return bulk arrays directly upstream.
    // An implementation's default can pool even multi-megabyte arrays, reserving
    // several equally large spare blocks and retaining them for process life.
    static auto* const resource = new std::pmr::synchronized_pool_resource(
        std::pmr::pool_options{.largest_required_pool_block = 64 * 1024},
        std::pmr::new_delete_resource());
    return resource;
}

namespace {

// Coroutine frames come and go several times per request, always in a handful
// of recurring sizes. A thread-local LIFO cache keyed by 128-byte size class
// hands a just-freed frame straight to the next allocation of the same class,
// so the request hot path skips the general allocator and reuses cache-warm
// memory. Blocks are allocated at the class ceiling, which keeps every cached
// block large enough for any request that maps to its bin. Only the sized
// deallocation path may cache: the unsized fallback cannot know the class.
constexpr std::size_t task_frame_cache_granularity = 128;
constexpr std::size_t task_frame_cache_max_block_bytes = 8 * 1024;
constexpr std::size_t task_frame_cache_budget_bytes = 128 * 1024;
constexpr std::size_t task_frame_cache_bin_count =
    task_frame_cache_max_block_bytes / task_frame_cache_granularity;

[[nodiscard]] constexpr std::size_t task_frame_class_bytes(std::size_t bytes_value) noexcept {
    // Large frames bypass the cache; rounding them could overflow.
    if (bytes_value > task_frame_cache_max_block_bytes) {
        return bytes_value;
    }
    return (bytes_value + task_frame_cache_granularity - 1) & ~(task_frame_cache_granularity - 1);
}

// True once this thread's cache has been destroyed. Coroutine frames can be
// freed during thread or static teardown after ~task_frame_cache has run; touching
// the cache then would use an object whose lifetime has ended. This flag is
// trivially destructible, so its storage outlives the cache's destructor and
// stays readable through teardown. The allocate/free paths fall back to the raw
// allocator once it is set.
thread_local bool task_frame_cache_destroyed = false;

class task_frame_cache final {
public:
    task_frame_cache() noexcept = default;
    task_frame_cache(const task_frame_cache&) = delete;
    task_frame_cache& operator=(const task_frame_cache&) = delete;

    ~task_frame_cache() {
        task_frame_cache_destroyed = true;
        for (void*& head : bins_) {
            while (head != nullptr) {
                void* next_value = *static_cast<void**>(head);
                ::operator delete(head);
                head = next_value;
            }
        }
    }

    [[nodiscard]] void* take_block(std::size_t class_bytes) noexcept {
        void*& head = bins_[bin_index(class_bytes)];
        void* block = head;
        if (block != nullptr) {
            head = *static_cast<void**>(block);
            cached_bytes_ -= class_bytes;
        }
        return block;
    }

    [[nodiscard]] bool store_block(void* block, std::size_t class_bytes) noexcept {
        if (cached_bytes_ + class_bytes > task_frame_cache_budget_bytes) {
            return false;
        }
        void*& head = bins_[bin_index(class_bytes)];
        *static_cast<void**>(block) = head;
        head = block;
        cached_bytes_ += class_bytes;
        return true;
    }

private:
    [[nodiscard]] static constexpr std::size_t bin_index(std::size_t class_bytes) noexcept {
        return class_bytes / task_frame_cache_granularity - 1;
    }

    std::array<void*, task_frame_cache_bin_count> bins_{};
    std::size_t cached_bytes_{0};
};

thread_local task_frame_cache task_frame_cache;

}  // namespace

void* task_frame_allocate(std::size_t bytes_value) {
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value == 0 ? 1 : bytes_value);
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_cache_max_block_bytes) {
        if (void* cached = task_frame_cache.take_block(class_bytes)) {
            return cached;
        }
    }
    return ::operator new(class_bytes);
}

void task_frame_deallocate(void* pointer) noexcept {
    ::operator delete(pointer);
}

void task_frame_deallocate_sized(void* pointer, std::size_t bytes_value) noexcept {
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value == 0 ? 1 : bytes_value);
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_cache_max_block_bytes &&
        task_frame_cache.store_block(pointer, class_bytes)) {
        return;
    }
    ::operator delete(pointer);
}

}  // namespace detail

class worker_memory::impl_type final {
public:
    explicit impl_type(std::pmr::memory_resource& upstream)
        : pool_(&upstream) {}

    std::pmr::unsynchronized_pool_resource pool_;
};

void worker_memory::impl_deleter_type::operator()(impl_type* impl) const noexcept {
    if (impl != nullptr) {
        auto* upstream = impl->pool_.upstream_resource();
        detail::destroy_pmr_object(impl, upstream);
    }
}

void validate_memory_pool_config(const memory_pool_config& config) {
    if (config.request_initial_buffer_bytes_ == 0) {
        throw std::invalid_argument("memory pool request initial buffer size must be greater than zero");
    }
}

worker_memory::worker_memory(const memory_pool_config& config)
    : worker_memory(*detail::process_resource(), config) {}

worker_memory::worker_memory(std::pmr::memory_resource& upstream, const memory_pool_config& config)
    : impl_((validate_memory_pool_config(config), detail::construct_pmr_object<impl_type>(&upstream, upstream))),
      resource_(&impl_->pool_),
      request_initial_buffer_bytes_(config.request_initial_buffer_bytes_) {}

worker_memory::~worker_memory() = default;

request_memory::request_memory(worker_memory& worker_value)
    : arena_(worker_value.request_initial_buffer_bytes(), worker_value.resource()) {}

request_memory::request_memory(worker_memory& worker_value, std::span<std::byte> initial_buffer)
    : arena_(initial_buffer.data(), initial_buffer.size(), worker_value.resource()) {}

request_memory::request_memory(child_arena_type, std::pmr::memory_resource* upstream)
    : arena_(request_arena_initial_bytes, upstream) {}

request_memory request_memory::fork() const& {
    return request_memory(child_arena_type{}, arena_.upstream_resource());
}

std::pmr::memory_resource* request_memory::resource() & noexcept {
    return &arena_;
}

std::pmr::memory_resource* request_memory::resource() const& noexcept {
    return &arena_;
}

std::pmr::memory_resource* request_memory::upstream_resource() & noexcept {
    return arena_.upstream_resource();
}

std::pmr::memory_resource* request_memory::upstream_resource() const& noexcept {
    return arena_.upstream_resource();
}

}  // namespace ruvia
