#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "FetchStreamSource.h"
#include "HttpClientContentEncoding.h"
#include "HttpStreamingDecoder.h"

namespace ruvia::detail {

// FetchStreamSource decorator that decodes a Content-Encoding on the fly. It owns an inner source
// (h1 or h2) and pulls its raw transfer bytes through a StreamingContentDecoder, so readChunk()
// yields decoded slices. Per-call output is bounded (kMaxStreamDecodeChunk) so a small compressed
// chunk cannot expand without limit under readChunk backpressure. status()/headers()/close()
// delegate to the inner source (the Content-Encoding header is left in place, matching the
// buffered fetch, which also decodes the body but does not strip the header).
class DecodingStreamSource final : public FetchStreamSource {
public:
    // Output cap per readChunk(): a small compressed chunk can decode to at most this before the
    // caller must consume it and pull again (real backpressure over a decompression bomb).
    static constexpr std::size_t kMaxStreamDecodeChunk = 256 * 1024;

    DecodingStreamSource(
        std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter> inner,
        HttpContentCoding coding,
        std::pmr::memory_resource* resource) noexcept
        : inner_(std::move(inner)),
          decoder_(coding),
          pendingInput_(resource),
          resource_(resource) {}

    [[nodiscard]] std::uint16_t status() const noexcept override { return inner_->status(); }
    [[nodiscard]] const std::pmr::vector<FetchResponseHeader>& headers() const noexcept override {
        return inner_->headers();
    }

    [[nodiscard]] Task<std::pmr::string> readChunk() override {
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
                    // EOF, no output, decoder can't advance on the remaining bytes: truncated.
                    throw std::runtime_error("http client: truncated compressed response");
                }
                // Buffered input exhausted (or the decoder needs more): pull another raw chunk.
            }

            std::pmr::string chunk = co_await inner_->readChunk();
            if (chunk.empty()) {
                innerEof_ = true;
            } else if (pendingInput_.empty()) {
                pendingInput_ = std::move(chunk);
            } else {
                pendingInput_.append(chunk.data(), chunk.size());
            }
        }
    }

    void close() noexcept override { inner_->close(); }
    void destroy() noexcept override { destroyPmrObject(this, resource_); }

private:
    std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter> inner_;
    StreamingContentDecoder decoder_;
    std::pmr::string pendingInput_;
    std::pmr::memory_resource* resource_;
    bool innerEof_{false};
};

// Wrap `inner` in a decoding decorator when the caller asked for it and the response carries a
// single decodable Content-Encoding; otherwise return `inner` unchanged (raw transfer bytes).
template <typename Headers>
[[nodiscard]] inline std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter>
maybeWrapDecodingStreamSource(
    std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter> inner,
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
    return std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter>(wrapped);
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
