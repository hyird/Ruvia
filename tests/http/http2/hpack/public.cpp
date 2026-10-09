#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/hpack.h"

#include "test_harness.h"

namespace {

class toggle_allocation_resource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

}  // namespace

RUVIA_TEST(hpack_public_dynamic_entries_own_fields_after_input_changes) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        std::string warm_entries;
        for (char name = 'a'; name < 'i'; name += 2) {
            warm_entries.push_back(static_cast<char>(0x40));
            warm_entries.push_back('\x01');
            warm_entries.push_back(name);
            warm_entries.push_back('\x01');
            warm_entries.push_back(static_cast<char>(name + 1));
        }
        RUVIA_CHECK(decoder.decode(warm_entries, [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\x20", 1), [](auto, auto) { return true; }).decoded());
        std::pmr::string restore_limit(std::pmr::new_delete_resource());
        ruvia::hpack_encoder::encode_dynamic_table_size_update(restore_limit, 4096);
        RUVIA_CHECK(decoder.decode(std::string_view(restore_limit), [](auto, auto) { return true; }).decoded());

        {
            std::string inline_name(24, 'n');
            std::string inline_value(24, 'v');
            std::pmr::string inline_block(std::pmr::new_delete_resource());
            ruvia::hpack_encoder::encode_header(inline_block, inline_name, inline_value);
            inline_block[0] = static_cast<char>(0x40);
            RUVIA_CHECK(
                decoder.decode(std::string_view(inline_block), [](auto, auto) { return true; }).decoded());
            std::fill(inline_block.begin(), inline_block.end(), '#');
            std::fill(inline_name.begin(), inline_name.end(), '#');
            std::fill(inline_value.begin(), inline_value.end(), '#');
        }
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto name, auto value) {
                               return name == std::string(24, 'n') && value == std::string(24, 'v');
                           })
                .decoded());

        {
            std::string heap_name(24, 'a');
            std::string heap_value(25, 'b');
            std::pmr::string heap_block(std::pmr::new_delete_resource());
            ruvia::hpack_encoder::encode_header(heap_block, heap_name, heap_value);
            heap_block[0] = static_cast<char>(0x40);
            RUVIA_CHECK(
                decoder.decode(std::string_view(heap_block), [](auto, auto) { return true; }).decoded());
            std::fill(heap_block.begin(), heap_block.end(), '#');
            std::fill(heap_name.begin(), heap_name.end(), '#');
            std::fill(heap_value.begin(), heap_value.end(), '#');
        }
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto name, auto value) {
                               return name == std::string(24, 'a') && value == std::string(25, 'b');
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbf", 1), [](auto name, auto value) {
                               return name == std::string(24, 'n') && value == std::string(24, 'v');
                           })
                .decoded());
    }
}

RUVIA_TEST(hpack_public_empty_and_one_sided_fields_are_owned_and_indexable) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        const std::string long_bytes(49, 'x');
        std::pmr::string block(std::pmr::new_delete_resource());
        ruvia::hpack_encoder::encode_header(block, "", "");
        block[0] = static_cast<char>(0x40);
        const auto second_field = block.size();
        ruvia::hpack_encoder::encode_header(block, "", long_bytes);
        block[second_field] = static_cast<char>(0x40);
        const auto third_field = block.size();
        ruvia::hpack_encoder::encode_header(block, long_bytes, "");
        block[third_field] = static_cast<char>(0x40);
        RUVIA_CHECK(decoder.decode(std::string_view(block), [](auto, auto) { return true; }).decoded());
        std::fill(block.begin(), block.end(), '#');

        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [&](auto name, auto value) {
                               return name == long_bytes && value.empty();
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbf", 1), [&](auto name, auto value) {
                               return name.empty() && value == long_bytes;
                           })
                .decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xc0", 1), [](auto name, auto value) {
                               return name.empty() && value.empty();
                           })
                .decoded());
    }
}

