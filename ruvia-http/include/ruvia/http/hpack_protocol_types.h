#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

class hpack_decoder;
namespace detail {
class hpack_decoder;
}

enum class hpack_decode_error : std::uint8_t {
    need_more,
    integer_overflow,
    invalid_index,
    invalid_string,
    invalid_huffman,
    dynamic_table_size,
    callback_rejected,
};

class hpack_decode_result final {
public:
    [[nodiscard]] constexpr bool decoded() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] constexpr std::optional<hpack_decode_error> error() const noexcept {
        return error_;
    }

private:
    friend class hpack_decoder;
    friend class detail::hpack_decoder;

    explicit constexpr hpack_decode_result(
        std::optional<hpack_decode_error> error = std::nullopt) noexcept
        : error_(error) {}

    std::optional<hpack_decode_error> error_{};
};

}  // namespace ruvia
