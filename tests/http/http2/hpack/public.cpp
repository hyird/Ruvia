#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/hpack.h"

#include "poisoned_retirement_resource.h"
#include "stl_allocation_fixture.h"
#include "test_exceptions.h"

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

// RFC 7541 section 5.1 prefix integer, used to craft wire input.
void append_prefixed_integer(std::string& output, std::uint8_t flags, unsigned prefix_bits, std::uint32_t value) {
    const std::uint32_t prefix_limit = (1U << prefix_bits) - 1U;
    if (value < prefix_limit) {
        output.push_back(static_cast<char>(flags | value));
        return;
    }
    output.push_back(static_cast<char>(flags | prefix_limit));
    value -= prefix_limit;
    while (value >= 128) {
        output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

[[nodiscard]] std::string table_size_update(std::uint32_t bytes) {
    std::string output;
    append_prefixed_integer(output, 0x20, 5, bytes);
    return output;
}

[[nodiscard]] std::string indexed_field(std::uint32_t index) {
    std::string output;
    append_prefixed_integer(output, 0x80, 7, index);
    return output;
}

// Literal with incremental indexing and a new name (RFC 7541 section 6.2.1),
// without Huffman coding.
[[nodiscard]] std::string indexed_literal(std::string_view name, std::string_view value) {
    std::string output(1, static_cast<char>(0x40));
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(name.size()));
    output.append(name);
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(value.size()));
    output.append(value);
    return output;
}

void prepare_alias_output(std::pmr::string& output, bool grow) {
    output = "content-type|authorization|x-alias-name|abcdefghijklmnopqrstuvwxyz";
    output.append(192, 'v');
    if (grow) {
        output.resize(output.capacity(), '#');
    } else {
        output.reserve(output.size() * 4 + 128);
    }
}

