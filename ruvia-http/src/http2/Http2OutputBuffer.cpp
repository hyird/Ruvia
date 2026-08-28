#include "ruvia/http/detail/http2/frame/Http2OutputBuffer.h"

#include <array>
#include <utility>

namespace ruvia::detail {

void Http2OutputBuffer::take(std::pmr::string& into) {
    if (consumed_ == 0 && into.get_allocator() == bytes_.get_allocator()) {
        into.swap(bytes_);
        bytes_.clear();
        return;
    }
    into.assign(bytes_.data() + consumed_, bytes_.size() - consumed_);
    bytes_.clear();
    consumed_ = 0;
}

void Http2OutputBuffer::appendGoawayFrame(std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug) {
    std::array<char, 8> payload;
    auto* const end = http2WriteGoawayPayload(payload.data(), lastStreamId, error);
    appendFrame(Http2FrameType::kGoaway, 0, 0, std::string_view(payload.data(), static_cast<std::size_t>(end - payload.data())), debug);
}

void Http2OutputBuffer::appendRstStream(std::uint32_t streamId, Http2ErrorCode error) {
    std::array<char, 4> payload;
    auto* const end = http2Write32(payload.data(), static_cast<std::uint32_t>(error));
    appendFrame(Http2FrameType::kRstStream, 0, streamId, std::string_view(payload.data(), static_cast<std::size_t>(end - payload.data())));
}

}  // namespace ruvia::detail
