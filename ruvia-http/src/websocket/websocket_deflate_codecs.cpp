#include "ruvia/http/websocket_deflate_codecs.h"

#include <stdexcept>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_zlib_stream.h"
#include "websocket/websocket_deflate_message.h"

namespace ruvia {

// Pinned native streams: zlib keeps back-pointers into them, so each lives in
// one stable allocation from the codec set's resource.
struct websocket_deflate_codecs::deflater final {
    deflater(std::pmr::memory_resource* resource, int compression_level, int window_bits)
        : stream_(resource, compression_level, detail::websocket_deflate_zlib_window_bits(window_bits),
              detail::zlib_default_mem_level, detail::websocket_deflate_zlib_strategy(window_bits)) {}

    detail::zlib_deflate_stream stream_;
};

struct websocket_deflate_codecs::inflater final {
    explicit inflater(std::pmr::memory_resource* resource)
        : stream_(resource, -max_window_bits) {}

    detail::zlib_inflate_stream stream_;
};

namespace {

// Creates the slot's stream on first use. A stream zlib refused to initialize
// is not kept, so the next message retries.
template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_codec(
    codec_type*& slot, std::pmr::memory_resource* resource, args_type... args) {
    if (slot == nullptr) {
        auto* created = detail::construct_http_pmr_object<codec_type>(
            detail::http_resolved_pmr_resource_tag{}, resource, resource, args...);
        if (!created->stream_.initialized()) {
            detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, created, resource);
            return nullptr;
        }
        slot = created;
    }
    return slot;
}

// Every exit returns the slot's stream to its initial state. A stream that
// cannot reset is dropped and recreated by the next message.
template <typename codec_type>
class reset_after_message final {
public:
    reset_after_message(codec_type*& slot, std::pmr::memory_resource* resource) noexcept
        : slot_(slot),
          resource_(resource) {}
    reset_after_message(const reset_after_message&) = delete;
    reset_after_message& operator=(const reset_after_message&) = delete;

    ~reset_after_message() {
        if (!slot_->stream_.reset()) {
            detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, slot_, resource_);
            slot_ = nullptr;
        }
    }

private:
    codec_type*& slot_;
    std::pmr::memory_resource* resource_;
};

}  // namespace

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
    auto* codec = acquire_codec(slot, resource_, compression_level, window_bits);
    if (codec == nullptr) {
        return false;
    }
    const reset_after_message reset_guard(slot, resource_);
    return detail::websocket_deflate_message(codec->stream_, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(
    std::string_view payload, std::pmr::string& out, protocol_byte_limit message_limit) {
    auto* codec = acquire_codec(inflater_, resource_);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const reset_after_message reset_guard(inflater_, resource_);
    return detail::websocket_inflate_message(codec->stream_, payload, out, message_limit);
}

}  // namespace ruvia
