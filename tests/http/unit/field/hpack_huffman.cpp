#include "field/hpack_huffman.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "field/hpack_huffman_tables.h"
#include "test_harness.h"

namespace {

std::string encode_huffman(std::string_view input) {
    std::string encoded;
    std::uint64_t pending = 0;
    unsigned pending_bits = 0;
    for (const auto byte : input) {
        const auto symbol = static_cast<unsigned char>(byte);
        const auto length = ruvia::detail::hpack_huffman_lengths[symbol];
        pending = (pending << length) | ruvia::detail::hpack_huffman_codes[symbol];
        pending_bits += length;
        while (pending_bits >= 8) {
            pending_bits -= 8;
            encoded.push_back(static_cast<char>(pending >> pending_bits));
        }
    }
    if (pending_bits != 0) {
        const auto padding_bits = 8 - pending_bits;
        encoded.push_back(static_cast<char>((pending << padding_bits) | ((1U << padding_bits) - 1)));
    }
    return encoded;
}

}  // namespace

RUVIA_TEST(hpack_huffman_appends_all_byte_pairs_at_each_bit_alignment) {
    std::pmr::string decoded;
    // The five-bit code for '0' places each following pair at all eight bit offsets.
    for (std::size_t prefix = 0; prefix < 8; ++prefix) {
        std::string input(prefix, '0');
        input.resize(prefix + 2);
        for (unsigned first = 0; first < 256; ++first) {
            input[prefix] = static_cast<char>(first);
            for (unsigned second = 0; second < 256; ++second) {
                input[prefix + 1] = static_cast<char>(second);
                decoded = "prefix:";
                RUVIA_CHECK(ruvia::detail::append_hpack_huffman(encode_huffman(input), decoded));
                RUVIA_CHECK_EQ(std::string_view(decoded).substr(7), std::string_view(input));
                RUVIA_CHECK_EQ(std::string_view(decoded).substr(0, 7), std::string_view("prefix:"));
            }
        }
    }
}

RUVIA_TEST(hpack_huffman_accepts_empty_and_all_legal_padding_lengths) {
    std::pmr::string decoded("unchanged");
    RUVIA_CHECK(ruvia::detail::append_hpack_huffman({}, decoded));
    RUVIA_CHECK_EQ(decoded, std::string_view("unchanged"));
    for (std::size_t count = 1; count <= 8; ++count) {
        const std::string input(count, '0');
        const auto encoded = encode_huffman(input);
        decoded.clear();
        RUVIA_CHECK(ruvia::detail::append_hpack_huffman(encoded, decoded));
        RUVIA_CHECK_EQ(std::string_view(decoded), std::string_view(input));
    }
}

RUVIA_TEST(hpack_huffman_rejects_eos_and_incomplete_codes_at_each_bit_alignment) {
    for (std::size_t prefix = 0; prefix < 8; ++prefix) {
        // Strip the encoder's padding, append the thirty-bit EOS, then another '0'.
        const auto prefix_bits = static_cast<unsigned>(prefix * 5);
        const auto bit_count = prefix_bits + 30 + 5;
        const auto padding = (8 - bit_count % 8) % 8;
        // Every '0' code is five zero bits, so only EOS contributes one bits.
        const auto bits = (std::uint64_t{0x3fffffff} << (5 + padding)) | ((1U << padding) - 1);
        std::string encoded((bit_count + 7) / 8, '\0');
        for (std::size_t byte = 0; byte < encoded.size(); ++byte) {
            const auto shift = (encoded.size() - byte - 1) * 8;
            encoded[byte] = shift < 64 ? static_cast<char>(bits >> shift) : '\0';
        }
        std::pmr::string decoded;
        RUVIA_CHECK(!ruvia::detail::append_hpack_huffman(encoded, decoded));
    }
    for (std::size_t count = 1; count <= 3; ++count) {
        std::pmr::string decoded;
        RUVIA_CHECK(!ruvia::detail::append_hpack_huffman(std::string(count, '\xff'), decoded));
    }
    std::pmr::string decoded;
    RUVIA_CHECK(!ruvia::detail::append_hpack_huffman(std::string(1, '\0'), decoded));
}
