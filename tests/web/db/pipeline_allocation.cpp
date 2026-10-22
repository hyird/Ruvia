#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// A database handle keeps results in its client's own memory, which no caller
// can substitute, so this executable replaces the global operator new to fail
// one large allocation on the client's worker.

namespace {

// Armed on one thread: its next allocation of at least this many bytes fails.
thread_local std::size_t failing_allocation_bytes = 0;

void* allocate(std::size_t bytes, std::size_t alignment) {
    if (failing_allocation_bytes != 0 && bytes >= failing_allocation_bytes) {
        failing_allocation_bytes = 0;
        throw std::bad_alloc();
    }
    const auto size = bytes == 0 ? alignment : (bytes + alignment - 1) / alignment * alignment;
#ifdef _WIN32
    void* const pointer = _aligned_malloc(size, alignment);
#else
    void* const pointer = std::aligned_alloc(alignment, size);
#endif
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void release(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* allocate_nothrow(std::size_t bytes, std::size_t alignment) noexcept {
    try {
        return allocate(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

void* operator new(std::size_t bytes) {
    return allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t bytes) {
    return allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate_nothrow(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate_nothrow(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocate_nothrow(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept {
    release(pointer);
}
void operator delete[](void* pointer) noexcept {
    release(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    release(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    release(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    release(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    release(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    release(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    release(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    release(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    release(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    release(pointer);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace pipeline_allocation_test {

struct outcome final {
    std::string value_;
    bool out_of_memory_{false};
    std::optional<ruvia::db_error::code_type> error_;
};

inline ruvia::task<void> query(ruvia::db_handle handle, std::string_view sql, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const std::bad_alloc&) {
        result.out_of_memory_ = true;
    } catch (const ruvia::db_error& error) {
        result.error_ = error.code();
    }
}

}  // namespace pipeline_allocation_test

// A statement whose rows cannot be kept fails alone: its result was complete,
// so the statements in flight around it get theirs and the connection stays.
RUVIA_TEST(db_postgresql_pipelined_statement_whose_rows_cannot_be_kept_fails_alone) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    // Large enough that keeping the rows takes allocations of over a megabyte.
    constexpr std::size_t large_rows = 100000;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .batch_ = 3}, {.rows_ = std::vector<std::string>(large_rows, "x")}, {.rows_ = {"c"}},
            {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_allocation_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_allocation_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_allocation_test::query(handle, "SELECT v FROM large", outcomes[1]));
        statements.spawn(pipeline_allocation_test::query(handle, "SELECT v FROM c", outcomes[2]));
        failing_allocation_bytes = std::size_t{1024} * 1024;
        co_await statements.join();
        failing_allocation_bytes = 0;
        co_await pipeline_allocation_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].out_of_memory_);
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}
#endif
