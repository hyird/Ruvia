#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_io_buffer.h"

#include "test_exceptions.h"

namespace {

void write_text(std::span<char> target, std::string_view text) {
    std::memcpy(target.data(), text.data(), text.size());
}

enum class reallocation_operation { reserve,
    shrink,
    append,
    append_alias };

}  // namespace

TEST(Ruvia, io_buffer_publishes_only_committed_bytes) {
    {
        ruvia::http_io_buffer buffer;
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
}

TEST(Ruvia, io_buffer_grows_geometrically_and_keeps_readable_bytes) {
    ruvia::http_io_buffer buffer;
    buffer.append("abc");
    EXPECT_EQ(buffer.capacity(), std::size_t{3});
    write_text(buffer.prepare(1), "d");
    buffer.commit(1);
    EXPECT_EQ(buffer.capacity(), std::size_t{6});
    EXPECT_EQ(buffer.view(), "abcd");
    const std::string large(100, 'x');
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

TEST(Ruvia, io_buffer_small_reads_preserve_the_sliding_window) {
    for (const bool prepared : {false, true}) {
        {
            ruvia::http_io_buffer buffer;
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
    }
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
