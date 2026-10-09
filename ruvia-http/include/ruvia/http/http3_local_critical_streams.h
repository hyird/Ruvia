#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/http3_settings.h"

namespace ruvia {

enum class http3_local_critical_streams_error : unsigned char {
    settings_encoding_error,
};

// Fixed, independently sendable prefixes for the three locally initiated
// unidirectional critical streams. These prefixes never include FIN: callers
// must keep all three streams open for their entire connection lifetime.
class http3_local_critical_streams final {
public:
    [[nodiscard]] static std::variant<http3_local_critical_streams, http3_local_critical_streams_error>
    create(const http3_settings& settings = {}) noexcept;

    [[nodiscard]] std::span<const char> control_prefix() const noexcept {
        return std::span<const char>(control_).first(control_size_);
    }
    [[nodiscard]] std::span<const char> qpack_encoder_prefix() const noexcept {
        return std::span<const char>(qpack_encoder_).first(qpack_encoder_size_);
    }
    [[nodiscard]] std::span<const char> qpack_decoder_prefix() const noexcept {
        return std::span<const char>(qpack_decoder_).first(qpack_decoder_size_);
    }

private:
    static constexpr std::size_t control_prefix_capacity = 1 + 2 * 8 + 5 * 2 * 8;

    std::array<char, control_prefix_capacity> control_{};
    std::array<char, 1> qpack_encoder_{};
    std::array<char, 1> qpack_decoder_{};
    std::size_t control_size_{0};
    std::size_t qpack_encoder_size_{0};
    std::size_t qpack_decoder_size_{0};
};

}  // namespace ruvia
