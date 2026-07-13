#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2HpackHuffmanTables.h"

#include <array>
#include <cstddef>

namespace ruvia::detail {

namespace {

constexpr std::size_t kHpackHuffmanNodeCapacity = 1024;

struct HpackHuffmanTree final {
    std::array<HpackHuffmanNode, kHpackHuffmanNodeCapacity> nodes{};
    std::size_t size{1};
};

consteval HpackHuffmanTree buildHpackHuffmanTree() {
    HpackHuffmanTree tree;
    for (std::size_t symbol = 0; symbol < kHpackHuffmanCodes.size(); ++symbol) {
        std::int16_t node = 0;
        const auto code = kHpackHuffmanCodes[symbol];
        const auto length = kHpackHuffmanLengths[symbol];
        for (std::uint8_t bitIndex = 0; bitIndex < length; ++bitIndex) {
            const auto shift = static_cast<std::uint8_t>(length - bitIndex - 1);
            const auto bit = static_cast<std::uint8_t>((code >> shift) & 0x1U);
            auto& next = tree.nodes[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                next = static_cast<std::int16_t>(tree.size);
                tree.nodes[tree.size] = HpackHuffmanNode{};
                ++tree.size;
            }
            node = next;
        }
        tree.nodes[static_cast<std::size_t>(node)].symbol = static_cast<std::int16_t>(symbol);
    }
    return tree;
}

inline constexpr auto kHpackHuffmanTree = buildHpackHuffmanTree();
static_assert(kHpackHuffmanTree.size <= kHpackHuffmanNodeCapacity);

}  // namespace

HpackDecoder::StepResult HpackDecoder::decodeHuffman(std::string_view encoded, std::pmr::string& output) {
    output.clear();
    output.reserve(encoded.size());
    std::int16_t node = 0;
    std::uint8_t depth = 0;
    bool allOnes = true;

    for (const auto byteValue : encoded) {
        const auto byte = static_cast<unsigned char>(byteValue);
        for (int bitIndex = 7; bitIndex >= 0; --bitIndex) {
            const auto bit = static_cast<std::uint8_t>((byte >> bitIndex) & 0x1U);
            const auto next = kHpackHuffmanTree.nodes[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                return HpackDecodeError::kInvalidHuffman;
            }
            node = next;
            ++depth;
            allOnes = allOnes && bit == 1;

            const auto symbol = kHpackHuffmanTree.nodes[static_cast<std::size_t>(node)].symbol;
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
