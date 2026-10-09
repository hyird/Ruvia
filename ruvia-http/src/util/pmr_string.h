#pragma once

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline constexpr std::size_t retained_pmr_string_bytes = 4096;

// Drop the consumed [0, offset) prefix of a buffer whose size() is the live
// content length and whose `offset` tracks how much has already been consumed.
// A fully consumed buffer is cleared outright; otherwise the unconsumed tail is
// moved to the front only once the consumed prefix grows past `compact_threshold`,
// so steady streaming reads amortize the memmove instead of shifting on every
// consume. `offset` is reset to 0 whenever the buffer is rewritten.
//
// Shared by every offset-tracked read buffer (HTTP/2 frame input, multipart
// parsing) so the compaction policy lives in exactly one place.
inline void compact_consumed_prefix(
    std::pmr::string& buffer, std::size_t& offset, std::size_t compact_threshold) {
    if (offset >= buffer.size()) {
        buffer.clear();
        offset = 0;
        return;
    }
    if (offset < compact_threshold) {
        return;
    }
    const auto remaining = buffer.size() - offset;
    std::memmove(buffer.data(), buffer.data() + offset, remaining);
    buffer.resize(remaining);
    offset = 0;
}

inline void resize_pmr_string_for_overwrite(std::pmr::string& target, std::size_t size) {
    target.resize(size);
}

inline void clear_pmr_string_retaining_small(
    std::pmr::string& target, std::size_t retained_bytes = retained_pmr_string_bytes) {
    target.clear();
    if (target.capacity() <= retained_bytes) {
        return;
    }

    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia::detail
