#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/protocol_byte_limit.h"

namespace ruvia {

enum class websocket_inflate_result : std::uint8_t {
    ok,
    error,
    too_large,
};

// RFC 7692 permessage-deflate codecs for directions negotiated without context
// takeover. Such a direction starts every message from an empty dictionary,
// so one owner (for example a worker) can serve any number of connections
// with one codec set instead of a dictionary pair per connection.
//
// Each call handles one whole message and resets the codec it used before
// returning, on success, failure and exception alike: no message, and no
// failed message, ever influences another. Raw deflaters are created on first
// use for each (compression level, window bits) pair; one raw inflater with a
// 15-bit window serves every receive window. All codec memory comes from the
// resource, which must outlive this object. Not thread-safe; the address is
// stable for the borrowers.
class websocket_deflate_codecs final {
public:
    explicit websocket_deflate_codecs(std::pmr::memory_resource* resource) noexcept;
    ~websocket_deflate_codecs();
    websocket_deflate_codecs(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs& operator=(const websocket_deflate_codecs&) = delete;
    websocket_deflate_codecs(websocket_deflate_codecs&&) = delete;
    websocket_deflate_codecs& operator=(websocket_deflate_codecs&&) = delete;

    // Appends the compressed message payload to `out` without the trailing
    // 0x00 0x00 0xFF 0xFF of the final flush (RFC 7692 section 7.2.1). The
    // level is 0 through 9 and the negotiated window 8 through 15; anything
    // else throws std::invalid_argument. false reports a codec failure;
    // allocation exceptions propagate with their original type.
    [[nodiscard]] bool deflate_message(
        std::string_view payload, std::pmr::string& out, int compression_level, int window_bits);

    // Appends the decompressed message to `out`, bounded by `message_limit`
    // across everything this call appends (RFC 7692 section 7.2.2).
    [[nodiscard]] websocket_inflate_result inflate_message(
        std::string_view payload, std::pmr::string& out, protocol_byte_limit message_limit);

private:
    struct deflater;
    struct inflater;

    static constexpr int min_window_bits = 8;
    static constexpr int max_window_bits = 15;
    static constexpr int compression_levels = 10;

    std::pmr::memory_resource* resource_;
    std::array<deflater*, static_cast<std::size_t>((max_window_bits - min_window_bits + 1) * compression_levels)>
        deflaters_{};
    inflater* inflater_{nullptr};
};

}  // namespace ruvia
