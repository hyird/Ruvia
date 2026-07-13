#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

enum class HttpTransferCoding : std::uint8_t {
    kGzip,
    kDeflate
};

// One supported compression transfer-coding can be represented before final
// "chunked" framing, or as the sole coding of a close-delimited response. More
// stacked codings are rejected so the incremental decoder remains a fixed-size
// state machine without staging a second decoded body.
inline constexpr std::size_t kMaxTransferCodings = 1;

struct HttpTransferCodings {
    std::array<HttpTransferCoding, kMaxTransferCodings> values{};
    std::size_t count{0};

    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }
};

}  // namespace ruvia::detail
