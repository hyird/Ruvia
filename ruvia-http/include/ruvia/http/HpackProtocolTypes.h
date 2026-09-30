#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

class HpackDecoder;
namespace detail {
class HpackDecoder;
}

enum class HpackDecodeError : std::uint8_t {
    kNeedMore,
    kIntegerOverflow,
    kInvalidIndex,
    kInvalidString,
    kInvalidHuffman,
    kDynamicTableSize,
    kCallbackRejected,
};

class HpackDecodeResult final {
public:
    [[nodiscard]] constexpr bool decoded() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] constexpr std::optional<HpackDecodeError> error() const noexcept {
        return error_;
    }

private:
    friend class HpackDecoder;
    friend class detail::HpackDecoder;

    explicit constexpr HpackDecodeResult(
        std::optional<HpackDecodeError> error = std::nullopt) noexcept
        : error_(error) {}

    std::optional<HpackDecodeError> error_{};
};

}  // namespace ruvia
