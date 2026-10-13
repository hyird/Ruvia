#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_io_buffer.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{0};
    std::size_t allocations_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++allocations_;
        return result_value;
    }

    void do_deallocate(void* address, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(address, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

void write_text(std::span<char> target, std::string_view text) {
    std::memcpy(target.data(), text.data(), text.size());
}

}  // namespace

RUVIA_TEST(io_buffer_publishes_only_committed_bytes) {
    counting_resource resource;
    {
        ruvia::http_io_buffer buffer(&resource);
        RUVIA_CHECK(buffer.empty());
        auto space = buffer.prepare(16);
        RUVIA_CHECK_EQ(space.size(), std::size_t{16});
        RUVIA_CHECK(buffer.empty());
        write_text(space, "hello");
        buffer.commit(5);
        RUVIA_CHECK_EQ(buffer.view(), "hello");
        RUVIA_CHECK(buffer.capacity() >= std::size_t{16});
        RUVIA_CHECK(ruvia::testing::throws_on([&] { buffer.commit(buffer.capacity()); }));
        RUVIA_CHECK_EQ(buffer.view(), "hello");
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

RUVIA_TEST(io_buffer_consume_keeps_views_until_room_is_needed) {
    counting_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    buffer.reserve(8);
    RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{8});
    buffer.append("abcdef");
    const auto consumed = buffer.view();
    buffer.consume(4);
    // Consuming never moves bytes: the earlier view still reads them.
    RUVIA_CHECK_EQ(consumed, "abcdef");
    RUVIA_CHECK_EQ(buffer.view(), "ef");
    // The tail has 2 bytes; 4 more fit only by moving the unread tail forward.
    const auto allocations = resource.allocations_;
    write_text(buffer.prepare(4), "ghij");
    buffer.commit(4);
    RUVIA_CHECK_EQ(resource.allocations_, allocations);
    RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{8});
    RUVIA_CHECK_EQ(buffer.view(), "efghij");
    // Consuming past the end drops everything and rewinds the window.
    buffer.consume(100);
    RUVIA_CHECK(buffer.empty());
    write_text(buffer.prepare(8), "12345678");
    buffer.commit(8);
    RUVIA_CHECK_EQ(resource.allocations_, allocations);
    RUVIA_CHECK_EQ(buffer.view(), "12345678");
}

RUVIA_TEST(io_buffer_grows_geometrically_and_keeps_readable_bytes) {
    counting_resource resource;
    ruvia::http_io_buffer buffer(&resource);
    buffer.append("abc");
    RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{3});
    write_text(buffer.prepare(1), "d");
    buffer.commit(1);
    RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{6});
    RUVIA_CHECK_EQ(buffer.view(), "abcd");
    const std::string large(100, 'x');
    buffer.append(large);
    RUVIA_CHECK_EQ(buffer.view(), "abcd" + large);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)buffer.prepare(static_cast<std::size_t>(-1)); }));
    RUVIA_CHECK_EQ(buffer.view(), "abcd" + large);
}

RUVIA_TEST(io_buffer_append_accepts_its_own_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.reserve(8);
    buffer.append("abcdef");
    // Fits the tail.
    buffer.append(buffer.view().substr(0, 2));
    RUVIA_CHECK_EQ(buffer.view(), "abcdefab");
    // Needs new storage while the source lives in the old one.
    buffer.append(buffer.view());
    RUVIA_CHECK_EQ(buffer.view(), "abcdefababcdefab");
    // Needs the room of the consumed prefix while the source lies inside it.
    ruvia::http_io_buffer window;
    window.reserve(8);
    window.append("abcdefgh");
    const auto consumed = window.view();
    window.consume(6);
    window.append(consumed.substr(0, 4));
    RUVIA_CHECK_EQ(window.view(), "ghabcd");
    RUVIA_CHECK_EQ(window.capacity(), std::size_t{8});
}

RUVIA_TEST(io_buffer_releases_storage_by_policy) {
    counting_resource resource;
    {
        ruvia::http_io_buffer buffer(&resource);
        buffer.reserve(64);
        buffer.append("small");
        buffer.clear_retaining_small(64);
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{64});
        RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{64});
        buffer.append("large");
        buffer.clear_retaining_small(32);
        RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});

        buffer.reserve(256);
        buffer.append("0123456789");
        buffer.consume(4);
        buffer.shrink_to(16);
        RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{16});
        RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{16});
        RUVIA_CHECK_EQ(buffer.view(), "456789");
        // Never below the readable bytes.
        buffer.shrink_to(0);
        RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{6});
        RUVIA_CHECK_EQ(buffer.view(), "456789");

        ruvia::http_io_buffer moved(std::move(buffer));
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(buffer.capacity(), std::size_t{0});
        RUVIA_CHECK_EQ(moved.view(), "456789");
        RUVIA_CHECK(moved.get_allocator().resource() == &resource);
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

RUVIA_TEST(io_buffer_small_reads_preserve_the_sliding_window) {
    counting_resource resource;
    for (const bool prepared : {false, true}) {
        {
            ruvia::http_io_buffer buffer(&resource);
            std::string expected(64, '\0');
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
                RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
            }
            // A self-append can grow or compact; it must read the old window.
            buffer.consume(32);
            expected.erase(0, 32);
            const auto copied = expected;
            buffer.append(buffer.view());
            expected.append(copied);
            RUVIA_CHECK_EQ(buffer.view(), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
    }
}
