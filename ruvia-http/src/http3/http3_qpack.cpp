#include "ruvia/http/http3_qpack.h"

#include <array>
#include <limits>
#include <string_view>

#include "field/hpack_huffman.h"
#include "http3/qpack_static_table.h"

namespace ruvia {
std::variant<http3_qpack_static_entry, http3_qpack_error> get_http3_qpack_static_entry(
    std::uint64_t index) noexcept {
    if (index >= detail::qpack_static_table.size()) {
        return http3_qpack_error::invalid_index;
    }
    return detail::qpack_static_table[static_cast<std::size_t>(index)];
}

std::variant<http3_qpack_integer, http3_qpack_error> decode_http3_qpack_integer(
    std::span<const char> input, std::uint8_t prefix_bits) noexcept {
    if (prefix_bits == 0 || prefix_bits > 8) {
        return http3_qpack_error::integer_overflow;
    }
    if (input.empty()) {
        return http3_qpack_error::need_more_data;
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefix_bits) - 1U);
    std::uint64_t value = static_cast<std::uint8_t>(input[0]) & mask;
    if (value < mask) {
        return http3_qpack_integer{value, 1};
    }
    unsigned shift = 0;
    std::size_t offset = 1;
    for (;;) {
        if (offset == input.size()) {
            return http3_qpack_error::need_more_data;
        }
        const auto byte = static_cast<std::uint8_t>(input[offset++]);
        const auto payload_value = static_cast<std::uint64_t>(byte & 0x7fU);
        if (shift >= 64 || payload_value > (std::numeric_limits<std::uint64_t>::max() - value) >> shift) {
            return http3_qpack_error::integer_overflow;
        }
        value += payload_value << shift;
        if ((byte & 0x80U) == 0) {
            return http3_qpack_integer{value, offset};
        }
        shift += 7;
    }
}

std::variant<std::size_t, http3_qpack_error> encode_http3_qpack_integer(std::span<char> output,
    std::uint8_t prefix_bits, std::uint8_t prefix, std::uint64_t value) noexcept {
    if (prefix_bits == 0 || prefix_bits > 8) {
        return http3_qpack_error::integer_overflow;
    }
    const auto mask = static_cast<std::uint8_t>((1U << prefix_bits) - 1U);
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
        return http3_qpack_error::output_too_small;
    }
    for (std::size_t i = 0; i < size; ++i) {
        output[i] = encoded[i];
    }
    return size;
}

std::variant<std::size_t, http3_qpack_error> decode_http3_qpack_string(
    std::span<const char> input, std::pmr::string& output) {
    return decode_http3_qpack_string(input, 7, output);
}

std::variant<std::size_t, http3_qpack_error> decode_http3_qpack_string(
    std::span<const char> input, std::uint8_t prefix_bits, std::pmr::string& output) {
    if (prefix_bits == 0 || prefix_bits > 7) {
        return http3_qpack_error::integer_overflow;
    }
    if (input.empty()) {
        return http3_qpack_error::need_more_data;
    }
    const bool huffman =
        (static_cast<std::uint8_t>(input[0]) & (1U << prefix_bits)) != 0;
    const auto length = decode_http3_qpack_integer(input, prefix_bits);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    const auto prefix_bytes = std::get<0>(length).encoded_bytes_;
    if (std::get<0>(length).value_ > input.size() - prefix_bytes) {
        return http3_qpack_error::need_more_data;
    }
    const auto encoded = std::string_view(input.data() + prefix_bytes, static_cast<std::size_t>(std::get<0>(length).value_));
    if (huffman) {
        output.clear();
        if (!detail::append_hpack_huffman(encoded, output)) {
            output.clear();
            return http3_qpack_error::invalid_huffman;
        }
    } else {
        output.assign(encoded);
    }
    return prefix_bytes + static_cast<std::size_t>(std::get<0>(length).value_);
}

std::variant<std::size_t, http3_qpack_error> encode_http3_qpack_string(std::span<char> output,
    std::string_view value) noexcept {
    std::array<char, 11> length{};
    const auto length_bytes = encode_http3_qpack_integer(length, 7, 0, value.size());
    if ((length_bytes.index() != 0)) {
        return std::get<1>(length_bytes);
    }
    if (output.size() < std::get<0>(length_bytes) + value.size()) {
        return http3_qpack_error::output_too_small;
    }
    for (std::size_t i = 0; i < std::get<0>(length_bytes); ++i) {
        output[i] = length[i];
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        output[std::get<0>(length_bytes) + i] = value[i];
    }
    return std::get<0>(length_bytes) + value.size();
}

}  // namespace ruvia
