#include "ruvia/core/memory/memory_pool.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RUVIA_SANITIZER_ALLOCATOR 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define RUVIA_SANITIZER_ALLOCATOR 1
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif
#else
#define RUVIA_SANITIZER_ALLOCATOR 0
#endif

#if RUVIA_SANITIZER_ALLOCATOR
#include <cstdlib>
#else
#include <mimalloc.h>
#endif

#include <algorithm>
#include <array>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "ruvia/core/detail/task/task_promise.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

namespace {

[[nodiscard]] void* allocate_mimalloc(std::size_t bytes_value, std::size_t alignment) {
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, (std::max)(alignment, alignof(void*)))) {
        return block;
    }
#else
    void* block = nullptr;
    if (::posix_memalign(&block, (std::max)(alignment, alignof(void*)), size) == 0 && block != nullptr) {
        return block;
    }
#endif
#else
    if (void* block = mi_malloc_aligned(size, alignment)) {
        return block;
    }
#endif
    throw std::bad_alloc();
}

void free_mimalloc(void* block) noexcept {
#if RUVIA_SANITIZER_ALLOCATOR
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
#else
    mi_free(block);
#endif
}

class mimalloc_memory_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        return allocate_mimalloc(bytes_value, alignment);
    }

    void do_deallocate(void* block, std::size_t, std::size_t) noexcept override {
        free_mimalloc(block);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

std::pmr::memory_resource* process_resource() noexcept {
    // The adapter has process lifetime without a C++ static destructor:
    // detached blocking work may release completion state during teardown.
    // mimalloc owns its thread heaps and supports freeing on another thread,
    // including after the allocating thread exits. No shared pool is layered
    // over it, and the default PMR resource object is not replaced.
    alignas(mimalloc_memory_resource) static std::byte storage[sizeof(mimalloc_memory_resource)];
    static auto* const resource = ::new (storage) mimalloc_memory_resource;
    return resource;
}

namespace detail {

namespace {

// LIFO free lists of cached blocks keyed by size class. Class N holds blocks of
// exactly (N + 1) * granularity bytes, so any block in a bin serves every request
// that rounds to it; each block's first word links it to the next. The owner
// decides what is cacheable, allocates misses at the class ceiling and frees
// what store() refuses or drain() hands back. The cached total never exceeds the
// budget passed to store(). Unsynchronized: each owner is thread- or worker-affine.
template <std::size_t t_granularity, std::size_t t_max_block_bytes>
class size_class_lifo_bins final {
public:
    static_assert(t_granularity >= sizeof(void*) && (t_granularity & (t_granularity - 1)) == 0,
        "size class granularity must be a power of two that holds the link word");
    static_assert(t_max_block_bytes % t_granularity == 0, "the largest class must be a whole class");

    static constexpr std::size_t granularity = t_granularity;
    static constexpr std::size_t max_block_bytes = t_max_block_bytes;

    // The class ceiling of 1..max_block_bytes bytes.
    [[nodiscard]] static constexpr std::size_t class_bytes(std::size_t bytes_value) noexcept {
        return (bytes_value + granularity - 1) & ~(granularity - 1);
    }

    [[nodiscard]] std::size_t cached_bytes() const noexcept {
        return cached_bytes_;
    }

    [[nodiscard]] void* take(std::size_t block_bytes) noexcept {
        void*& head = bins_[bin_index(block_bytes)];
        void* const block = head;
        if (block != nullptr) {
            head = *static_cast<void**>(block);
            cached_bytes_ -= block_bytes;
        }
        return block;
    }

    // Caches `block` of class `block_bytes` unless that would exceed
    // `budget_bytes`; false leaves the block with the caller.
    [[nodiscard]] bool store(void* block, std::size_t block_bytes, std::size_t budget_bytes) noexcept {
        // cached_bytes_ never exceeds the budget, so the subtraction cannot wrap.
        if (budget_bytes - cached_bytes_ < block_bytes) {
            return false;
        }
        void*& head = bins_[bin_index(block_bytes)];
        *static_cast<void**>(block) = head;
        head = block;
        cached_bytes_ += block_bytes;
        return true;
    }

    // Hands every cached block to release(block, block_bytes). Each block is
    // unlinked and accounted before the call, which may reenter these bins.
    template <typename release_type>
    void drain(release_type&& release) noexcept {
        for (std::size_t index = 0; index != bin_count; ++index) {
            const std::size_t block_bytes = (index + 1) * granularity;
            void*& head = bins_[index];
            while (head != nullptr) {
                void* const block = head;
                head = *static_cast<void**>(block);
                cached_bytes_ -= block_bytes;
                release(block, block_bytes);
            }
        }
    }

private:
    static constexpr std::size_t bin_count = max_block_bytes / granularity;

    [[nodiscard]] static constexpr std::size_t bin_index(std::size_t block_bytes) noexcept {
        return block_bytes / granularity - 1;
    }

    std::array<void*, bin_count> bins_{};
    std::size_t cached_bytes_{0};
};

// Coroutine frames come and go several times per request, always in a handful
// of recurring sizes. A thread-local LIFO cache keyed by 128-byte size class
// hands a just-freed frame straight to the next allocation of the same class,
// so the request hot path skips the general allocator and reuses cache-warm
// memory. Blocks are allocated at the class ceiling, which keeps every cached
// block large enough for any request that maps to its bin. Deallocation is
// always sized, so every freed frame maps back to its class.
using task_frame_bins = size_class_lifo_bins<128, 16 * 1024>;
constexpr std::size_t task_frame_cache_budget_bytes = 512 * 1024;

[[nodiscard]] constexpr std::size_t task_frame_class_bytes(std::size_t bytes_value) noexcept {
    const std::size_t size = bytes_value == 0 ? 1 : bytes_value;
    // Large frames bypass the cache; rounding them could overflow.
    if (size > task_frame_bins::max_block_bytes) {
        return size;
    }
    return task_frame_bins::class_bytes(size);
}

// True once this thread's cache has been destroyed. Coroutine frames can be
// freed during thread or static teardown after ~task_frame_cache has run; touching
// the cache then would use an object whose lifetime has ended. This flag is
// trivially destructible, so its storage outlives the cache's destructor and
// stays readable through teardown. The allocate/free paths fall back to the raw
// allocator once it is set.
thread_local bool task_frame_cache_destroyed = false;

// Frees this thread's cached frames at thread exit.
struct task_frame_cache final {
    ~task_frame_cache() {
        task_frame_cache_destroyed = true;
        release_all();
    }

    void release_all() noexcept {
        bins_.drain([](void* block, std::size_t) noexcept { free_mimalloc(block); });
    }

    task_frame_bins bins_;
};

thread_local task_frame_cache frame_cache;

}  // namespace

void* task_frame_allocate(std::size_t bytes_value) {
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value);
#if !RUVIA_SANITIZER_ALLOCATOR
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_bins::max_block_bytes) {
        if (void* cached = frame_cache.bins_.take(class_bytes)) {
            return cached;
        }
    }
#endif
    return allocate_mimalloc(class_bytes, alignof(std::max_align_t));
}

