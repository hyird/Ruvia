#include "ruvia/http/websocket_deflate_codecs.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "coding/http_zlib_stream.h"
#include "websocket/websocket_deflate_message.h"

namespace ruvia {
namespace {

template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_codec(codec_type*& slot, args_type&&... args) {
    if (slot == nullptr) {
        auto created = std::make_unique<codec_type>(std::forward<args_type>(args)...);
        if (!created->initialized()) {
            return nullptr;
        }
        slot = created.release();
    }
    return slot;
}

template <typename codec_type>
class codec_reset_guard final {
public:
    explicit codec_reset_guard(codec_type*& slot) noexcept
        : slot_(slot),
          uncaught_exceptions_(std::uncaught_exceptions()) {}
    codec_reset_guard(const codec_reset_guard&) = delete;
    codec_reset_guard& operator=(const codec_reset_guard&) = delete;

    ~codec_reset_guard() {
        if (std::uncaught_exceptions() != uncaught_exceptions_ || !slot_->reset()) {
            delete std::exchange(slot_, nullptr);
        }
    }

private:
    codec_type*& slot_;
    int uncaught_exceptions_;
};

}  // namespace

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        delete codec;
    }
    delete inflater_;
}

bool websocket_deflate_codecs::deflate_message(
    std::string_view payload, std::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>(
        (window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = acquire_codec(slot, compression_level,
        detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level,
        detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const codec_reset_guard reset_guard(slot);
    return detail::websocket_deflate_message(*codec, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(
    std::string_view payload, std::string& out, protocol_byte_limit message_limit) {
    auto* codec = acquire_codec(inflater_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const codec_reset_guard reset_guard(inflater_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit);
}

}  // namespace ruvia
