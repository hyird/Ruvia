#pragma once

#include <cstdint>

namespace ruvia::detail {

// The outbound request-content contract is chosen before the initial HEADERS are
// serialized. Keeping it as one value prevents Content-Length and END_STREAM from
// becoming independent sources of truth.
enum class Http2RequestContentMode : std::uint8_t {
    kNone,
    kKnownLength,
    kStreaming
};

class Http2RequestContent final {
public:
    [[nodiscard]] static constexpr Http2RequestContent none() noexcept {
        return Http2RequestContent(Http2RequestContentMode::kNone, 0);
    }

    [[nodiscard]] static constexpr Http2RequestContent knownLength(
        std::uint64_t length) noexcept {
        return Http2RequestContent(Http2RequestContentMode::kKnownLength, length);
    }

    [[nodiscard]] static constexpr Http2RequestContent streaming() noexcept {
        return Http2RequestContent(Http2RequestContentMode::kStreaming, 0);
    }

    [[nodiscard]] constexpr Http2RequestContentMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    constexpr Http2RequestContent(
        Http2RequestContentMode mode,
        std::uint64_t length) noexcept
        : length_(length), mode_(mode) {}

    std::uint64_t length_{0};
    Http2RequestContentMode mode_{Http2RequestContentMode::kNone};
};

}  // namespace ruvia::detail