void task_frame_deallocate_sized(void* pointer, std::size_t bytes_value) noexcept {
#if !RUVIA_SANITIZER_ALLOCATOR
    const std::size_t class_bytes = task_frame_class_bytes(bytes_value);
    if (!task_frame_cache_destroyed && class_bytes <= task_frame_bins::max_block_bytes &&
        frame_cache.bins_.store(pointer, class_bytes, task_frame_cache_budget_bytes)) {
        return;
    }
#else
    (void)bytes_value;
#endif
    free_mimalloc(pointer);
}

void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment) {
    return allocate_mimalloc(bytes_value, alignment);
}

void task_frame_deallocate_aligned(void* pointer) noexcept {
    free_mimalloc(pointer);
}

void task_frame_cache_release() noexcept {
    if (!task_frame_cache_destroyed) {
        frame_cache.release_all();
    }
}

}  // namespace detail

// Worker-local exact-size cache. Small and large size classes share one
// retention budget; only freed blocks count against it. Overflow returns
// upstream immediately, and release_cached() never touches live allocations.
// Blocks above 1 MiB or with alignment above 64 pass straight through.
class worker_memory::impl_type final : public std::pmr::memory_resource {
public:
    static constexpr std::size_t small_block_bytes = std::size_t{16} * 1024;
    static constexpr std::size_t block_alignment = 64;

    impl_type(std::pmr::memory_resource& upstream, std::size_t retained_budget_bytes) noexcept
        : upstream_(&upstream),
          retained_budget_bytes_(retained_budget_bytes) {}

    ~impl_type() override {
        release_cached();
    }

    impl_type(const impl_type&) = delete;
    impl_type& operator=(const impl_type&) = delete;

    void release_cached() noexcept {
        // The upstream may reenter this cache while releasing a block; drain
        // publishes each removal before the call.
        const auto release = [this](void* block, std::size_t block_bytes) noexcept {
            upstream_->deallocate(block, block_bytes, block_alignment);
        };
        small_bins_.drain(release);
        bins_.drain(release);
    }

