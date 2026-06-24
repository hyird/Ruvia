#pragma once

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline constexpr std::size_t kRetainedPmrStringBytes = 4096;

// Drop the consumed [0, offset) prefix of a buffer whose size() is the live
// content length and whose `offset` tracks how much has already been consumed.
// A fully consumed buffer is cleared outright; otherwise the unconsumed tail is
// moved to the front only once the consumed prefix grows past `compactThreshold`,
// so steady streaming reads amortize the memmove instead of shifting on every
// consume. `offset` is reset to 0 whenever the buffer is rewritten.
//
// Shared by every offset-tracked read buffer (HTTP/2 frame input, multipart
// parsing) so the compaction policy lives in exactly one place.
inline void compactConsumedPrefix(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t compactThreshold) {
    if (offset >= buffer.size()) {
        buffer.clear();
        offset = 0;
        return;
    }
    if (offset < compactThreshold) {
        return;
    }
    const auto remaining = buffer.size() - offset;
    std::memmove(buffer.data(), buffer.data() + offset, remaining);
    buffer.resize(remaining);
    offset = 0;
}

inline void resizePmrStringForOverwrite(std::pmr::string& target, std::size_t size) {
    target.resize_and_overwrite(size, [](char*, std::size_t count) noexcept {
        return count;
    });
}

inline void clearPmrStringRetainingSmall(
    std::pmr::string& target,
    std::size_t retainedBytes = kRetainedPmrStringBytes) {
    target.clear();
    if (target.capacity() <= retainedBytes) {
        return;
    }

    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia::detail
