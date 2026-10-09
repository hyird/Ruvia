#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_limits.h"

#include "http2/http2_stream_state.h"
#include "util/pmr_string.h"

namespace ruvia::detail {

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. An HPACK Huffman code can consume up to 30 bits per
// decoded octet, so a valid block can be larger than max_http_header_bytes. Four
// encoded bytes per decoded-budget byte cover that expansion plus the bounded
// representation overhead while retaining a hard memory limit for incomplete
// CONTINUATION sequences.
inline constexpr std::size_t max_http2_encoded_header_block_bytes = 4 * max_http_header_bytes;

inline void http2_reset_header_block(http2_stream_state& stream) {
    clear_pmr_string_retaining_small(stream.remote_header_block());
}

[[nodiscard]] inline bool http2_append_header_block(
    http2_stream_state& stream, std::string_view fragment) {
    auto& header_block = stream.remote_header_block();
    const auto current = header_block.size();
    if (current > max_http2_encoded_header_block_bytes ||
        fragment.size() > max_http2_encoded_header_block_bytes - current) {
        return false;
    }
    header_block.append(fragment.data(), fragment.size());
    return true;
}

[[nodiscard]] inline bool http2_start_header_block(
    http2_stream_state& stream, std::string_view fragment) {
    http2_reset_header_block(stream);
    return http2_append_header_block(stream, fragment);
}

}  // namespace ruvia::detail
