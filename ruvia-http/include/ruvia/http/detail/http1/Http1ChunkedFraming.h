#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia::detail {

inline constexpr std::string_view kHttp1ChunkDataTerminator = "\r\n";
inline constexpr std::string_view kHttp1LastChunkPrefix = "0\r\n";
inline constexpr std::string_view kHttp1TrailerSectionTerminator = "\r\n";

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

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(storage_.data() + offset_, storage_.size() - offset_);
    }

private:
    std::array<char, sizeof(std::size_t) * 2 + 2> storage_{};
    std::size_t offset_{storage_.size()};
};

inline void appendHttp1TrailerSection(
    std::pmr::string& output,
    const HttpResponseTrailerSection& section) {
    for (const auto& trailer : section.fields()) {
        output.append(trailer.name().data(), trailer.name().size());
        output.append(": ");
        output.append(trailer.value().data(), trailer.value().size());
        output.append(kHttp1ChunkDataTerminator);
    }
}

}  // namespace ruvia::detail