    [[nodiscard]] std::pmr::memory_resource* upstream_resource() const noexcept {
        return upstream_;
    }

private:
    using small_bins_type = detail::size_class_lifo_bins<alignof(std::max_align_t), small_block_bytes>;
    using bins_type = detail::size_class_lifo_bins<std::size_t{4} * 1024, std::size_t{1024} * 1024>;

    [[nodiscard]] static constexpr bool cacheable(std::size_t bytes_value, std::size_t alignment) noexcept {
        return bytes_value <= bins_type::max_block_bytes && alignment <= block_alignment;
    }

    template <typename bins_value_type>
    [[nodiscard]] void* allocate_from_bins(bins_value_type& bins, std::size_t bytes_value) {
        const std::size_t block_bytes = bins_value_type::class_bytes(bytes_value == 0 ? 1 : bytes_value);
        if (void* const block = bins.take(block_bytes)) {
            return block;
        }
        return upstream_->allocate(block_bytes, block_alignment);
    }

    template <typename bins_value_type>
    void deallocate_to_bins(
        bins_value_type& bins, void* block, std::size_t bytes_value, std::size_t other_cached_bytes) noexcept {
        const std::size_t block_bytes = bins_value_type::class_bytes(bytes_value == 0 ? 1 : bytes_value);
        if (!bins.store(block, block_bytes, retained_budget_bytes_ - other_cached_bytes)) {
            upstream_->deallocate(block, block_bytes, block_alignment);
        }
    }

    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (!cacheable(bytes_value, alignment)) {
            return upstream_->allocate(bytes_value, alignment);
        }
        return bytes_value <= small_block_bytes ? allocate_from_bins(small_bins_, bytes_value)
                                                : allocate_from_bins(bins_, bytes_value);
    }

    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override {
        if (!cacheable(bytes_value, alignment)) {
            upstream_->deallocate(block, bytes_value, alignment);
            return;
        }
        if (bytes_value <= small_block_bytes) {
            deallocate_to_bins(small_bins_, block, bytes_value, bins_.cached_bytes());
        } else {
            deallocate_to_bins(bins_, block, bytes_value, small_bins_.cached_bytes());
        }
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t retained_budget_bytes_;
    small_bins_type small_bins_;
    bins_type bins_;
};

void worker_memory::impl_deleter_type::operator()(impl_type* impl) const noexcept {
    auto* const upstream = impl->upstream_resource();
    destroy_pmr_object(impl, upstream);
}

void validate_memory_pool_config(const memory_pool_config& config) {
    if (config.request_initial_buffer_bytes_ == 0) {
        throw std::invalid_argument("memory pool request initial buffer size must be greater than zero");
    }
    if (config.retained_block_bytes_ > max_retained_block_bytes) {
        throw std::invalid_argument("memory pool retained block bytes exceed the addressable maximum");
    }
}

// The default upstream is the process-lifetime mimalloc adapter, not a shared
// pool. Each worker retains its own bounded caches and can also bind an
// explicit caller-owned upstream without changing its allocation contract.
worker_memory::worker_memory(const memory_pool_config& config)
    : worker_memory(*process_resource(), config) {}

worker_memory::worker_memory(std::pmr::memory_resource& upstream, const memory_pool_config& config)
    : impl_((validate_memory_pool_config(config),
          construct_pmr_object<impl_type>(&upstream, upstream, config.retained_block_bytes_))),
      resource_(impl_.get()),
      request_initial_buffer_bytes_(config.request_initial_buffer_bytes_) {}

worker_memory::~worker_memory() = default;

void worker_memory::trim(const worker_handle& owner) {
    const auto owner_id = owner.id();
    if (owner_id == 0 || !owner.is_current() || (owner_id_ != 0 && owner_id != owner_id_)) {
        throw std::logic_error("worker memory trim must run on its owner worker");
    }
    owner_id_ = owner_id;
    impl_->release_cached();
    detail::task_frame_cache_release();
}

request_memory::request_memory(worker_memory& worker_value)
    : request_memory(worker_value.request_initial_buffer_bytes(), worker_value.resource()) {}

request_memory::request_memory(worker_memory& worker_value, std::span<std::byte> initial_buffer)
    : arena_(initial_buffer.data(), initial_buffer.size(), worker_value.resource()),
      initial_bytes_(worker_value.request_initial_buffer_bytes()) {}

request_memory::request_memory(std::size_t initial_bytes, std::pmr::memory_resource* upstream)
    : arena_(initial_bytes, upstream),
      initial_bytes_(initial_bytes) {}

request_memory request_memory::fork() const& {
    return request_memory(initial_bytes_, arena_.upstream_resource());
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
