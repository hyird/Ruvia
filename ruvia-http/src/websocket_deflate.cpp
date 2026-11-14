#include "websocket_deflate.h"

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "http_compression_codec.h"
#include "http_field_text.h"
#include "http_protocol.h"
#include "websocket_connection.h"
#include "websocket_deflate_codecs.h"

namespace ruvia::detail {

// The one RFC 7692 whole-message deflate and inflate loop, shared by
// per-connection codecs and the shared no-context-takeover codec set. Context
// takeover is the owner's decision: these functions neither reset nor keep the
// state between messages.

// zlib cannot emit an 8-bit raw window, so an 8-bit sender window uses zlib's
// 9-bit window restricted to Huffman coding, which never refers back at all.
[[nodiscard]] constexpr int websocket_deflate_zlib_window_bits(int window_bits) noexcept {
    return -std::max(window_bits, 9);
}

[[nodiscard]] constexpr int websocket_deflate_zlib_strategy(int window_bits) noexcept {
    return window_bits == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
}

// Compresses a message, or one fragment of it, up to a sync flush and appends
// the raw-DEFLATE blocks to `out`. At the message end the trailing
// 0x00 0x00 0xFF 0xFF flush marker is removed (RFC 7692 §7.2.1); earlier
// fragments keep it, which the receiver inflates as an empty stored block.
[[nodiscard]] inline bool websocket_deflate_message(zlib_deflate_stream& deflater, std::string_view input, std::string& out, bool message_end = true) {
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    auto& stream = deflater.native();
    const zlib_borrow_scope borrow(stream);
    const auto start = out.size();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[4096];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = deflate(&stream, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR) {
            deflater.rethrow_allocation_failure();
            return false;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        out.append(buffer, produced);
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == input.size()) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input && !(stream.avail_in == 0 && supplied < input.size())) {
            return false;
        }
    }
    if (!message_end) {
        return true;
    }
    if (out.size() - start >= 4) {
        out.resize(out.size() - 4);
    }
    if (out.size() == start) {
        out.push_back('\0');
    }
    return true;
}

// One inflate pass over `data`; the message limit covers everything appended
// to `out` since `start`. A null `storage` grows `out` uncharged.
[[nodiscard]] inline websocket_inflate_result websocket_inflate_chunk(zlib_inflate_stream& inflater, const char* data, std::size_t size, std::string& out, std::size_t start, protocol_byte_limit message_limit, websocket_receive_storage* storage) {
    auto& stream = inflater.native();
    const zlib_borrow_scope borrow(stream);
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, std::string_view(data, size), supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (message_limit.addition_exceeds(out.size() - start, produced)) {
            return websocket_inflate_result::too_large;
        }
        if (storage != nullptr && !storage->reserve(websocket_receive_storage::slot::inflate, out, out.size() + produced)) {
            return websocket_inflate_result::too_large;
        }
        out.append(buffer, produced);
        // RFC 7692 permits BFINAL=1 followed by another DEFLATE stream.
        // Preserve the LZ77 window across streams, including takeover
        // history, while discarding the terminated stream's padding bits.
        if (status == Z_STREAM_END && inflateResetKeep(&stream) != Z_OK) {
            return websocket_inflate_result::error;
        }
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == size) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input && !(stream.avail_in == 0 && supplied < size)) {
            return websocket_inflate_result::error;
        }
    }
    return websocket_inflate_result::ok;
}

// Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
// the sender stripped, then raw-inflates into `out`, bounded by one explicit
// message limit across the appended bytes to defuse decompression bombs
// (RFC 7692 §7.2.2).
[[nodiscard]] inline websocket_inflate_result websocket_inflate_message(zlib_inflate_stream& inflater, std::string_view input, std::string& out, protocol_byte_limit message_limit, websocket_receive_storage* storage) {
    static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    const auto start = out.size();
    if (const auto result_value = websocket_inflate_chunk(inflater, input.data(), input.size(), out, start, message_limit, storage); result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    if (const auto result_value = websocket_inflate_chunk(inflater, reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, start, message_limit, storage); result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    // zlib reports bit 128 only at a complete DEFLATE block boundary.
    // Consuming every input byte alone does not detect a truncated block.
    return (inflater.native().data_type & 128) != 0 ? websocket_inflate_result::ok : websocket_inflate_result::error;
}

websocket_deflate::websocket_deflate(websocket_deflate_codecs* shared, websocket_deflater_config deflater, bool send_takeover, bool receive_takeover, int send_window, int receive_window) noexcept :
    shared_(shared),
    deflater_config_(deflater),
    send_window_(send_window),
    receive_window_(receive_window),
    send_takeover_(send_takeover),
    receive_takeover_(receive_takeover) {}

websocket_deflate::~websocket_deflate() = default;

bool websocket_deflate::compress(std::string_view input, std::string& out) {
    if (!send_takeover_ && shared_ != nullptr) {
        // Reset after every call, including failures and exceptions.
        return shared_->deflate_message(input, out, deflater_config_.compression_level_, send_window_);
    }
    if (deflater_ == nullptr) {
        deflater_ = std::make_unique<zlib_deflate_stream>(deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_), deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
        if (!deflater_->initialized()) {
            deflater_.reset();
            return false;
        }
    } else if (!send_takeover_ && !deflater_->reset()) {
        return false;
    }
    return websocket_deflate_message(*deflater_, input, out);
}

bool websocket_deflate::compress_fragment(std::string_view input, bool first, bool final, std::string& out) {
    if (deflater_ == nullptr) {
        deflater_ = std::make_unique<zlib_deflate_stream>(deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_), deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
        if (!deflater_->initialized()) {
            deflater_.reset();
            return false;
        }
    } else if (first && !send_takeover_ && !deflater_->reset()) {
        return false;
    }
    return websocket_deflate_message(*deflater_, input, out, final);
}

websocket_inflate_result websocket_deflate::decompress(std::string_view input, std::string& out, protocol_byte_limit message_limit, websocket_receive_storage& storage) {
    if (!receive_takeover_ && shared_ != nullptr) {
        // The shared inflater has a 15-bit window, which also inflates
        // every smaller negotiated window.
        auto* codec = acquire_codec(shared_->inflater_, -websocket_deflate_codecs::max_window_bits);
        if (codec == nullptr) {
            return websocket_inflate_result::error;
        }
        const codec_reset_guard reset_guard(shared_->inflater_);
        return websocket_inflate_message(*codec, input, out, message_limit, &storage);
    }
    if (inflater_ == nullptr) {
        inflater_ = std::make_unique<zlib_inflate_stream>(-receive_window_);
        if (!inflater_->initialized()) {
            inflater_.reset();
            return websocket_inflate_result::error;
        }
    } else if (!receive_takeover_ && !inflater_->reset()) {
        return websocket_inflate_result::error;
    }
    return websocket_inflate_message(*inflater_, input, out, message_limit, &storage);
}

void websocket_deflate::discard_compression() {
    if (deflater_ != nullptr && !deflater_->reset()) {
        throw std::runtime_error("failed to reset discarded WebSocket compression");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        delete codec;
    }
    delete inflater_;
}

bool websocket_deflate_codecs::deflate_message(std::string_view payload, std::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>((window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = detail::acquire_codec(slot, compression_level, detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level, detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const detail::codec_reset_guard reset_guard(slot);
    return detail::websocket_deflate_message(*codec, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(std::string_view payload, std::string& out, protocol_byte_limit message_limit) {
    auto* codec = detail::acquire_codec(inflater_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const detail::codec_reset_guard reset_guard(inflater_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit, nullptr);
}

}  // namespace ruvia
