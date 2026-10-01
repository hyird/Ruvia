#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace ruvia {

struct HttpPriority final {
    std::uint8_t urgency{3};
    bool incremental{false};
};
struct HttpPriorityFields final {
    std::optional<std::uint8_t> urgency{};
    std::optional<bool> incremental{};
    [[nodiscard]] constexpr HttpPriority requestPriority() const noexcept {
        return {urgency.value_or(3), incremental.value_or(false)};
    }
};
enum class HttpPriorityError : std::uint8_t { kInvalidSyntax,
    kInvalidValue,
    kInvalidFrame,
    kOutputTooSmall };
struct HttpPriorityUpdate final {
    std::uint64_t elementId{0};
    bool push{false};
    HttpPriorityFields fields{};
};

// RFC 9218 / RFC 8941 Dictionary parsing. Unknown members, invalid parameter
// types and out-of-range values are ignored; malformed structured syntax fails.
// Missing response parameters remain absent for intermediary merging.
[[nodiscard]] std::expected<HttpPriorityFields, HttpPriorityError> parseHttpPriority(std::string_view value) noexcept;
[[nodiscard]] std::expected<std::size_t, HttpPriorityError> encodeHttpPriority(std::span<char> output, HttpPriorityFields fields) noexcept;
[[nodiscard]] std::expected<HttpPriorityUpdate, HttpPriorityError> decodeHttp2PriorityUpdate(std::span<const char> payload) noexcept;
[[nodiscard]] std::expected<std::size_t, HttpPriorityError> encodeHttp2PriorityUpdate(std::span<char> output, std::uint32_t streamId, HttpPriorityFields fields) noexcept;
[[nodiscard]] std::expected<HttpPriorityUpdate, HttpPriorityError> decodeHttp3PriorityUpdate(std::uint64_t frameType, std::span<const char> payload) noexcept;
[[nodiscard]] std::expected<std::size_t, HttpPriorityError> encodeHttp3PriorityUpdate(std::span<char> output, HttpPriorityUpdate update) noexcept;

}  // namespace ruvia
