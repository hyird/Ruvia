#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/http/detail/util/PmrString.h"

namespace ruvia::detail {

inline constexpr std::size_t kFileChunkBytes = 64 * 1024;

inline void ensureFileChunkBuffer(std::pmr::string& chunk) {
    if (chunk.size() < kFileChunkBytes) {
        resizePmrStringForOverwrite(chunk, kFileChunkBytes);
    }
}

}  // namespace ruvia::detail
