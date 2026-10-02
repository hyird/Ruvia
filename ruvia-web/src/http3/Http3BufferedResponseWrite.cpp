#include "ruvia/web/detail/http3/Http3BufferedResponseWrite.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

Http3BufferedResponseWrite::Http3BufferedResponseWrite(std::pmr::memory_resource* workerPool)
    : workerPool_(workerPool != nullptr ? workerPool : std::pmr::get_default_resource()),
      headers_(workerPool_) {}

Http3BufferedResponseWrite& Http3BufferedResponseWrite::requireNoOutstandingSegment(
    Http3BufferedResponseWrite& other) {
    if (other.offered_) {
        throw std::logic_error("cannot move an HTTP/3 response cursor with an outstanding span");
    }
    return other;
}

Http3BufferedResponseWrite::Http3BufferedResponseWrite(Http3BufferedResponseWrite&& other)
    : workerPool_(requireNoOutstandingSegment(other).workerPool_),
      headers_(std::move(other.headers_)),
      decodedFieldSectionSize_(std::exchange(other.decodedFieldSectionSize_, 0)),
      body_(other.body_),
      dataPlan_(std::move(other.dataPlan_)),
      chunk_(other.chunk_),
      segmentOffset_(other.segmentOffset_),
      bodyOffset_(other.bodyOffset_),
      state_(other.state_),
      chunkPending_(other.chunkPending_) {
    other.state_ = State::kFailed;
}

std::expected<Http3BufferedResponseWrite, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::create(const HttpResponse& response,
    const HttpBufferedResponseWritePlan& writePlan, std::pmr::memory_resource* workerPool) noexcept {
    if (!writePlan.matchesResponse(response)) {
        return std::unexpected(Error::kInvalidResponsePlan);
    }
    if (response.fileBody() && writePlan.sendBody() && writePlan.contentLength() != 0) {
        return std::unexpected(Error::kFileBodyUnsupported);
    }
    try {
        auto encoded = encodeHttp3ResponseHead(response, writePlan, {}, workerPool);
        if (!encoded) {
            return std::unexpected(Error::kResponseEncoding);
        }
        return create(response, writePlan, std::move(*encoded), workerPool);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::kOutOfMemory);
    } catch (...) {
        return std::unexpected(Error::kResponseEncoding);
    }
}

std::expected<Http3BufferedResponseWrite, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::create(const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan,
    Http3ResponseHead encodedHead, std::pmr::memory_resource* workerPool) noexcept {
    if (!writePlan.matchesResponse(response)) {
        return std::unexpected(Error::kInvalidResponsePlan);
    }
    if (response.fileBody() && writePlan.sendBody() && writePlan.contentLength() != 0) {
        return std::unexpected(Error::kFileBodyUnsupported);
    }
    try {
        Http3BufferedResponseWrite cursor(workerPool);
        const auto* encoded = &encodedHead;
        cursor.decodedFieldSectionSize_ = encoded->decodedFieldSectionSize();
        if (encoded->fieldSection.size() > std::numeric_limits<std::size_t>::max() -
                                               kHttp3FrameHeaderMaxBytes) {
            return std::unexpected(Error::kResponseEncoding);
        }
        cursor.headers_.resize(kHttp3FrameHeaderMaxBytes + encoded->fieldSection.size());
        const auto frameHeader = encodeHttp3FrameHeader(cursor.headers_,
            static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->fieldSection.size());
        if (!frameHeader) {
            return std::unexpected(Error::kResponseEncoding);
        }
        cursor.headers_.resize(*frameHeader + encoded->fieldSection.size());
        std::copy(encoded->fieldSection.begin(), encoded->fieldSection.end(),
            cursor.headers_.begin() + static_cast<std::ptrdiff_t>(*frameHeader));

        const auto bodyPlan = writePlan.bodyPlan();
        const bool sendBody = writePlan.sendBody();
        cursor.body_ = sendBody ? response.bodyBytes() : std::string_view{};
        if (sendBody && cursor.body_.size() != writePlan.contentLength()) {
            return std::unexpected(Error::kInvalidResponsePlan);
        }
        const std::optional<std::uint64_t> length = sendBody
                                                        ? std::optional<std::uint64_t>(writePlan.contentLength())
                                                        : std::nullopt;
        cursor.dataPlan_.emplace(bodyPlan, length);
        return cursor;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::kOutOfMemory);
    } catch (...) {
        return std::unexpected(Error::kResponseEncoding);
    }
}

std::expected<Http3BufferedResponseWrite::Segment, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::next() noexcept {
    if (state_ == State::kFinished || state_ == State::kFailed) {
        return std::unexpected(Error::kInvalidState);
    }
    if (state_ == State::kFin) {
        return Segment{};
    }
    const auto segment = activeSegment();
    offered_ = !segment.empty();
    return segment;
}

