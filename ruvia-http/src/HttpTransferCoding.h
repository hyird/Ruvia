#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

enum class HttpTransferCoding : std::uint8_t {
    kGzip,
    kDeflate
};

// One request transfer-coding is allowed before final "chunked" framing. More
// stacked codings are rejected so streaming stays bounded without staging a
// full decoded body.
inline constexpr std::size_t kMaxTransferCodings = 1;

struct HttpTransferCodings {
    std::array<HttpTransferCoding, kMaxTransferCodings> values{};
    std::size_t count{0};

    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }
};

}  // namespace ruvia::detail
