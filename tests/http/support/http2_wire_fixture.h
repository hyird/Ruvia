#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"

namespace http2_connection_test {

// Encode a minimal valid request header block (HPACK literals) into `block`.
inline void encode_request(std::pmr::string& block, std::string_view method,
    std::string_view scheme = "https", std::string_view path = "/",
    std::optional<std::string_view> authority = "example.com") {
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", scheme);
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    if (authority.has_value()) {
        ruvia::hpack_encoder::encode_header(block, ":authority", *authority);
    }
}

inline void encode_get_request(std::pmr::string& block) {
    encode_request(block, "GET");
}

// Frame a HEADERS block on `stream_id` with the given flags into a fed-ready buffer.
inline std::pmr::string headers_frame(std::pmr::memory_resource* resource, std::uint32_t stream_id,
    std::uint8_t flags, std::string_view block) {
    std::pmr::string frame(resource);
    char hdr[9];
    (void)ruvia::encode_http2_frame_header(
        hdr, static_cast<std::uint32_t>(block.size()), ruvia::http2_frame_type::headers, flags, stream_id);
    frame.append(hdr, 9);
    frame.append(block.data(), block.size());
    return frame;
}

inline std::pmr::string data_frame(std::pmr::memory_resource* resource, std::uint32_t stream_id,
    std::uint8_t flags, std::string_view body) {
    std::pmr::string frame(resource);
    char hdr[9];
    (void)ruvia::encode_http2_frame_header(
        hdr, static_cast<std::uint32_t>(body.size()), ruvia::http2_frame_type::data, flags, stream_id);
    frame.append(hdr, 9);
    frame.append(body.data(), body.size());
    return frame;
}

}  // namespace http2_connection_test
