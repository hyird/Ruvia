#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_io_buffer.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::test::counting_memory_resource;

void write_text(std::span<char> target, std::string_view text) {
    std::memcpy(target.data(), text.data(), text.size());
}

struct retirement_observation final {
    ruvia::http_io_buffer* buffer_{};
    std::string_view expected_{};
    std::optional<std::size_t> capacity_{};
    bool committed_{false};
    std::unique_ptr<ruvia::http_io_buffer>* owner_{};
};

void observe_retirement(void* context) noexcept {
    auto& observation = *static_cast<retirement_observation*>(context);
    const auto capacity = observation.buffer_->capacity();
    observation.committed_ = capacity >= observation.expected_.size() &&
                             (!observation.capacity_ || capacity == *observation.capacity_) &&
                             observation.buffer_->view() == observation.expected_;
    // Only exercise reentry after the complete state is published.
    if (!observation.committed_) {
        return;
    }
    if (observation.owner_ != nullptr) {
        observation.owner_->reset();
        return;
    }
    observation.buffer_->clear();
    observation.buffer_->append("new");
}

enum class reallocation_operation { reserve,
    shrink,
    append,
    append_alias };

}  // namespace

TEST(Ruvia, io_buffer_publishes_only_committed_bytes) {
    counting_memory_resource resource;
    {
        ruvia::http_io_buffer buffer(&resource);
        EXPECT_TRUE(buffer.empty());
        auto space = buffer.prepare(16);
        EXPECT_EQ(space.size(), std::size_t{16});
        EXPECT_TRUE(buffer.empty());
        write_text(space, "hello");
        buffer.commit(5);
        EXPECT_EQ(buffer.view(), "hello");
        EXPECT_TRUE(buffer.capacity() >= std::size_t{16});
        EXPECT_TRUE(ruvia::test::throws_on([&] { buffer.commit(buffer.capacity()); }));
        EXPECT_EQ(buffer.view(), "hello");
    }
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
}

TEST(Ruvia, io_buffer_consume_keeps_views_until_room_is_needed) {
    counting_memory_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    buffer.reserve(8);
    EXPECT_EQ(buffer.capacity(), std::size_t{8});
    buffer.append("abcdef");
    const auto consumed = buffer.view();
    buffer.consume(4);
    // Consuming never moves bytes: the earlier view still reads them.
    EXPECT_EQ(consumed, "abcdef");
    EXPECT_EQ(buffer.view(), "ef");
    // The tail has 2 bytes; 4 more fit only by moving the unread tail forward.
    const auto allocations = resource.allocation_count();
    write_text(buffer.prepare(4), "ghij");
    buffer.commit(4);
    EXPECT_EQ(resource.allocation_count(), allocations);
    EXPECT_EQ(buffer.capacity(), std::size_t{8});
    EXPECT_EQ(buffer.view(), "efghij");
    // Consuming past the end drops everything and rewinds the window.
    buffer.consume(100);
    EXPECT_TRUE(buffer.empty());
    write_text(buffer.prepare(8), "12345678");
    buffer.commit(8);
    EXPECT_EQ(resource.allocation_count(), allocations);
    EXPECT_EQ(buffer.view(), "12345678");
}

TEST(Ruvia, io_buffer_grows_geometrically_and_keeps_readable_bytes) {
    counting_memory_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    buffer.append("abc");
    EXPECT_EQ(buffer.capacity(), std::size_t{3});
    write_text(buffer.prepare(1), "d");
    buffer.commit(1);
    EXPECT_EQ(buffer.capacity(), std::size_t{6});
    EXPECT_EQ(buffer.view(), "abcd");
    const std::pmr::string large(100, 'x');
    buffer.append(large);
    EXPECT_EQ(buffer.view(), "abcd" + large);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)buffer.prepare(static_cast<std::size_t>(-1)); }));
    EXPECT_EQ(buffer.view(), "abcd" + large);
}

TEST(Ruvia, io_buffer_append_accepts_its_own_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdef");
    // Fits the tail.
    buffer.append(buffer.view().substr(0, 2));
    EXPECT_EQ(buffer.view(), "abcdefab");
    // Needs new storage while the source lives in the old one.
    buffer.append(buffer.view());
    EXPECT_EQ(buffer.view(), "abcdefababcdefab");
    // Needs the room of the consumed prefix while the source lies inside it.
    ruvia::http_io_buffer window;
    window.reserve(8);
    window.append("abcdefgh");
    const auto consumed = window.view();
    window.consume(6);
    window.append(consumed.substr(0, 4));
    EXPECT_EQ(window.view(), "ghabcd");
    EXPECT_EQ(window.capacity(), std::size_t{8});
}

