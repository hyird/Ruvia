#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ruvia {

inline constexpr std::string_view http1_chunk_data_terminator = "\r\n";
inline constexpr std::string_view http1_last_chunk_prefix = "0\r\n";
inline constexpr std::string_view http1_trailer_section_terminator = "\r\n";

// The HTTP/1 chunk-size line, including its terminating CRLF. The caller owns
// framing decisions; this value only formats one chunk's wire prefix.
class http1_chunk_header final {
public:
    explicit http1_chunk_header(std::size_t chunk_size) noexcept {
        static constexpr char digits[] = "0123456789abcdef";
        auto* cursor_value = storage_.data() + storage_.size();
        *--cursor_value = '\n';
        *--cursor_value = '\r';
        do {
            *--cursor_value = digits[chunk_size & 0x0fU];
            chunk_size >>= 4U;
        } while (chunk_size != 0);
        offset_ = static_cast<std::size_t>(cursor_value - storage_.data());
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
