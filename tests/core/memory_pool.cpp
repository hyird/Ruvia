#include "ruvia/core/memory/memory_pool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_runtime.h"

namespace {
class default_resource_scope final {
public:
    explicit default_resource_scope(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}
    ~default_resource_scope() {
        std::pmr::set_default_resource(previous_);
    }
    default_resource_scope(const default_resource_scope&) = delete;
    default_resource_scope& operator=(const default_resource_scope&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

struct alignas(4096) retained_process_value final {
    std::array<std::byte, 4096> bytes_;
};
}  // namespace

TEST(Ruvia, worker_memory_trim_preserves_resource_identity_and_live_storage) {
    ruvia::worker_memory memory({.request_initial_buffer_bytes_ = 128});
    ruvia::worker_memory independent;
    const auto& borrowed_memory = memory;
    auto allocator = borrowed_memory.allocator<std::byte>();
    auto* const resource = memory.resource();
    EXPECT_TRUE(allocator.resource() == resource);
    EXPECT_TRUE(borrowed_memory.resource() == resource);
    EXPECT_TRUE(resource->is_equal(*resource));
    EXPECT_FALSE(resource->is_equal(*independent.resource()));
    EXPECT_FALSE(independent.resource()->is_equal(*resource));
    EXPECT_FALSE(resource->is_equal(*ruvia::process_resource()));
    EXPECT_FALSE(ruvia::process_resource()->is_equal(*resource));

    std::array<std::byte, 256> initial{};
    ruvia::request_memory request(memory, initial);
    auto* const handshake = static_cast<std::byte*>(request.resource()->allocate(128));
    auto* const overflow = static_cast<std::byte*>(request.resource()->allocate(8192));
    std::fill_n(handshake, 128, std::byte{0x5a});
    std::fill_n(overflow, 8192, std::byte{0xa5});
    std::pmr::string retained(30000, 'r', resource);
    const auto* const retained_address = retained.data();
    auto* const small = allocator.allocate(17);
    std::fill_n(small, 17, std::byte{0x33});

    ruvia::worker_runtime runtime;
    const auto owner = runtime.handle();
    bool trimmed = false;
    EXPECT_TRUE(owner.post([&] {
                         // Construction and final destruction belong to the caller;
                         // allocations and collection also work on the serving worker.
                         auto* const scratch = allocator.allocate(1024);
                         std::fill_n(scratch, 1024, std::byte{0xaa});
                         allocator.deallocate(scratch, 1024);
                         memory.trim(owner);
                         memory.trim(owner);
                         trimmed = true;
                     })
            .accepted());
    runtime.join();
    EXPECT_TRUE(trimmed);
    EXPECT_TRUE(memory.resource() == resource);
    EXPECT_TRUE(borrowed_memory.resource() == resource);
    EXPECT_TRUE(request.upstream_resource() == resource);
    EXPECT_TRUE(retained.data() == retained_address);
    EXPECT_EQ(retained.size(), std::size_t{30000});
    EXPECT_EQ(retained.find_first_not_of('r'), std::pmr::string::npos);
    EXPECT_TRUE(std::all_of(handshake, handshake + 128, [](std::byte value) {
        return value == std::byte{0x5a};
    }));
    EXPECT_TRUE(std::all_of(overflow, overflow + 8192, [](std::byte value) {
        return value == std::byte{0xa5};
    }));
    EXPECT_TRUE(std::all_of(small, small + 17, [](std::byte value) {
        return value == std::byte{0x33};
    }));
    allocator.deallocate(small, 17);
}

TEST(Ruvia, worker_memory_trim_rejects_missing_owner_and_non_owner_thread) {
    ruvia::worker_memory memory;
    ruvia::worker_runtime runtime;
    const auto owner = runtime.handle();
    for (const auto& candidate : {ruvia::worker_handle{}, owner}) {
        bool rejected = false;
        try {
            memory.trim(candidate);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    bool trimmed = false;
    EXPECT_TRUE(owner.post([&] {
                         memory.trim(owner);
                         trimmed = true;
                     })
            .accepted());
    runtime.join();
    EXPECT_TRUE(trimmed);
}

TEST(Ruvia, worker_memory_trim_rejects_a_worker_other_than_its_bound_owner) {
    ruvia::worker_memory memory;
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    const auto first_owner = first.handle();
    const auto second_owner = second.handle();
    bool bound = false;
    bool rejected = false;
    bool rebound = false;
    EXPECT_TRUE(first_owner.post([&] {
                               memory.trim(first_owner);
                               bound = true;
                           })
            .accepted());
    first.join();
    EXPECT_TRUE(bound);
    EXPECT_TRUE(second_owner
            .post([&] {
                // Running on a worker is not enough: it must be the bound one.
                try {
                    memory.trim(second_owner);
                } catch (const std::logic_error&) {
                    rejected = true;
                }
                try {
                    memory.trim(first_owner);
                } catch (const std::logic_error&) {
                    rebound = true;
                }
            })
            .accepted());
    second.join();
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(rebound);
}

// Non-multiple sizes must still honor every requested alignment.
TEST(Ruvia, worker_memory_and_process_resource_align_small_blocks) {
    ruvia::worker_memory worker;
    for (auto* const resource : {worker.resource(), ruvia::process_resource()}) {
        std::vector<std::pair<void*, std::pair<std::size_t, std::size_t>>> live;
        for (const std::size_t alignment : {std::size_t{8}, std::size_t{16}, std::size_t{32}, std::size_t{64}}) {
            for (std::size_t bytes_value = 1; bytes_value <= 300; ++bytes_value) {
                for (unsigned repeat = 0; repeat < 3; ++repeat) {
                    auto* const block = resource->allocate(bytes_value, alignment);
                    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block) % alignment, std::uintptr_t{0});
                    live.emplace_back(block, std::pair{bytes_value, alignment});
                }
            }
        }
        for (const auto& [block, shape] : live) {
            resource->deallocate(block, shape.first, shape.second);
        }
    }
}

TEST(Ruvia, worker_memory_and_process_resource_support_over_aligned_storage) {
    ruvia::worker_memory worker;
    for (auto* const resource : {worker.resource(), ruvia::process_resource()}) {
        for (const std::size_t alignment : {std::size_t{128}, std::size_t{4096}, std::size_t{65536}}) {
            for (const std::size_t bytes : {std::size_t{17}, std::size_t{30000}, std::size_t{2} * 1024 * 1024 + 1}) {
                auto* const block = static_cast<std::byte*>(resource->allocate(bytes, alignment));
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block) % alignment, std::uintptr_t{0});
                std::fill_n(block, bytes, std::byte{0x5a});
                EXPECT_TRUE(std::all_of(block, block + bytes, [](std::byte value) {
                    return value == std::byte{0x5a};
                }));
                resource->deallocate(block, bytes, alignment);
            }
        }
    }
}

