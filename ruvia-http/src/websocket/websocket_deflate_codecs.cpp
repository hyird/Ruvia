#include "ruvia/http/websocket_deflate_codecs.h"

#include <stdexcept>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_zlib_stream.h"
#include "coding/pmr_codec_slot.h"
#include "websocket/websocket_deflate_message.h"

namespace ruvia {

websocket_deflate_codecs::websocket_deflate_codecs(std::pmr::memory_resource* resource) noexcept
    : resource_(detail::http_pmr_resource_or_default(resource)) {}

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, codec, resource_);
    }
    detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, inflater_, resource_);
}

bool websocket_deflate_codecs::deflate_message(
    std::string_view payload, std::pmr::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>(
        (window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = detail::acquire_pmr_codec(slot, resource_, resource_, compression_level,
        detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level,
        detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const detail::pmr_codec_reset_guard reset_guard(slot, resource_);
    return detail::websocket_deflate_message(*codec, payload, out, resource_);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(
    std::string_view payload, std::pmr::string& out, protocol_byte_limit message_limit) {
    auto* codec = detail::acquire_pmr_codec(inflater_, resource_, resource_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const detail::pmr_codec_reset_guard reset_guard(inflater_, resource_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit, resource_);
}

}  // namespace ruvia
