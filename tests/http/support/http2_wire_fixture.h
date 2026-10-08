#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Framing.h"

namespace http2_connection_test {

// Encode a minimal valid request header block (HPACK literals) into `block`.
inline void encodeRequest(std::pmr::string& block, std::string_view method,
    std::string_view scheme = "https", std::string_view path = "/",
    std::optional<std::string_view> authority = "example.com") {
    ruvia::HpackEncoder::encodeHeader(block, ":method", method);
    ruvia::HpackEncoder::encodeHeader(block, ":scheme", scheme);
    ruvia::HpackEncoder::encodeHeader(block, ":path", path);
    if (authority.has_value()) {
        ruvia::HpackEncoder::encodeHeader(block, ":authority", *authority);
    }
}

inline void encodeGetRequest(std::pmr::string& block) {
    encodeRequest(block, "GET");
}

// Frame a HEADERS block on `streamId` with the given flags into a fed-ready buffer.
inline std::pmr::string headersFrame(std::pmr::memory_resource* resource, std::uint32_t streamId,
    std::uint8_t flags, std::string_view block) {
    std::pmr::string frame(resource);
    char hdr[9];
    (void)ruvia::encodeHttp2FrameHeader(
        hdr, static_cast<std::uint32_t>(block.size()), ruvia::Http2FrameType::kHeaders, flags, streamId);
    frame.append(hdr, 9);
    frame.append(block.data(), block.size());
    return frame;
}

inline std::pmr::string dataFrame(std::pmr::memory_resource* resource, std::uint32_t streamId,
    std::uint8_t flags, std::string_view body) {
    std::pmr::string frame(resource);
    char hdr[9];
    (void)ruvia::encodeHttp2FrameHeader(
        hdr, static_cast<std::uint32_t>(body.size()), ruvia::Http2FrameType::kData, flags, streamId);
    frame.append(hdr, 9);
    frame.append(body.data(), body.size());
    return frame;
}

}  // namespace http2_connection_test
