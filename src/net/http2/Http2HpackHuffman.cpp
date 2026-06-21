#include "Http2Hpack.h"
#include "Http2HpackHuffmanTables.h"

namespace ruvia::detail {

HpackError HpackDecoder::decodeHuffman(std::string_view encoded, std::pmr::string& output) {
    output.clear();
    output.reserve(encoded.size());
    std::int16_t node = 0;
    std::uint8_t depth = 0;
    bool allOnes = true;

    for (const auto byteValue : encoded) {
        const auto byte = static_cast<unsigned char>(byteValue);
        for (int bitIndex = 7; bitIndex >= 0; --bitIndex) {
            const auto bit = static_cast<std::uint8_t>((byte >> bitIndex) & 0x1U);
            const auto next = huffman_[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                return HpackError::kInvalidHuffman;
            }
            node = next;
            ++depth;
            allOnes = allOnes && bit == 1;

            const auto symbol = huffman_[static_cast<std::size_t>(node)].symbol;
            if (symbol >= 0) {
                if (symbol == 256) {
                    return HpackError::kInvalidHuffman;
                }
                output.push_back(static_cast<char>(symbol));
                node = 0;
                depth = 0;
                allOnes = true;
            }
        }
    }

    if (node != 0 && (depth > 7 || !allOnes)) {
        return HpackError::kInvalidHuffman;
    }
    return HpackError::kNone;
}

void HpackDecoder::buildHuffmanTree() noexcept {
    huffman_[0] = HuffmanNode{};
    huffmanNodeCount_ = 1;
    for (std::size_t symbol = 0; symbol < kHpackHuffmanCodes.size(); ++symbol) {
        std::int16_t node = 0;
        const auto code = kHpackHuffmanCodes[symbol];
        const auto length = kHpackHuffmanLengths[symbol];
        for (std::uint8_t bitIndex = 0; bitIndex < length; ++bitIndex) {
            const auto shift = static_cast<std::uint8_t>(length - bitIndex - 1);
            const auto bit = static_cast<std::uint8_t>((code >> shift) & 0x1U);
            auto& next = huffman_[static_cast<std::size_t>(node)].child[bit];
            if (next < 0) {
                next = static_cast<std::int16_t>(huffmanNodeCount_);
                huffman_[huffmanNodeCount_] = HuffmanNode{};
                ++huffmanNodeCount_;
            }
            node = next;
        }
        huffman_[static_cast<std::size_t>(node)].symbol = static_cast<std::int16_t>(symbol);
    }
}

}  // namespace ruvia::detail
