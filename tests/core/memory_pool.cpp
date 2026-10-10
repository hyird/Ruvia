#include "ruvia/core/memory/memory_pool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t attempts_{};
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t live_{};
    std::size_t live_bytes_{};
    std::optional<std::size_t> fail_at_{};
    bool matched_returns_{true};

private:
    struct allocation_type final {
        std::size_t bytes_;
        std::size_t alignment_;
    };

    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++attempts_;
        if (fail_at_ == attempts_) {
            throw std::bad_alloc();
        }
        auto* block = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        try {
            blocks_.emplace(block, allocation_type{bytes_value, alignment});
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(block, bytes_value, alignment);
            throw;
        }
        ++allocations_;
        ++live_;
        live_bytes_ += bytes_value;
        return block;
    }
    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override {
        const auto entry_value = blocks_.find(block);
        if (entry_value == blocks_.end()) {
            std::terminate();
        }
        const auto actual = entry_value->second;
        matched_returns_ = matched_returns_ && bytes_value == actual.bytes_ && alignment == actual.alignment_;
        blocks_.erase(entry_value);
        ++returns_;
        --live_;
        live_bytes_ -= actual.bytes_;
        std::pmr::new_delete_resource()->deallocate(block, actual.bytes_, actual.alignment_);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::pmr::unordered_map<void*, allocation_type> blocks_{std::pmr::new_delete_resource()};
};

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
}  // namespace

RUVIA_TEST(worker_memory_uses_bound_upstream_and_reclaims_request_arenas) {
    counting_resource upstream;
    auto* const previous_default = std::pmr::get_default_resource();
    {
        ruvia::worker_memory worker_value(upstream, {.request_initial_buffer_bytes_ = 128});
        RUVIA_CHECK_EQ(worker_value.request_initial_buffer_bytes(), std::size_t{128});
        std::optional<std::size_t> cached;
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            {
                ruvia::request_memory request(worker_value);
                auto* bytes_value = static_cast<std::byte*>(request.resource()->allocate(65536));
                std::fill_n(bytes_value, 65536, std::byte{0x5a});
                RUVIA_CHECK(bytes_value[65535] == std::byte{0x5a});
            }
            // Live upstream blocks can include pool caches, but finished
            // request arenas must not accumulate new live blocks each cycle.
            if (cached) {
                RUVIA_CHECK_EQ(upstream.live_, *cached);
            } else {
                cached = upstream.live_;
            }
        }
        RUVIA_CHECK(upstream.allocations_ > 0);
        RUVIA_CHECK(std::pmr::get_default_resource() == previous_default);
    }
    RUVIA_CHECK_EQ(upstream.live_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
    RUVIA_CHECK_EQ(upstream.live_bytes_, std::size_t{0});
    RUVIA_CHECK(upstream.matched_returns_);
    RUVIA_CHECK(std::pmr::get_default_resource() == previous_default);
}