void check_encoded_field(std::string_view output, std::string_view prefix,
    std::string_view expected_name, std::string_view expected_value) {
    EXPECT_EQ(output.substr(0, prefix.size()), prefix);
    ruvia::hpack_decoder decoder;
    std::size_t fields = 0;
    const auto result = decoder.decode(output.substr(prefix.size()),
        [&](std::string_view name, std::string_view value) {
            ++fields;
            EXPECT_EQ(name, expected_name);
            EXPECT_EQ(value, expected_value);
            return true;
        });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

void check_newest(ruvia::hpack_decoder& decoder,
    std::uint32_t index, std::string_view expected_name, std::string_view expected_value) {
    std::size_t fields = 0;
    const auto result = decoder.decode(indexed_field(index), [&](std::string_view name, std::string_view value) {
        EXPECT_EQ(name, expected_name);
        EXPECT_EQ(value, expected_value);
        ++fields;
        return true;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

[[nodiscard]] bool decoder_is_poisoned(ruvia::hpack_decoder& decoder) {
    std::size_t fields = 0;
    const auto result = decoder.decode(std::string_view("\x82", 1), [&](auto, auto) {
        ++fields;
        return true;
    });
    return result == ruvia::hpack_decode_error::compression_error && fields == 0;
}

}  // namespace

TEST(Ruvia, hpack_public_encoder_accepts_fields_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        identical,
        static_name,
        sensitive_name };
    for (const bool grow : {false, true}) {
        for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
                 alias_kind::overlapping, alias_kind::identical, alias_kind::static_name,
                 alias_kind::sensitive_name}) {
            ruvia::testing::poisoned_retirement_resource resource;
            {
                std::pmr::string output(&resource);
                prepare_alias_output(output, grow);
                const std::string prefix(output);
                const auto* storage = output.data();
                const auto contents = std::string_view(output);
                const auto payload = contents.find("abcdefghijklmnopqrstuvwxyz");
                std::string_view name = "x-outside";
                std::string_view value = "outside-value";
                switch (kind) {
                    case alias_kind::name:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        break;
                    case alias_kind::value:
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::disjoint:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::overlapping:
                        name = contents.substr(payload, 40);
                        value = contents.substr(payload + 11, 80);
                        break;
                    case alias_kind::identical:
                        name = contents.substr(payload, 60);
                        value = name;
                        break;
                    case alias_kind::static_name:
                        name = contents.substr(0, 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::sensitive_name:
                        name = contents.substr(contents.find("authorization"), 13);
                        value = contents.substr(payload, 100);
                        break;
                }
                const std::string expected_name(name);
                const std::string expected_value(value);
                ruvia::hpack_encoder::encode_header(output, name, value);
                EXPECT_TRUE(resource.retired(storage) == grow);
                check_encoded_field(output, prefix, expected_name, expected_value);
            }
            EXPECT_EQ(resource.live_bytes(), std::size_t{0});
        }
    }
}

TEST(Ruvia, hpack_public_encoder_never_indexes_sensitive_fields) {
    // RFC 7541 section 7.1.3: credentials use the never-indexed literal form,
    // even when the static table holds an exact name/value match.
    for (const std::string_view name : {"authorization", "proxy-authorization", "cookie", "set-cookie"}) {
        std::pmr::string output;
        ruvia::hpack_encoder::encode_header(output, name, "");
        EXPECT_TRUE(!output.empty());
        EXPECT_EQ(static_cast<unsigned char>(output[0]) & 0xf0U, 0x10U);
        check_encoded_field(output, {}, name, "");
    }
}

TEST(Ruvia, hpack_public_table_size_rejects_out_of_range_integers_and_excess_updates) {
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
             static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1}) {
        EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value(invalid).has_value());
        ruvia::hpack_decoder decoder;
        bool rejected_limit = false;
        try {
            decoder.set_max_dynamic_table_size(invalid);
        } catch (const std::invalid_argument&) {
            rejected_limit = true;
        }
        EXPECT_TRUE(rejected_limit);
    }
    EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value((std::numeric_limits<std::uint64_t>::max)()).has_value());

    ruvia::hpack_decoder decoder;
    decoder.set_max_dynamic_table_size(64);
    EXPECT_TRUE(!decoder.decode(table_size_update(64), [](auto, auto) { return true; }).has_value());
    EXPECT_TRUE(decoder.decode(table_size_update(65), [](auto, auto) { return true; }) ==
                ruvia::hpack_decode_error::compression_error);
    EXPECT_TRUE(decoder_is_poisoned(decoder));
}

TEST(Ruvia, hpack_public_lowered_capacity_requires_a_leading_update) {
    // RFC 7541 section 4.2: the first block after the decoder's maximum shrinks
    // must begin with a dynamic table size update.
    for (const bool signaled : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        decoder.set_max_dynamic_table_size(0);
        std::string block = signaled ? table_size_update(0) : std::string();
        block.push_back(static_cast<char>(0x82));
        std::size_t fields = 0;
        const auto result = decoder.decode(block, [&](auto, auto) {
            ++fields;
            return true;
        });
        if (signaled) {
            EXPECT_TRUE(!result.has_value());
            EXPECT_EQ(fields, std::size_t{1});
            EXPECT_TRUE(decoder.decode(indexed_field(62), [](auto, auto) { return true; }) ==
                        ruvia::hpack_decode_error::compression_error);
        } else {
            EXPECT_TRUE(result == ruvia::hpack_decode_error::compression_error);
            EXPECT_EQ(fields, std::size_t{0});
        }
    }
}

TEST(Ruvia, hpack_public_encoder_preserves_output_when_alias_copy_fails) {
    toggle_allocation_resource resource;
    std::pmr::string output(256, 'a', &resource);
    const std::string before(output);
    const auto borrowed = std::string_view(output);
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        ruvia::hpack_encoder::encode_header(output, borrowed, borrowed);
        check_encoded_field(output, before, before, before);
        return;
    }
    resource.reject();
    bool threw = false;
    try {
        ruvia::hpack_encoder::encode_header(output, borrowed, borrowed);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(std::string_view(output), std::string_view(before));
}

