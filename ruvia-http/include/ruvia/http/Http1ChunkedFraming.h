#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ruvia {

inline constexpr std::string_view kHttp1ChunkDataTerminator = "\r\n";
inline constexpr std::string_view kHttp1LastChunkPrefix = "0\r\n";
inline constexpr std::string_view kHttp1TrailerSectionTerminator = "\r\n";

// The HTTP/1 chunk-size line, including its terminating CRLF. The caller owns
// framing decisions; this value only formats one chunk's wire prefix.
class Http1ChunkHeader final {
public:
    explicit Http1ChunkHeader(std::size_t chunkSize) noexcept {
        static constexpr char digits[] = "0123456789abcdef";
        auto* cursor = storage_.data() + storage_.size();
        *--cursor = '\n';
        *--cursor = '\r';
        do {
            *--cursor = digits[chunkSize & 0x0fU];
            chunkSize >>= 4U;
        } while (chunkSize != 0);
        offset_ = static_cast<std::size_t>(cursor - storage_.data());
    }

    [[nodiscard]] std::string_view view() const& noexcept {
        return std::string_view(storage_.data() + offset_, storage_.size() - offset_);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

private:
    std::array<char, sizeof(std::size_t) * 2 + 2> storage_{};
    std::size_t offset_{storage_.size()};
};

}  // namespace ruvia