RUVIA_TEST(worker_memory_rejects_zero_request_initial_buffer) {
    counting_resource upstream;
    const ruvia::memory_pool_config config{.request_initial_buffer_bytes_ = 0};
    for (const bool construct : {false, true}) {
        bool rejected = false;
        try {
            if (construct) {
                ruvia::worker_memory worker_value(upstream, config);
            } else {
                ruvia::validate_memory_pool_config(config);
            }
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    RUVIA_CHECK_EQ(upstream.live_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, std::size_t{0});
    ruvia::validate_memory_pool_config({.request_initial_buffer_bytes_ = 1});
}

RUVIA_TEST(worker_memory_resource_identity_preserves_live_arena_and_pool_storage) {
    counting_resource upstream;
    {
        ruvia::worker_memory worker_value(upstream, {.request_initial_buffer_bytes_ = 128});
        const auto& borrowed_worker = worker_value;
        auto allocator = worker_value.allocator<std::byte>();
        RUVIA_CHECK(allocator.resource() == worker_value.resource());
        RUVIA_CHECK(borrowed_worker.resource() == worker_value.resource());
        RUVIA_CHECK(worker_value.resource() != &upstream);
        const auto allocations_before_request = upstream.allocations_;
        std::array<std::byte, 256> initial;
        ruvia::request_memory request(worker_value, initial);
        auto* handshake = static_cast<std::byte*>(request.resource()->allocate(128));
        std::fill_n(handshake, 128, std::byte{0x5a});
        RUVIA_CHECK_EQ(upstream.allocations_, allocations_before_request);
        std::pmr::string retained(4096, 'r', worker_value.resource());
        const auto* retained_address = retained.data();
        std::optional<std::size_t> warmed_bytes;
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            auto* scratch = allocator.allocate(1024);
            std::fill_n(scratch, 1024, std::byte{0xaa});
            allocator.deallocate(scratch, 1024);
            RUVIA_CHECK(handshake[0] == std::byte{0x5a} && handshake[127] == std::byte{0x5a});
            RUVIA_CHECK(retained.data() == retained_address && retained.size() == 4096);
            RUVIA_CHECK(retained.front() == 'r' && retained.back() == 'r');
            if (warmed_bytes) {
                RUVIA_CHECK_EQ(upstream.live_bytes_, *warmed_bytes);
            } else {
                warmed_bytes = upstream.live_bytes_;
            }
        }
    }
    RUVIA_CHECK_EQ(upstream.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
    RUVIA_CHECK(upstream.matched_returns_);
}

RUVIA_TEST(const_request_memory_resource_allocates_with_initial_buffer_and_returns_overflow) {
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        std::array<std::byte, 256> initial_value{};
        const ruvia::request_memory request(worker, initial_value);
        auto* const resource = request.resource();
        RUVIA_CHECK(resource == request.resource());
        RUVIA_CHECK(request.upstream_resource() == worker.resource());

        const auto allocations_before_initial = upstream.allocations_;
        auto* initial_bytes = static_cast<std::byte*>(resource->allocate(128));
        std::fill_n(initial_bytes, 128, std::byte{0x5a});
        RUVIA_CHECK(initial_bytes[127] == std::byte{0x5a});
        resource->deallocate(initial_bytes, 128);
        RUVIA_CHECK_EQ(upstream.allocations_, allocations_before_initial);

        auto* overflow_bytes = static_cast<std::byte*>(resource->allocate(4096));
        std::fill_n(overflow_bytes, 4096, std::byte{0xa5});
        RUVIA_CHECK(overflow_bytes[4095] == std::byte{0xa5});
        resource->deallocate(overflow_bytes, 4096);
        RUVIA_CHECK(upstream.allocations_ > allocations_before_initial);
    }
    RUVIA_CHECK_EQ(upstream.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
    RUVIA_CHECK(upstream.matched_returns_);
}

RUVIA_TEST(worker_memory_default_owner_ignores_global_default_resource) {
    counting_resource global_default;
    global_default.fail_at_ = 1;
    auto* previous = std::pmr::get_default_resource();
    {
        default_resource_scope temporary_default(global_default);
        const ruvia::worker_memory worker;
        auto* bytes_value = static_cast<std::byte*>(worker.resource()->allocate(65536));
        std::fill_n(bytes_value, 65536, std::byte{0x42});
        RUVIA_CHECK(bytes_value[65535] == std::byte{0x42});
        worker.resource()->deallocate(bytes_value, 65536);
        RUVIA_CHECK_EQ(global_default.attempts_, std::size_t{0});
        RUVIA_CHECK(std::pmr::get_default_resource() == &global_default);
    }
    RUVIA_CHECK(std::pmr::get_default_resource() == previous);
}

RUVIA_TEST(worker_memory_pool_data_growth_failure_preserves_existing_storage) {
    constexpr std::size_t allocation_bytes = 1024;
    constexpr std::size_t max_growth_attempts = 4096;
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        auto* const resource = worker.resource();
        auto* const retained = static_cast<std::byte*>(resource->allocate(allocation_bytes));
        std::fill_n(retained, allocation_bytes, std::byte{0x5a});
        std::vector<std::byte*> transient_blocks;
        transient_blocks.reserve(max_growth_attempts);

        bool warmed_pool_growth = false;
        for (std::size_t attempt_value = 0; attempt_value < max_growth_attempts; ++attempt_value) {
            const auto upstream_attempts = upstream.attempts_;
            transient_blocks.push_back(
                static_cast<std::byte*>(resource->allocate(allocation_bytes)));
            if (upstream.attempts_ > upstream_attempts) {
                warmed_pool_growth = true;
                break;
            }
        }
        for (auto* block : transient_blocks) {
            resource->deallocate(block, allocation_bytes);
        }
        transient_blocks.clear();
        RUVIA_CHECK(warmed_pool_growth);

        const auto baseline_allocations = upstream.allocations_;
        const auto baseline_returns = upstream.returns_;
        const auto baseline_live = upstream.live_;
        const auto baseline_live_bytes = upstream.live_bytes_;
        upstream.fail_at_ = upstream.attempts_ + 1;
        bool growth_failed = false;
        for (std::size_t attempt_value = 0; attempt_value < max_growth_attempts; ++attempt_value) {
            try {
                transient_blocks.push_back(
                    static_cast<std::byte*>(resource->allocate(allocation_bytes)));
            } catch (const std::bad_alloc&) {
                growth_failed = true;
                break;
            }
        }
        upstream.fail_at_.reset();
        for (auto* block : transient_blocks) {
            resource->deallocate(block, allocation_bytes);
        }
        transient_blocks.clear();

        RUVIA_CHECK(growth_failed);
        RUVIA_CHECK(retained[0] == std::byte{0x5a} &&
                    retained[allocation_bytes - 1] == std::byte{0x5a});
        RUVIA_CHECK_EQ(upstream.allocations_, baseline_allocations);
        RUVIA_CHECK_EQ(upstream.returns_, baseline_returns);
        RUVIA_CHECK_EQ(upstream.live_, baseline_live);
        RUVIA_CHECK_EQ(upstream.live_bytes_, baseline_live_bytes);
        RUVIA_CHECK(upstream.matched_returns_);

        const auto attempts_before_retry = upstream.attempts_;
        std::byte* grown_block = nullptr;
        for (std::size_t attempt_value = 0; attempt_value < max_growth_attempts; ++attempt_value) {
            const auto upstream_attempts = upstream.attempts_;
            auto* const block = static_cast<std::byte*>(resource->allocate(allocation_bytes));
            transient_blocks.push_back(block);
            if (upstream.attempts_ > upstream_attempts) {
                grown_block = block;
                break;
            }
        }
        RUVIA_CHECK(grown_block != nullptr);
        RUVIA_CHECK(upstream.attempts_ > attempts_before_retry);
        RUVIA_CHECK(upstream.allocations_ > baseline_allocations);
        if (grown_block != nullptr) {
            std::fill_n(grown_block, allocation_bytes, std::byte{0xa5});
            RUVIA_CHECK(grown_block[0] == std::byte{0xa5} &&
                        grown_block[allocation_bytes - 1] == std::byte{0xa5});
        }
        RUVIA_CHECK(retained[0] == std::byte{0x5a} &&
                    retained[allocation_bytes - 1] == std::byte{0x5a});
        for (auto* block : transient_blocks) {
            resource->deallocate(block, allocation_bytes);
        }
        resource->deallocate(retained, allocation_bytes);
    }
    RUVIA_CHECK_EQ(upstream.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.live_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
    RUVIA_CHECK(upstream.matched_returns_);
}

RUVIA_TEST(request_memory_fork_survives_parent_and_keeps_its_own_allocations) {
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        ruvia::request_memory child_value = [&] {
            std::array<std::byte, 256> initial;
            ruvia::request_memory parent(worker, initial);
            return parent.fork();
        }();
        RUVIA_CHECK(child_value.upstream_resource() == worker.resource());
        auto* retained = static_cast<std::byte*>(child_value.resource()->allocate(8192));
        std::fill_n(retained, 8192, std::byte{0x3c});
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            ruvia::request_memory independent(worker);
            auto* scratch = static_cast<std::byte*>(independent.resource()->allocate(8192));
            std::fill_n(scratch, 8192, std::byte{0xaa});
            RUVIA_CHECK(retained[0] == std::byte{0x3c} && retained[8191] == std::byte{0x3c});
        }
    }
    RUVIA_CHECK_EQ(upstream.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.returns_);
    RUVIA_CHECK(upstream.matched_returns_);
}