TEST(Ruvia, hpack_public_encoder_keeps_output_when_literal_growth_fails) {
    for (const std::string_view name : {"x-field", "content-type"}) {
        std::array<std::byte, 256> storage{};
        std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size(), std::pmr::null_memory_resource());
        std::pmr::string output("prefix", &resource);
        const std::string value(4096, 'v');
        bool threw = false;
        try {
            ruvia::hpack_encoder::encode_header(output, name, value);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(std::string_view(output), std::string_view("prefix"));
        output.append(" reusable");
        EXPECT_EQ(std::string_view(output), std::string_view("prefix reusable"));
    }
}

TEST(Ruvia, hpack_public_encoder_preserves_the_output_terminator_as_input) {
    for (const bool empty_output : {false, true}) {
        std::pmr::string output(empty_output ? "" : "prefix");
        const std::string prefix(output);
        const std::string_view value(output.data() + output.size(), 1);
        ruvia::hpack_encoder::encode_header(output, "content-type", value);
        check_encoded_field(output, prefix, "content-type", std::string_view("\0", 1));
    }
}

TEST(Ruvia, hpack_public_dynamic_entries_own_fields_after_input_changes) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        ruvia::hpack_decoder decoder({.resource_ = &resource});
        {
            std::string block = indexed_literal(std::string(24, 'n'), std::string(24, 'v'));
            EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
            block.assign(block.size(), '#');
        }
        {
            // Huffman-coded literal: the decoded bytes cannot borrow the input.
            std::pmr::string block(std::pmr::new_delete_resource());
            ruvia::hpack_encoder::encode_header(block, std::string(24, 'a'), std::string(25, 'b'));
            block[0] = static_cast<char>(0x40);
            EXPECT_TRUE(!decoder.decode(std::string_view(block), [](auto, auto) { return true; }).has_value());
            std::fill(block.begin(), block.end(), '#');
        }
        check_newest(decoder, 62, std::string(24, 'a'), std::string(25, 'b'));
        check_newest(decoder, 63, std::string(24, 'n'), std::string(24, 'v'));
        // RFC 7541 Appendix C.4.1: ":authority: www.example.com" as a Huffman literal.
        constexpr std::string_view huffman_authority(
            "\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 14);
        EXPECT_TRUE(!decoder.decode(huffman_authority, [](std::string_view name, std::string_view value) {
                                return name == ":authority" && value == "www.example.com";
                            })
                .has_value());
    }
}

TEST(Ruvia, hpack_public_empty_and_one_sided_fields_are_indexable) {
    ruvia::hpack_decoder decoder;
    const std::string long_bytes(49, 'x');
    const std::string block = indexed_literal("", "") + indexed_literal("", long_bytes) + indexed_literal(long_bytes, "");
    EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_bytes, "");
    check_newest(decoder, 63, "", long_bytes);
    check_newest(decoder, 64, "", "");
}

TEST(Ruvia, hpack_public_dynamic_entries_remain_indexable_across_evictions) {
    ruvia::hpack_decoder decoder;
    EXPECT_TRUE(!decoder.decode(table_size_update(120), [](auto, auto) { return true; }).has_value());
    const std::string name(40, 'n');
    const std::string value(40, 'v');
    const std::string insertion = indexed_literal(name, value);
    for (std::size_t i = 0; i < 200; ++i) {
        EXPECT_TRUE(!decoder.decode(insertion, [](auto, auto) { return true; }).has_value());
        check_newest(decoder, 62, name, value);
    }
}

