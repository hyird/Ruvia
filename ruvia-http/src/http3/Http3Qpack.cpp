#include "ruvia/http/Http3Qpack.h"

#include <array>
#include <limits>
#include <string_view>

#include "field/hpack_huffman.h"
#include "http3/qpack_static_table.h"

namespace ruvia {
std::variant<Http3QpackStaticEntry, Http3QpackError> http3QpackStaticEntry(
    std::uint64_t index) noexcept {
    if (index >= detail::qpack_static_table.size()) {
        return Http3QpackError::kInvalidIndex;
    }
    return detail::qpack_static_table[static_cast<std::size_t>(index)];
}

std::variant<Http3QpackInteger, Http3QpackError> decodeHttp3QpackInteger(
    std::span<const char> input, std::uint8_t prefixBits) noexcept {
    if (prefixBits == 0 || prefixBits > 8) {
        return Http3QpackError::kIntegerOverflow;
    }
    if (input.empty()) {
        return Http3QpackError::kNeedMoreData;
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefixBits) - 1U);
    std::uint64_t value = static_cast<std::uint8_t>(input[0]) & mask;
    if (value < mask) {
        return Http3QpackInteger{value, 1};
    }
    unsigned shift = 0;
    std::size_t offset = 1;
    for (;;) {
        if (offset == input.size()) {
            return Http3QpackError::kNeedMoreData;
        }
        const auto byte = static_cast<std::uint8_t>(input[offset++]);
        const auto payload = static_cast<std::uint64_t>(byte & 0x7fU);
        if (shift >= 64 || payload > (std::numeric_limits<std::uint64_t>::max() - value) >> shift) {
            return Http3QpackError::kIntegerOverflow;
        }
        value += payload << shift;
        if ((byte & 0x80U) == 0) {
            return Http3QpackInteger{value, offset};
        }
        shift += 7;
    }
}

std::variant<std::size_t, Http3QpackError> encodeHttp3QpackInteger(std::span<char> output,
    std::uint8_t prefixBits, std::uint8_t prefix, std::uint64_t value) noexcept {
    if (prefixBits == 0 || prefixBits > 8) {
        return Http3QpackError::kIntegerOverflow;
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefixBits) - 1U);
    std::array<char, 11> encoded{};
    std::size_t size = 0;
    if (value < mask) {
        encoded[size++] = static_cast<char>((prefix & ~mask) | static_cast<std::uint8_t>(value));
    } else {
        encoded[size++] = static_cast<char>((prefix & ~mask) | mask);
        value -= mask;
        while (value >= 128) {
            encoded[size++] = static_cast<char>((value & 0x7fU) | 0x80U);
            value >>= 7;
        }
        encoded[size++] = static_cast<char>(value);
    }
    if (output.size() < size) {
        return Http3QpackError::kOutputTooSmall;
    }
    for (std::size_t i = 0; i < size; ++i) {
        output[i] = encoded[i];
    }
    return size;
}

std::variant<std::size_t, Http3QpackError> decodeHttp3QpackString(
    std::span<const char> input, std::pmr::string& output) {
    return decodeHttp3QpackString(input, 7, output);
}

std::variant<std::size_t, Http3QpackError> decodeHttp3QpackString(
    std::span<const char> input, std::uint8_t prefixBits, std::pmr::string& output) {
    if (prefixBits == 0 || prefixBits > 7) {
        return Http3QpackError::kIntegerOverflow;
    }
    if (input.empty()) {
        return Http3QpackError::kNeedMoreData;
    }
    const bool huffman =
        (static_cast<std::uint8_t>(input[0]) & (1U << prefixBits)) != 0;
    const auto length = decodeHttp3QpackInteger(input, prefixBits);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    const auto prefixBytes = std::get<0>(length).encodedBytes;
    if (std::get<0>(length).value > input.size() - prefixBytes) {
        return Http3QpackError::kNeedMoreData;
    }
    const auto encoded = std::string_view(input.data() + prefixBytes, static_cast<std::size_t>(std::get<0>(length).value));
    if (huffman) {
        output.clear();
        if (!detail::append_hpack_huffman(encoded, output)) {
            output.clear();
            return Http3QpackError::kInvalidHuffman;
        }
    } else {
        output.assign(encoded);
    }
    return prefixBytes + static_cast<std::size_t>(std::get<0>(length).value);
}

std::variant<std::size_t, Http3QpackError> encodeHttp3QpackString(std::span<char> output,
    std::string_view value) noexcept {
    std::array<char, 11> length{};
    const auto lengthBytes = encodeHttp3QpackInteger(length, 7, 0, value.size());
    if ((lengthBytes.index() != 0)) {
        return std::get<1>(lengthBytes);
    }
    if (output.size() < std::get<0>(lengthBytes) + value.size()) {
        return Http3QpackError::kOutputTooSmall;
    }
    for (std::size_t i = 0; i < std::get<0>(lengthBytes); ++i) {
        output[i] = length[i];
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        output[std::get<0>(lengthBytes) + i] = value[i];
    }
    return std::get<0>(lengthBytes) + value.size();
}

}  // namespace ruvia
