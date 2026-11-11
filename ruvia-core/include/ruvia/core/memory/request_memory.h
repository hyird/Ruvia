#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>

namespace ruvia {

// Default initial bump-block size for a request arena. A request whose
// allocations fit within it touches no further heap. Runtime integrations
// size reusable connection blocks from the same configured byte count.
inline constexpr std::size_t request_arena_initial_bytes = std::size_t{4} * 1024;

// Collects the calling thread's mimalloc heap and forces the shared arenas to
// purge. Live allocations are untouched. A completely idle thread never runs
// mimalloc's deferred collection by itself, so a worker that went quiet calls
// this once.
void collect_thread_heap() noexcept;

// Request/handshake lifetime arena over process_resource() and an optional
// borrowed initial buffer, which must outlive this object. Individual
// deallocation is a no-op; destruction returns overflow blocks to mimalloc,
// not necessarily the OS. Objects using the arena must be destroyed before the
// arena itself.
class request_memory final {
public:
    explicit request_memory(std::size_t initial_bytes = request_arena_initial_bytes);
    request_memory(std::size_t initial_bytes, std::span<std::byte> initial_buffer);
    ~request_memory() = default;

    request_memory(const request_memory&) = delete;
    request_memory& operator=(const request_memory&) = delete;

    // Independent arena with the same initial block size; it borrows nothing
    // from this arena and may outlive it.
    [[nodiscard]] request_memory fork() const&;
    request_memory fork() const&& = delete;

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const& noexcept {
        return std::pmr::polymorphic_allocator<t_type>(&arena_);
    }

    template <typename t_type = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<t_type> allocator() const&& = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() & noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const& noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() && = delete;
    [[nodiscard]] std::pmr::memory_resource* resource() const&& = delete;

private:
    mutable std::pmr::monotonic_buffer_resource arena_;
    std::size_t initial_bytes_;
};

}  // namespace ruvia