TEST(Ruvia, io_buffer_releases_storage_by_policy) {
    counting_memory_resource resource;
    {
        ruvia::http_io_buffer buffer(&resource);
        buffer.reserve(64);
        buffer.append("small");
        buffer.clear_retaining_small(64);
        EXPECT_TRUE(buffer.empty());
        EXPECT_EQ(buffer.capacity(), std::size_t{64});
        EXPECT_EQ(resource.live_bytes(), std::size_t{64});
        buffer.append("large");
        buffer.clear_retaining_small(32);
        EXPECT_EQ(buffer.capacity(), std::size_t{0});
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});

        buffer.reserve(256);
        buffer.append("0123456789");
        buffer.consume(4);
        buffer.shrink_to(16);
        EXPECT_EQ(buffer.capacity(), std::size_t{16});
        EXPECT_EQ(resource.live_bytes(), std::size_t{16});
        EXPECT_EQ(buffer.view(), "456789");
        // Never below the readable bytes.
        buffer.shrink_to(0);
        EXPECT_EQ(buffer.capacity(), std::size_t{6});
        EXPECT_EQ(buffer.view(), "456789");

        ruvia::http_io_buffer moved(std::move(buffer));
        EXPECT_TRUE(buffer.empty());
        EXPECT_EQ(buffer.capacity(), std::size_t{0});
        EXPECT_EQ(moved.view(), "456789");
        EXPECT_TRUE(moved.get_allocator().resource() == &resource);
    }
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
}

TEST(Ruvia, io_buffer_small_reads_preserve_the_sliding_window) {
    counting_memory_resource resource;
    for (const bool prepared : {false, true}) {
        {
            ruvia::http_io_buffer buffer(&resource);
            std::pmr::string expected(64, '\0');
            for (std::size_t index = 0; index < expected.size(); ++index) {
                expected[index] = static_cast<char>(index);
            }
            buffer.append(expected);
            for (std::size_t index = 0; index < 1024; ++index) {
                buffer.consume(1);
                expected.erase(0, 1);
                const char byte = static_cast<char>(index);
                if (prepared) {
                    buffer.prepare(1)[0] = byte;
                    buffer.commit(1);
                } else {
                    buffer.append(std::string_view(&byte, 1));
                }
                expected.push_back(byte);
                EXPECT_EQ(buffer.view(), std::string_view(expected));
            }
            // A self-append can grow or compact; it must read the old window.
            buffer.consume(32);
            expected.erase(0, 32);
            const auto copied = expected;
            buffer.append(buffer.view());
            expected.append(copied);
            EXPECT_EQ(buffer.view(), std::string_view(expected));
        }
        EXPECT_EQ(resource.live_bytes(), std::size_t{0});
    }
}

TEST(Ruvia, io_buffer_spare_read_cycles_preserve_content_without_growing) {
    counting_memory_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    buffer.reserve(128);
    std::pmr::string expected(64, 'a');
    buffer.append(expected);
    for (std::size_t round = 0; round < 4096; ++round) {
        buffer.consume(1);
        expected.erase(0, 1);
        const auto spare = buffer.prepare_spare();
        ASSERT_FALSE(spare.empty());
        EXPECT_EQ(buffer.view(), std::string_view(expected));
        if (round == 0) {
            EXPECT_TRUE(ruvia::test::throws_on([&] { buffer.commit(spare.size() + 1); }));
            buffer.commit(0);
            EXPECT_EQ(buffer.view(), std::string_view(expected));
        }
        const char byte = static_cast<char>('a' + round % 26);
        spare[0] = byte;
        buffer.commit(1);
        expected.push_back(byte);
        EXPECT_EQ(buffer.view(), std::string_view(expected));
        EXPECT_EQ(buffer.capacity(), std::size_t{128});
    }
    EXPECT_EQ(resource.allocation_count(), std::size_t{1});
}

TEST(Ruvia, io_buffer_spare_handles_empty_full_and_partial_windows) {
    counting_memory_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    EXPECT_TRUE(buffer.prepare_spare().empty());
    EXPECT_TRUE(buffer.prepare(0).empty());
    buffer.commit(0);
    buffer.consume(0);
    buffer.append({});
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});

    buffer.reserve(8);
    const auto spare = buffer.prepare_spare();
    ASSERT_GE(spare.size(), std::size_t{8});
    write_text(spare, "abcdefgh");
    buffer.commit(8);
    EXPECT_TRUE(buffer.prepare_spare().empty());
    EXPECT_TRUE(ruvia::test::throws_on([&] { buffer.commit(1); }));
    buffer.commit(0);
    EXPECT_EQ(buffer.view(), "abcdefgh");

    buffer.consume(6);
    const auto reclaimed = buffer.prepare_spare();
    ASSERT_GE(reclaimed.size(), std::size_t{3});
    write_text(reclaimed, "ijk");
    buffer.commit(3);
    EXPECT_EQ(buffer.view(), "ghijk");
    EXPECT_EQ(buffer.capacity(), std::size_t{8});
    EXPECT_EQ(resource.allocation_count(), std::size_t{1});

    buffer.append(buffer.view().substr(0, 2));
    EXPECT_EQ(buffer.view(), "ghijkgh");
    buffer.consume(buffer.size());
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.prepare_spare().size(), std::size_t{8});
}

