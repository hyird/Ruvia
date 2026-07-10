#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

constexpr std::size_t kFileChunkBytes = 64 * 1024;

inline void ensureFileChunkBuffer(std::pmr::string& chunk) {
    if (chunk.size() >= kFileChunkBytes) {
        return;
    }
    resizePmrStringForOverwrite(chunk, kFileChunkBytes);
}

}  // namespace ruvia::detail
