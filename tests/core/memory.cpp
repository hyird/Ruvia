#include "ruvia/core/memory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct alignas(4096) retained_process_value final {
    std::array<std::byte, 4096> bytes_;
};

}  // namespace

TEST(Ruvia, collect_thread_heap_preserves_live_storage) {
    std::string retained(30000, 'r');
    const auto* const retained_address = retained.data();
    std::vector<std::byte> small(17, std::byte{0x33});
    {
        std::vector<std::byte> scratch(1024, std::byte{0xaa});
        (void)scratch;
    }

    ruvia::collect_thread_heap();
    ruvia::collect_thread_heap();

    EXPECT_TRUE(retained.data() == retained_address);
    EXPECT_EQ(retained.size(), std::size_t{30000});
    EXPECT_EQ(retained.find_first_not_of('r'), std::string::npos);
    EXPECT_TRUE(std::ranges::all_of(small, [](std::byte value) {
        return value == std::byte{0x33};
    }));
}

TEST(Ruvia, storage_survives_its_allocating_thread) {
    constexpr std::size_t text_bytes = 2 * 1024 * 1024;
    std::optional<std::string> text;
    std::optional<std::vector<retained_process_value>> values;
    std::thread([&] {
        text.emplace(text_bytes, 's');
        values.emplace(3);
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
    EXPECT_EQ(text->find_first_not_of('s'), std::string::npos);

    bool preserved_after_growth = false;
    std::thread([&] {
        values->reserve(values->capacity() + 1);
        text->append(text->capacity() + 1, 't');
        preserved_after_growth = text->find_first_not_of('s') == text_bytes &&
                                 text->find_first_not_of('t', text_bytes) == std::string::npos;
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
