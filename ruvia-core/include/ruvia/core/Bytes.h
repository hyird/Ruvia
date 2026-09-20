#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace ruvia {

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> asBytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

[[nodiscard]] inline std::string_view asChars(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace ruvia