RUVIA_TEST(hpack_public_dynamic_entries_remain_indexable_across_evictions) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        const std::string size_update{static_cast<char>(0x3f), static_cast<char>(0x59)};
        RUVIA_CHECK(decoder.decode(size_update, [](auto, auto) { return true; }).decoded());

        const std::string name(40, 'n');
        const std::string value(40, 'v');
        std::string insertion(1, static_cast<char>(0x40));
        insertion.push_back(static_cast<char>(name.size()));
        insertion += name;
        insertion.push_back(static_cast<char>(value.size()));
        insertion += value;

        for (std::size_t i = 0; i < 200; ++i) {
            RUVIA_CHECK(decoder.decode(insertion, [](auto, auto) { return true; }).decoded());
            RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                                   [&](std::string_view actual_name, std::string_view actual_value) {
                                       return actual_name == name && actual_value == value;
                                   })
                    .decoded());
        }
    }
}

RUVIA_TEST(hpack_public_size_update_evicts_entries_and_accepts_new_insertions) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});
    const std::string name(1500, 'n');
    const std::string value(1500, 'v');
    std::pmr::string insertion(std::pmr::get_default_resource());
    ruvia::hpack_encoder::encode_header(insertion, name, value);
    insertion[0] = static_cast<char>(0x40);
    RUVIA_CHECK(decoder.decode(std::string_view(insertion), [](auto, auto) { return true; }).decoded());

    std::pmr::string evict(std::pmr::get_default_resource());
    ruvia::hpack_encoder::encode_dynamic_table_size_update(evict, 0);
    RUVIA_CHECK(decoder.decode(std::string_view(evict), [](auto, auto) { return true; }).decoded());

    std::pmr::string small(std::pmr::get_default_resource());
    ruvia::hpack_encoder::encode_dynamic_table_size_update(small, 4096);
    small.append(std::string_view("\x40\x01\x61\x01\x62", 5));
    RUVIA_CHECK(decoder.decode(std::string_view(small), [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [](auto name_view, auto value_view) {
                           return name_view == "a" && value_view == "b";
                       })
            .decoded());
}

RUVIA_TEST(hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::hpack_decoder decoder;
    const std::string insertion{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
    const auto rejected = decoder.decode(insertion, [](auto, auto) { return false; });
    RUVIA_CHECK(!rejected.decoded());
    RUVIA_CHECK(rejected.error() == ruvia::hpack_decode_error::callback_rejected);

    bool indexed_header_found = false;
    const char indexed_byte = static_cast<char>(0xbe);
    const auto indexed = decoder.decode(std::string_view(&indexed_byte, 1),
        [&indexed_header_found](std::string_view name, std::string_view value) {
            indexed_header_found = name == "a" && value == "b";
            return true;
        });
    RUVIA_CHECK(indexed.decoded());
    RUVIA_CHECK(indexed_header_found);
}

RUVIA_TEST(hpack_public_failed_decode_rolls_back_dynamic_table) {
    ruvia::hpack_decoder decoder;
    const std::string first{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
    RUVIA_CHECK(decoder.decode(first, [](auto, auto) { return true; }).decoded());

    const std::string failing{static_cast<char>(0x40), 0x01, 'c', 0x01, 'd',
        static_cast<char>(0x80)};
    RUVIA_CHECK(!decoder.decode(failing, [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                           [](std::string_view name, std::string_view value) {
                               return name == "a" && value == "b";
                           })
            .decoded());
    const auto absent = decoder.decode(std::string_view("\xbf", 1), [](auto, auto) { return true; });
    RUVIA_CHECK(!absent.decoded());
}

RUVIA_TEST(hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::hpack_decoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    std::string insertion(1, static_cast<char>(0x40));
    const auto append_length = [&insertion](std::size_t length) {
        insertion.push_back(static_cast<char>(0x7f));
        length -= 127;
        while (length >= 128) {
            insertion.push_back(static_cast<char>((length & 0x7f) | 0x80));
            length >>= 7;
        }
        insertion.push_back(static_cast<char>(length));
    };
    append_length(name.size());
    insertion += name;
    append_length(value.size());
    insertion += value;
    RUVIA_CHECK(decoder.decode(insertion, [](auto, auto) { return true; }).decoded());
    RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1),
                           [&](std::string_view actual_name, std::string_view actual_value) {
                               return actual_name == name && actual_value == value;
                           })
            .decoded());
}