TEST(Ruvia, hpack_public_leading_capacity_updates_apply_in_wire_order) {
    const std::array<std::array<std::uint32_t, 3>, 3> sequences{{{512, 64, 64}, {512, 64, 512}, {0, 0, 512}}};
    for (const auto& updates : sequences) {
        ruvia::hpack_decoder decoder;
        const std::string seed_value(64, 's');
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", seed_value), [](auto, auto) { return true; }).has_value());
        std::string block;
        for (const auto size : updates) {
            block += table_size_update(size);
        }
        block += indexed_literal("x", "fresh");
        std::size_t fields = 0;
        const auto decoded = decoder.decode(block, [&](std::string_view name, std::string_view value) {
            EXPECT_EQ(name, std::string_view("x"));
            EXPECT_EQ(value, std::string_view("fresh"));
            ++fields;
            return true;
        });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields, std::size_t{1});
        check_newest(decoder, 62, "x", "fresh");
        // Restoring capacity cannot restore an entry evicted at an intermediate minimum.
        EXPECT_TRUE(decoder.decode(indexed_field(63), [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
    }
}

TEST(Ruvia, hpack_public_capacity_updates_reject_excess_and_late_positions) {
    for (const bool late : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", "kept"), [](auto, auto) { return true; }).has_value());
        const std::string invalid = late ? indexed_field(62) + table_size_update(0) : table_size_update(4097);
        EXPECT_TRUE(decoder.decode(invalid, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::hpack_decoder decoder;
    const auto rejected = decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return false; });
    EXPECT_TRUE(rejected == ruvia::hpack_decode_error::callback_rejected);
    check_newest(decoder, 62, "a", "b");
}

TEST(Ruvia, hpack_public_compression_error_poisons_the_decoder) {
    for (const std::string_view failing : {std::string_view("\x80", 1), std::string_view("\x40\x05\x61", 3)}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        std::string block = indexed_literal("c", "d");
        block.append(failing);
        EXPECT_TRUE(decoder.decode(block, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_allocation_failure_throws_and_poisons_the_decoder) {
    toggle_allocation_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});
    resource.reject();
    bool threw = false;
    try {
        (void)decoder.decode(indexed_literal("x-insert", std::string(64, 'v')), [](auto, auto) { return true; });
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    resource.reject(false);
    EXPECT_TRUE(decoder_is_poisoned(decoder));
}

TEST(Ruvia, hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::hpack_decoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    EXPECT_TRUE(!decoder.decode(indexed_literal(name, value), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, name, value);
}

TEST(Ruvia, hpack_public_eviction_preserves_relative_indices) {
    ruvia::hpack_decoder decoder;
    const std::string long_name(48, 'n');
    std::string initial = table_size_update(1150);
    for (int i = 0; i < 36; ++i) {
        const char value = static_cast<char>('A' + i);
        initial += indexed_literal(i % 2 == 0 ? std::string_view("a") : std::string_view(long_name),
            std::string_view(&value, 1));
    }
    initial.push_back(static_cast<char>(0x82));
    EXPECT_TRUE(!decoder.decode(initial, [](auto, auto) { return true; }).has_value());
    std::string indices;
    for (std::uint32_t index = 62; index < 82; ++index) {
        indices += indexed_field(index);
    }
    int expected = 35;
    const auto result = decoder.decode(indices, [&](std::string_view name, std::string_view value) {
        const auto expected_name = expected % 2 == 0 ? std::string_view("a") : std::string_view(long_name);
        const bool matches = name == expected_name && value.size() == 1 && value[0] == 'A' + expected;
        --expected;
        return matches;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(expected, 15);
    // An indexed name survives the eviction its own insertion causes.
    EXPECT_TRUE(!decoder.decode(std::string_view("\x7e\x01z", 3), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_name, "z");
}

TEST(Ruvia, hpack_public_decoder_move_preserves_dynamic_entries) {
    std::pmr::unsynchronized_pool_resource first_resource;
    std::pmr::unsynchronized_pool_resource second_resource;
    {
        ruvia::hpack_decoder source_value({.resource_ = &first_resource});
        EXPECT_TRUE(!source_value.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        ruvia::hpack_decoder moved(std::move(source_value));
        ruvia::hpack_decoder destination({.resource_ = &second_resource});
        destination = std::move(moved);
        check_newest(destination, 62, "a", "b");
    }

    toggle_allocation_resource unused_resource;
    {
        ruvia::hpack_decoder unused({.resource_ = &unused_resource});
    }
    unused_resource.reject();
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        ruvia::hpack_decoder rejected({.resource_ = &unused_resource});
    }));
}

#if !defined(_MSC_VER)
TEST(Ruvia, hpack_public_callback_exception_precedes_continuation_allocation_failure) {
    toggle_allocation_resource resource;
    ruvia::hpack_decoder decoder({.resource_ = &resource});

    std::string block(1, static_cast<char>(0x82));  // indexed :method: GET
    block += indexed_literal(std::string(100, 'x'), "y");

    bool saw_original = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            resource.reject();
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        saw_original = std::string_view(error.what()) == "original callback failure";
    }
    EXPECT_TRUE(saw_original);
}
#endif

TEST(Ruvia, hpack_public_callback_cannot_replace_the_active_block_or_its_capacity) {
    for (const bool resize : {false, true}) {
        ruvia::hpack_decoder decoder;
        ruvia::hpack_decoder independent;
        std::size_t callbacks = 0;
        std::size_t rejected = 0;
        const auto result = decoder.decode(indexed_literal("a", "b"), [&](std::string_view name, std::string_view value) {
            ++callbacks;
            try {
                if (resize) {
                    decoder.set_max_dynamic_table_size(0);
                } else {
                    (void)decoder.decode({}, [](auto, auto) { return true; });
                }
            } catch (const std::logic_error&) {
                ++rejected;
            }
            EXPECT_EQ(name, "a");
            EXPECT_EQ(value, "b");
            std::size_t independent_fields = 0;
            EXPECT_TRUE(!independent.decode(std::string_view("\x82", 1), [&](auto inner_name, auto inner_value) {
                                        ++independent_fields;
                                        EXPECT_EQ(inner_name, ":method");
                                        EXPECT_EQ(inner_value, "GET");
                                        return true;
                                    })
                    .has_value());
            EXPECT_EQ(independent_fields, std::size_t{1});
            return true;
        });
        EXPECT_TRUE(!result.has_value());
        EXPECT_EQ(callbacks, std::size_t{1});
        EXPECT_EQ(rejected, std::size_t{1});
        check_newest(decoder, 62, "a", "b");
    }
}

TEST(Ruvia, hpack_public_callback_state_error_is_rethrown_after_the_block) {
    for (const bool resize : {false, true}) {
        for (const bool malformed : {false, true}) {
            ruvia::hpack_decoder decoder;
            const auto accept = [](auto, auto) { return true; };
            EXPECT_TRUE(!decoder.decode(indexed_literal("s", "t"), accept).has_value());
            std::string block = indexed_literal("a", "b") + indexed_literal("c", "d");
            if (malformed) {
                block.push_back(static_cast<char>(0x80));
            }
            std::size_t callbacks = 0;
            bool state_error = false;
            try {
                (void)decoder.decode(block, [&](auto, auto) {
                    ++callbacks;
                    if (resize) {
                        decoder.set_max_dynamic_table_size(0);
                    } else {
                        (void)decoder.decode({}, accept);
                    }
                    return true;
                });
            } catch (const std::logic_error&) {
                state_error = true;
            }
            EXPECT_TRUE(state_error);
            EXPECT_EQ(callbacks, std::size_t{1});
            if (malformed) {
                EXPECT_TRUE(decoder_is_poisoned(decoder));
            } else {
                check_newest(decoder, 62, "c", "d");
                check_newest(decoder, 63, "a", "b");
            }
        }
    }
}
