#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/Http3Settings.h"

namespace ruvia {

enum class Http3LocalCriticalStreamsError : unsigned char {
    kSettingsEncodingError,
};

// Fixed, independently sendable prefixes for the three locally initiated
// unidirectional critical streams. These prefixes never include FIN: callers
// must keep all three streams open for their entire connection lifetime.
class Http3LocalCriticalStreams final {
public:
    [[nodiscard]] static std::variant<Http3LocalCriticalStreams, Http3LocalCriticalStreamsError>
    create(const Http3Settings& settings = {}) noexcept;

    [[nodiscard]] std::span<const char> controlPrefix() const noexcept {
        return std::span<const char>(control_).first(controlSize_);
    }
    [[nodiscard]] std::span<const char> qpackEncoderPrefix() const noexcept {
        return std::span<const char>(qpackEncoder_).first(qpackEncoderSize_);
    }
    [[nodiscard]] std::span<const char> qpackDecoderPrefix() const noexcept {
        return std::span<const char>(qpackDecoder_).first(qpackDecoderSize_);
    }

private:
    static constexpr std::size_t kControlPrefixCapacity = 1 + 2 * 8 + 5 * 2 * 8;

    std::array<char, kControlPrefixCapacity> control_{};
    std::array<char, 1> qpackEncoder_{};
    std::array<char, 1> qpackDecoder_{};
    std::size_t controlSize_{0};
    std::size_t qpackEncoderSize_{0};
    std::size_t qpackDecoderSize_{0};
};

}  // namespace ruvia
