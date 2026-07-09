#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpBodyStream.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "client/HttpClientContentEncoding.h"
#include "client/HttpStreamingDecoder.h"

namespace ruvia::detail {

// Decodes a Content-Encoding on the fly by wrapping an inner HttpBodyStream: it pulls the inner
// stream's raw transfer bytes through a StreamingContentDecoder so each nextChunk() yields decoded
// bytes. Per-call output is bounded (kMaxStreamDecodeChunk) so a small compressed chunk cannot
// expand without limit under readChunk backpressure. Exposed itself as an HttpBodyStream.
class DecodingStreamSource final {
public:
    // Output cap per chunk: a small compressed chunk decodes to at most this before the caller must
    // consume it and pull again (real backpressure over a decompression bomb).
    static constexpr std::size_t kMaxStreamDecodeChunk = 256 * 1024;

    DecodingStreamSource(
        HttpBodyStream inner,
        HttpContentCoding coding,
        std::pmr::memory_resource* resource) noexcept
        : inner_(std::move(inner)),
          decoder_(coding),
          pendingInput_(resource),
          currentChunk_(resource),
          resource_(resource) {}

    static Task<std::string_view> streamNextChunk(void* self) {
        auto* source = static_cast<DecodingStreamSource*>(self);
        source->currentChunk_ = co_await source->readChunk();
        co_return std::string_view(source->currentChunk_.data(), source->currentChunk_.size());
    }
    static void streamDestroy(void* self) noexcept {
        auto* source = static_cast<DecodingStreamSource*>(self);
        destroyPmrObject(source, source->resource_);
    }

private:
    Task<std::pmr::string> readChunk() {
        for (;;) {
            // Produce decoded output from whatever raw input is buffered (and, at EOF, flush).
            if (!pendingInput_.empty() || innerEof_) {
                std::pmr::string out(resource_);
                std::string_view in(pendingInput_);
                if (!decoder_.decode(in, out, kMaxStreamDecodeChunk)) {
                    throw std::runtime_error(
                        "http client: failed to decode streamed Content-Encoding");
                }
                pendingInput_.erase(0, pendingInput_.size() - in.size());  // drop consumed prefix
                if (!out.empty()) {
                    co_return out;
                }
                if (innerEof_ && pendingInput_.empty()) {
                    if (!decoder_.finished()) {
                        throw std::runtime_error("http client: truncated compressed response");
                    }
                    co_return std::pmr::string(resource_);  // clean end of stream
                }
                if (innerEof_) {
                    throw std::runtime_error("http client: truncated compressed response");
                }
            }

            const std::string_view chunk = co_await inner_.nextChunk();
            if (chunk.empty()) {
                innerEof_ = true;
            } else if (pendingInput_.empty()) {
                pendingInput_.assign(chunk.data(), chunk.size());
            } else {
                pendingInput_.append(chunk.data(), chunk.size());
            }
        }
    }

    HttpBodyStream inner_;
    StreamingContentDecoder decoder_;
    std::pmr::string pendingInput_;
    std::pmr::string currentChunk_;  // backing store for the view streamNextChunk hands out
    std::pmr::memory_resource* resource_;
    bool innerEof_{false};
};

// Wrap `inner` in a decoding decorator when the caller asked for it and the response carries a
// single decodable Content-Encoding; otherwise return `inner` unchanged (raw transfer bytes).
template <typename Headers>
[[nodiscard]] inline HttpBodyStream maybeWrapDecodingStreamSource(
    HttpBodyStream inner,
    const Headers& headers,
    bool decodeStream,
    std::pmr::memory_resource* resource) {
    if (!decodeStream) {
        return inner;
    }
    const auto coding = httpClientContentCodingOf(headers);
    if (coding == HttpContentCoding::kNone) {
        return inner;
    }
    auto* wrapped = constructPmrObject<DecodingStreamSource>(
        resource, std::move(inner), coding, resource);
    return HttpBodyStream(
        wrapped, &DecodingStreamSource::streamNextChunk, &DecodingStreamSource::streamDestroy);
}

}  // namespace ruvia::detail