TEST(Ruvia, process_resource_storage_survives_its_allocating_thread) {
    constexpr std::size_t text_bytes = 2 * 1024 * 1024;
    std::optional<std::pmr::string> text;
    std::optional<std::pmr::vector<retained_process_value>> values;
    std::thread([&] {
        text.emplace(text_bytes, 's', ruvia::process_resource());
        values.emplace(3, ruvia::process_resource());
        for (std::size_t index = 0; index < values->size(); ++index) {
            (*values)[index].bytes_.fill(static_cast<std::byte>(index + 1));
        }
    }).join();

    for (std::size_t index = 0; index < values->size(); ++index) {
        const auto& value = (*values)[index];
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&value) % alignof(retained_process_value), std::uintptr_t{0});
        EXPECT_TRUE(std::ranges::all_of(value.bytes_, [index](std::byte byte) {
            return byte == static_cast<std::byte>(index + 1);
        }));
    }
    EXPECT_EQ(text->size(), text_bytes);
    EXPECT_EQ(text->find_first_not_of('s'), std::pmr::string::npos);

    bool preserved_after_growth = false;
    std::thread([&] {
        // Grow and retire both consumers after their allocating thread exited.
        values->reserve(values->capacity() + 1);
        text->append(text->capacity() + 1, 't');
        preserved_after_growth = text->find_first_not_of('s') == text_bytes &&
                                 text->find_first_not_of('t', text_bytes) == std::pmr::string::npos;
        for (std::size_t index = 0; index < values->size(); ++index) {
            preserved_after_growth = preserved_after_growth &&
                                     std::ranges::all_of((*values)[index].bytes_, [index](std::byte byte) {
                                         return byte == static_cast<std::byte>(index + 1);
                                     });
        }
        text.reset();
        values.reset();
    }).join();
    EXPECT_TRUE(preserved_after_growth);
}