RUVIA_TEST(hpack_public_eviction_preserves_indices_and_rollback) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        const std::string long_name(48, 'n');
        std::pmr::string initial;
        ruvia::hpack_encoder::encode_dynamic_table_size_update(initial, 1150);
        for (int i = 0; i < 36; ++i) {
            std::pmr::string field;
            const char value = static_cast<char>('A' + i);
            ruvia::hpack_encoder::encode_header(field,
                i % 2 == 0 ? std::string_view("a") : std::string_view(long_name),
                std::string_view(&value, 1));
            field[0] = static_cast<char>(0x40);
            initial += field;
        }
        initial.push_back(static_cast<char>(0x82));
        RUVIA_CHECK(decoder.decode(initial, [](std::string_view, std::string_view) {
                               return true;
                           })
                .decoded());
        std::pmr::string indices;
        for (std::uint32_t index = 62; index < 82; ++index) {
            ruvia::hpack_encoder::encode_indexed(indices, index);
        }
        const auto verify_indices = [&] {
            int expected = 35;
            const auto result_value = decoder.decode(indices, [&](std::string_view name, std::string_view value) {
                const auto expected_name = expected % 2 == 0 ? std::string_view("a") : std::string_view(long_name);
                const bool matches = name == expected_name && value.size() == 1 && value[0] == 'A' + expected;
                --expected;
                return matches;
            });
            RUVIA_CHECK(result_value.decoded());
            RUVIA_CHECK_EQ(expected, 15);
        };
        verify_indices();
        // Fail after a literal that evicts an entry, then verify rollback.
        const std::string failed{static_cast<char>(0x7e), 0x01, 'z', static_cast<char>(0x80)};
        RUVIA_CHECK(!decoder.decode(failed, [](auto, auto) { return true; }).decoded());
        verify_indices();
        RUVIA_CHECK(decoder.decode(std::string_view(failed).substr(0, 3), [](auto, auto) { return true; }).decoded());
        RUVIA_CHECK(decoder.decode(std::string_view("\xbe", 1), [&](std::string_view name, std::string_view value) {
                               return name == long_name && value == "z";
                           })
                .decoded());
    }
}

RUVIA_TEST(hpack_public_decoder_move_preserves_dynamic_entries) {
    std::pmr::unsynchronized_pool_resource first_resource;
    std::pmr::unsynchronized_pool_resource second_resource;
    {
        ruvia::hpack_decoder source_value({.resource_ = &first_resource});
        const std::string insertion{static_cast<char>(0x40), 0x01, 'a', 0x01, 'b'};
        RUVIA_CHECK(source_value.decode(insertion, [](auto, auto) { return true; }).decoded());
        ruvia::hpack_decoder moved(std::move(source_value));
        ruvia::hpack_decoder destination({.resource_ = &second_resource});
        destination = std::move(moved);
        RUVIA_CHECK(destination.decode(std::string_view("\xbe", 1), [](std::string_view name, std::string_view value) {
                                   return name == "a" && value == "b";
                               })
                .decoded());
    }

    toggle_allocation_resource unused_resource;
    {
        ruvia::hpack_decoder unused({.resource_ = &unused_resource});
    }
    unused_resource.reject();
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        ruvia::hpack_decoder rejected({.resource_ = &unused_resource});
    }));
}

#if !defined(_MSC_VER)
RUVIA_TEST(hpack_public_callback_exception_precedes_continuation_allocation_failure) {
    toggle_allocation_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});

    std::string block;
    block.push_back(static_cast<char>(0x82));  // indexed :method: GET
    block.push_back(static_cast<char>(0x40));  // incremental literal, new name
    block.push_back(static_cast<char>(100));
    block.append(100, 'x');
    block.push_back(1);
    block.push_back('y');

    bool saw_original = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            resource.reject();
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        saw_original = std::string_view(error.what()) == "original callback failure";
    }
    RUVIA_CHECK(saw_original);
}
#endif
