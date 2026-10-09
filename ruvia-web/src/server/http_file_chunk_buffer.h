#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

#include "ruvia/core/pmr_string.h"

namespace ruvia::detail {

inline constexpr std::size_t file_chunk_bytes = 64 * 1024;

inline void ensure_file_chunk_buffer(std::pmr::string& chunk) {
    if (chunk.size() < file_chunk_bytes) {
        ::ruvia::resize_pmr_string_for_overwrite(chunk, file_chunk_bytes);
    }
}

}  // namespace ruvia::detail