TEST(Ruvia, io_buffer_reader_prepares_when_no_spare_is_available) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdefgh");
    buffer.consume(1);
    auto space = buffer.prepare_spare();
    if (space.empty()) {
        space = buffer.prepare(1);
    }
    ASSERT_FALSE(space.empty());
    space[0] = 'i';
    buffer.commit(1);
    EXPECT_EQ(buffer.view(), "bcdefghi");
}

TEST(Ruvia, io_buffer_clear_detaches_storage_before_reentrant_refill) {
    ruvia::test::reentrant_memory_resource resource;
    retirement_observation observation;
    {
        ruvia::http_io_buffer buffer(&resource);
        buffer.append("seed");
        observation = {&buffer, {}, 0};
        resource.before_deallocate_ = observe_retirement;
        resource.deallocation_context_ = &observation;

        buffer.clear_retaining_small(0);

        EXPECT_TRUE(observation.committed_);
        EXPECT_EQ(buffer.view(), "new");
    }
    EXPECT_EQ(resource.live_bytes(), 0U);
}

TEST(Ruvia, io_buffer_reallocation_commits_before_reentrant_refill) {
    for (const auto operation : {reallocation_operation::reserve, reallocation_operation::shrink,
             reallocation_operation::append, reallocation_operation::append_alias}) {
        SCOPED_TRACE(static_cast<int>(operation));
        ruvia::test::reentrant_memory_resource resource;
        retirement_observation observation;
        {
            ruvia::http_io_buffer buffer(&resource);
            buffer.reserve(operation == reallocation_operation::shrink ? 16 : 4);
            buffer.append("seed");
            observation = {&buffer, "seed", 8};
            if (operation == reallocation_operation::shrink) {
                observation.capacity_ = 4;
            } else if (operation == reallocation_operation::append) {
                observation.expected_ = "seedmore";
                observation.capacity_.reset();
            } else if (operation == reallocation_operation::append_alias) {
                observation.expected_ = "seedseed";
                observation.capacity_.reset();
            }
            resource.before_deallocate_ = observe_retirement;
            resource.deallocation_context_ = &observation;

            switch (operation) {
                case reallocation_operation::reserve:
                    buffer.reserve(8);
                    break;
                case reallocation_operation::shrink:
                    buffer.shrink_to(4);
                    break;
                case reallocation_operation::append:
                    buffer.append("more");
                    break;
                case reallocation_operation::append_alias:
                    buffer.append(buffer.view());
                    break;
            }

            EXPECT_TRUE(observation.committed_);
            EXPECT_EQ(buffer.view(), "new");
        }
        EXPECT_EQ(resource.live_bytes(), 0U);
    }
}

TEST(Ruvia, io_buffer_retirement_can_destroy_its_owner) {
    for (const bool release : {false, true}) {
        SCOPED_TRACE(release);
        ruvia::test::reentrant_memory_resource resource;
        retirement_observation observation;
        {
            auto owner = std::make_unique<ruvia::http_io_buffer>(&resource);
            owner->reserve(4);
            owner->append("seed");
            observation = {owner.get(), release ? "" : "seed", release ? 0U : 8U, false, &owner};
            resource.before_deallocate_ = observe_retirement;
            resource.deallocation_context_ = &observation;

            if (release) {
                owner->clear_retaining_small(0);
            } else {
                owner->reserve(8);
            }

            EXPECT_TRUE(observation.committed_);
            EXPECT_EQ(owner, nullptr);
        }
        EXPECT_EQ(resource.live_bytes(), 0U);
    }
}

TEST(Ruvia, io_buffer_allocation_failure_preserves_borrow_for_retry) {
    ruvia::test::counting_memory_resource resource;
    {
        ruvia::http_io_buffer buffer(&resource);
        buffer.reserve(4);
        buffer.append("seed");
        const auto borrowed = buffer.view();
        resource.reject_allocations(true);

        EXPECT_THROW(buffer.append(borrowed), std::bad_alloc);
        EXPECT_EQ(buffer.view(), "seed");
        EXPECT_EQ(borrowed, "seed");

        resource.reject_allocations(false);
        buffer.append(borrowed);
        EXPECT_EQ(buffer.view(), "seedseed");
    }
    EXPECT_EQ(resource.live_bytes(), 0U);
}
