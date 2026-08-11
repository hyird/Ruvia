#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ruvia {

enum class HttpTransferCoding : std::uint8_t { kGzip, kDeflate };

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

static_assert(std::is_trivially_copyable_v<HttpTransferCodings>);
static_assert(sizeof(HttpTransferCodings) <= sizeof(std::size_t) * 2);

}  // namespace ruvia
