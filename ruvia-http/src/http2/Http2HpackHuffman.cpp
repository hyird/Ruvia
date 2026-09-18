#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/hpack/Http2HpackHuffmanTables.h"

namespace ruvia::detail {

namespace {

// A complete binary prefix tree with N symbols has exactly 2*N-1 nodes.
constexpr std::size_t kHpackHuffmanNodeCount = kHpackHuffmanCodes.size() * 2 - 1;
using HpackHuffmanTree = std::array<HpackHuffmanNode, kHpackHuffmanNodeCount>;
static_assert(kHpackHuffmanNodeCount <= (std::numeric_limits<std::int16_t>::max)());
static_assert(kHpackHuffmanCodes.size() == kHpackHuffmanLengths.size());

consteval HpackHuffmanTree buildHpackHuffmanTree() {
    HpackHuffmanTree tree{};
    std::size_t size = 1;
    for (std::size_t symbol = 0; symbol < kHpackHuffmanCodes.size(); ++symbol) {
        std::int16_t node = 0;
        const auto code = kHpackHuffmanCodes[symbol];
        const auto length = kHpackHuffmanLengths[symbol];
        for (std::uint8_t bitIndex = 0; bitIndex < length; ++bitIndex) {
            if (tree[static_cast<std::size_t>(node)].symbol >= 0) {
                throw "HPACK Huffman code has an existing symbol as a prefix";
            }
            const auto shift = static_cast<std::uint8_t>(length - bitIndex - 1);
            const auto bit = static_cast<std::uint8_t>((code >> shift) & 0x1U);
            auto& next = tree[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                if (size == tree.size()) {
                    throw "HPACK Huffman tree exceeds the complete-tree node count";
                }
                next = static_cast<std::int16_t>(size++);
            }
            node = next;
        }
        auto& leaf = tree[static_cast<std::size_t>(node)];
        if (leaf.symbol >= 0 || leaf.child[0] >= 0 || leaf.child[1] >= 0) {
            throw "HPACK Huffman code duplicates or prefixes another symbol";
        }
        leaf.symbol = static_cast<std::int16_t>(symbol);
    }
    if (size != tree.size()) {
        throw "HPACK Huffman tree is incomplete";
    }
    return tree;
}

inline constexpr auto kHpackHuffmanTree = buildHpackHuffmanTree();

}  // namespace

HpackDecoder::StepResult HpackDecoder::decodeHuffman(
    std::string_view encoded, std::pmr::string& output) {
    output.clear();
    output.reserve(encoded.size());
    std::int16_t node = 0;
    std::uint8_t depth = 0;
    bool allOnes = true;

    for (const auto byteValue : encoded) {
        const auto byte = static_cast<unsigned char>(byteValue);
        for (int bitIndex = 7; bitIndex >= 0; --bitIndex) {
            const auto bit = static_cast<std::uint8_t>((byte >> bitIndex) & 0x1U);
            const auto next = kHpackHuffmanTree[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                return HpackDecodeError::kInvalidHuffman;
            }
            node = next;
            ++depth;
            allOnes = allOnes && bit == 1;

            const auto symbol = kHpackHuffmanTree[static_cast<std::size_t>(node)].symbol;
            if (symbol >= 0) {
                if (symbol == 256) {
                    return HpackDecodeError::kInvalidHuffman;
                }
                output.push_back(static_cast<char>(symbol));
                node = 0;
                depth = 0;
                allOnes = true;
            }
        }
    }

    if (node != 0 && (depth > 7 || !allOnes)) {
        return HpackDecodeError::kInvalidHuffman;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