Http3BufferedResponseWrite::NextStep Http3BufferedResponseWrite::nextStep() const noexcept {
    switch (state_) {
        case State::kHeaders:
            return segmentOffset_ < headers_.size() ? NextStep::kBytes : NextStep::kFailed;
        case State::kDataHeader:
            return chunkPending_ && chunk_.emitsData &&
                           segmentOffset_ < chunk_.frameHeaderSize
                       ? NextStep::kBytes
                       : NextStep::kFailed;
        case State::kDataBody:
            return chunkPending_ && !chunk_.payload.empty() &&
                           segmentOffset_ < chunk_.payload.size()
                       ? NextStep::kBytes
                       : NextStep::kFailed;
        case State::kFin:
            return dataPlan_ && chunkPending_ ? NextStep::kFin : NextStep::kFailed;
        case State::kFinished:
            return NextStep::kComplete;
        case State::kFailed:
            return NextStep::kFailed;
    }
    return NextStep::kFailed;
}

Http3BufferedResponseWrite::Segment Http3BufferedResponseWrite::activeSegment() const noexcept {
    switch (state_) {
        case State::kHeaders:
            return Segment(headers_).subspan(segmentOffset_);
        case State::kDataHeader:
            return Segment(chunk_.frameHeader.data(), chunk_.frameHeaderSize).subspan(segmentOffset_);
        case State::kDataBody:
            return chunk_.payload.subspan(segmentOffset_);
        case State::kFin:
        case State::kFinished:
        case State::kFailed:
            return {};
    }
    return {};
}

std::expected<void, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::prepareData() noexcept {
    if (!dataPlan_ || state_ != State::kDataHeader || chunkPending_) {
        return std::unexpected(Error::kInvalidState);
    }
    const auto remaining = body_.size() - bodyOffset_;
    const auto chunkSize = std::min<std::uint64_t>(remaining, kHttp3VarIntMax);
    const bool finishing = chunkSize == remaining;
    const auto payload = body_.substr(bodyOffset_, static_cast<std::size_t>(chunkSize));
    auto planned = dataPlan_->planChunk(payload, finishing);
    if (!planned) {
        return failDataPlan();
    }
    chunk_ = *planned;
    chunkPending_ = true;
    segmentOffset_ = 0;
    if (!chunk_.emitsData) {
        state_ = State::kFin;
        chunkPending_ = true;
        return {};
    }
    return {};
}

std::expected<void, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::acknowledge(std::size_t count) noexcept {
    if (state_ == State::kFinished || state_ == State::kFailed || state_ == State::kFin ||
        !offered_) {
        return std::unexpected(Error::kInvalidState);
    }
    const auto segment = activeSegment();
    if (count > segment.size()) {
        return std::unexpected(Error::kExcessiveAcknowledgement);
    }
    if (count == 0) {
        return {};
    }

    if (state_ == State::kHeaders) {
        segmentOffset_ += count;
        if (segmentOffset_ == headers_.size()) {
            offered_ = false;
            state_ = State::kDataHeader;
            segmentOffset_ = 0;
            return prepareData();
        }
        return {};
    }
    if (state_ == State::kDataHeader) {
        segmentOffset_ += count;
        if (segmentOffset_ == chunk_.frameHeaderSize) {
            offered_ = false;
            state_ = State::kDataBody;
            segmentOffset_ = 0;
        }
        return {};
    }
    if (state_ == State::kDataBody) {
        segmentOffset_ += count;
        if (segmentOffset_ == chunk_.payload.size()) {
            offered_ = false;
            const auto chunkBytes = chunk_.payload.size();
            if (chunk_.finishing) {
                state_ = State::kFin;
                return {};
            }
            if (auto committed = dataPlan_->commitPayload(chunkBytes, false); !committed) {
                return failDataPlan();
            }
            bodyOffset_ += chunkBytes;
            chunkPending_ = false;
            segmentOffset_ = 0;
            state_ = State::kDataHeader;
            return prepareData();
        }
        return {};
    }
    return std::unexpected(Error::kInvalidState);
}

std::expected<void, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::acknowledgeFin(bool successful) noexcept {
    if (state_ != State::kFin || !dataPlan_ || !chunkPending_) {
        return std::unexpected(Error::kInvalidState);
    }
    if (!successful) {
        state_ = State::kFailed;
        chunkPending_ = false;
        return {};
    }
    const auto committed = dataPlan_->commitPayload(chunk_.payload.size(), true);
    if (!committed) {
        return failDataPlan();
    }
    state_ = State::kFinished;
    chunkPending_ = false;
    return {};
}

bool Http3BufferedResponseWrite::finReady() const noexcept {
    return state_ == State::kFin;
}

bool Http3BufferedResponseWrite::finished() const noexcept {
    return state_ == State::kFinished;
}

bool Http3BufferedResponseWrite::failed() const noexcept {
    return state_ == State::kFailed;
}

std::expected<void, Http3BufferedResponseWrite::Error>
Http3BufferedResponseWrite::failDataPlan() noexcept {
    state_ = State::kFailed;
    chunkPending_ = false;
    return std::unexpected(Error::kDataPlan);
}

}  // namespace ruvia::detail