TEST(Ruvia, worker_memory_rejects_zero_request_initial_buffer) {
    const ruvia::memory_pool_config config{.request_initial_buffer_bytes_ = 0};
    for (const bool construct : {false, true}) {
        bool rejected = false;
        try {
            if (construct) {
                ruvia::worker_memory worker(config);
            } else {
                ruvia::validate_memory_pool_config(config);
            }
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    ruvia::validate_memory_pool_config({.request_initial_buffer_bytes_ = 1});
    ruvia::worker_memory smallest({.request_initial_buffer_bytes_ = 1});
    EXPECT_EQ(smallest.request_initial_buffer_bytes(), std::size_t{1});
    ruvia::request_memory request(smallest);
    auto* const block = static_cast<std::byte*>(request.resource()->allocate(128));
    std::fill_n(block, 128, std::byte{0x5a});
    EXPECT_TRUE(block[127] == std::byte{0x5a});
}

TEST(Ruvia, const_request_memory_uses_initial_buffer_and_preserves_it_after_overflow) {
    ruvia::worker_memory worker;
    std::array<std::byte, 256> initial{};
    const ruvia::request_memory request(worker, initial);
    auto* const resource = request.resource();
    EXPECT_TRUE(resource == request.resource());
    EXPECT_TRUE(request.allocator<std::byte>().resource() == resource);
    EXPECT_TRUE(request.upstream_resource() == worker.resource());
    EXPECT_FALSE(resource->is_equal(*worker.resource()));

    auto* const initial_bytes = static_cast<std::byte*>(resource->allocate(128));
    const auto initial_address = reinterpret_cast<std::uintptr_t>(initial.data());
    const auto allocated_address = reinterpret_cast<std::uintptr_t>(initial_bytes);
    EXPECT_GE(allocated_address, initial_address);
    EXPECT_LE(allocated_address - initial_address, initial.size() - 128);
    std::fill_n(initial_bytes, 128, std::byte{0x5a});
    resource->deallocate(initial_bytes, 128);

    auto* const overflow_bytes = static_cast<std::byte*>(resource->allocate(4096));
    std::fill_n(overflow_bytes, 4096, std::byte{0xa5});
    EXPECT_TRUE(std::all_of(initial_bytes, initial_bytes + 128, [](std::byte value) {
        return value == std::byte{0x5a};
    }));
    EXPECT_TRUE(overflow_bytes[4095] == std::byte{0xa5});
    resource->deallocate(overflow_bytes, 4096);
}

TEST(Ruvia, memory_resources_ignore_global_default_resource) {
    auto* const previous = std::pmr::get_default_resource();
    {
        default_resource_scope temporary_default(*std::pmr::null_memory_resource());
        ruvia::worker_memory worker;
        ruvia::request_memory request(worker);
        for (auto* const resource : {worker.resource(), request.resource(), ruvia::process_resource()}) {
            auto* const bytes = static_cast<std::byte*>(resource->allocate(65536));
            std::fill_n(bytes, 65536, std::byte{0x42});
            EXPECT_TRUE(bytes[65535] == std::byte{0x42});
            resource->deallocate(bytes, 65536);
        }
        EXPECT_TRUE(std::pmr::get_default_resource() == std::pmr::null_memory_resource());
    }
    EXPECT_TRUE(std::pmr::get_default_resource() == previous);
}

TEST(Ruvia, request_memory_fork_survives_parent_and_keeps_its_own_allocations) {
    constexpr std::size_t initial_bytes = 64 * 1024;
    ruvia::worker_memory worker({.request_initial_buffer_bytes_ = initial_bytes});
    EXPECT_EQ(worker.request_initial_buffer_bytes(), initial_bytes);
    std::array<std::byte, 256> initial{};
    std::optional<ruvia::request_memory> parent;
    parent.emplace(worker, initial);
    const ruvia::request_memory child = parent->fork();
    EXPECT_TRUE(child.upstream_resource() == worker.resource());
    EXPECT_FALSE(child.resource()->is_equal(*parent->resource()));
    auto allocator = child.allocator<std::byte>();
    auto* const small = allocator.allocate(64);
    auto* const first = allocator.allocate(20000);
    auto* const second = allocator.allocate(20000);
    std::fill_n(small, 64, std::byte{0x3c});
    std::fill_n(first, 20000, std::byte{0x1a});
    std::fill_n(second, 20000, std::byte{0x2b});
    parent.reset();
    initial.fill(std::byte{0xff});
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        ruvia::request_memory independent(worker);
        auto* const scratch = static_cast<std::byte*>(independent.resource()->allocate(8192));
        std::fill_n(scratch, 8192, std::byte{0xaa});
    }
    EXPECT_TRUE(std::all_of(small, small + 64, [](std::byte value) {
        return value == std::byte{0x3c};
    }));
    EXPECT_TRUE(std::all_of(first, first + 20000, [](std::byte value) {
        return value == std::byte{0x1a};
    }));
    EXPECT_TRUE(std::all_of(second, second + 20000, [](std::byte value) {
        return value == std::byte{0x2b};
    }));
    auto* const after_parent = allocator.allocate(8192);
    std::fill_n(after_parent, 8192, std::byte{0x4d});
    EXPECT_TRUE(after_parent[8191] == std::byte{0x4d});
}

TEST(Ruvia, memory_resources_honor_alignment_for_zero_byte_allocations) {
    ruvia::worker_memory worker;
    for (auto* const resource : {worker.resource(), ruvia::process_resource()}) {
        for (const std::size_t alignment : {alignof(std::max_align_t), std::size_t{128}, std::size_t{4096}, std::size_t{65536}}) {
            std::array<void*, 8> blocks{};
            for (auto& block : blocks) {
                block = resource->allocate(0, alignment);
                EXPECT_TRUE(block != nullptr);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block) % alignment, std::uintptr_t{0});
            }
            for (auto* block : blocks) {
                resource->deallocate(block, 0, alignment);
            }
        }
    }
}
