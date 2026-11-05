#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

// Decodes HPACK Huffman bytes into `output` (which must have room for at least
// `encoded.size() + (encoded.size() * 3U) / 5U` bytes) and writes the decoded
// length to `decoded_size`. Returns false on invalid Huffman input or padding.
[[nodiscard]] bool decode_hpack_huffman(
    std::string_view encoded, char* output, std::size_t& decoded_size) noexcept;

// Appends decoded bytes using output's resource. Invalid input returns false and
// may leave a decoded prefix appended; the caller owns rollback and capacity policy.
[[nodiscard]] bool append_hpack_huffman(std::string_view encoded, std::pmr::string& output);
}  // namespace ruvia::detail
