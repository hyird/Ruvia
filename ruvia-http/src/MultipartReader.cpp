#include "ruvia/http/MultipartReader.h"

#include "MultipartReaderInternal.h"
#include "MultipartParsing.h"
#include "ruvia/http/detail/PmrResource.h"
#include "ruvia/http/detail/PmrString.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ruvia {

MultipartReader::MultipartReader(
    BodyReader& bodyReader,
    std::string_view boundary,
    std::pmr::memory_resource* resource)
    : bodyReader_(bodyReader),
      resource_(detail::httpPmrResourceOrDefault(resource)),
      buffer_(resource_),
      boundaryLine_(resource_),
      boundaryPrefix_(resource_),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {
    detail::httpAssignMultipartBoundaryMarkers(boundaryLine_, boundaryPrefix_, boundary);
}

std::string_view MultipartReader::bufferView() const noexcept {
    if (bufferOffset_ >= buffer_.size()) {
        return {};
    }
    return std::string_view(buffer_.data() + bufferOffset_, buffer_.size() - bufferOffset_);
}

void MultipartReader::consume(std::size_t bytes) noexcept {
    const auto available = bufferView().size();
    bufferOffset_ += std::min(bytes, available);
    if (bufferOffset_ == buffer_.size()) {
        buffer_.clear();
        bufferOffset_ = 0;
    }
}

void MultipartReader::compactConsumedPrefix() {
    detail::compactConsumedPrefix(buffer_, bufferOffset_, kCompactConsumedPrefixBytes);
}

void MultipartReader::compactPending() {
    if (pendingEraseBytes_ == 0) {
        return;
    }
    consume(pendingEraseBytes_);
    pendingEraseBytes_ = 0;
}

void MultipartReader::appendChunk(std::string_view chunk) {
    compactConsumedPrefix();
    buffer_.append(chunk.data(), chunk.size());
}

MultipartReader::PollResult MultipartReader::poll() {
    for (;;) {
        compactPending();
        switch (state_) {
            case State::kBoundary: {
                const auto status = processBoundary();
                if (status == PollStatus::kNeedMore || status == PollStatus::kDone) {
                    return {status, std::nullopt};
                }
                break;
            }
            case State::kHeaders: {
                const auto status = processHeaders();
                if (status == PollStatus::kNeedMore) {
                    return {status, std::nullopt};
                }
                break;
            }
            case State::kBody: {
                auto result = readBodyChunk();
                if (result.status != PollStatus::kContinue) {
                    return result;
                }
                break;
            }
            case State::kDone:
                return {PollStatus::kDone, std::nullopt};
        }
    }
}

MultipartReader::PollStatus MultipartReader::processBoundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    constexpr std::size_t kMaxMultipartPreambleBytes = 64 * 1024;
    for (;;) {
        if (firstBoundary_) {
            const auto boundary = std::string_view(boundaryLine_).substr(2);
            const auto pos = detail::httpFindMultipartBoundaryLine(bufferView(), boundary);
            if (pos == std::string_view::npos) {
                if (bufferView().size() > kMaxMultipartPreambleBytes) {
                    throw std::invalid_argument("multipart preamble exceeds limit");
                }
                return PollStatus::kNeedMore;
            }
            consume(pos);
            firstBoundary_ = false;
        } else if (bufferView().starts_with("\r\n")) {
            consume(2);
        }
        if (bufferView().size() < boundaryLine_.size() + 2) {
            return PollStatus::kNeedMore;
        }
        const auto buffer = bufferView();
        if (!buffer.starts_with(boundaryLine_)) {
            throw std::invalid_argument("invalid multipart body");
        }
        const auto afterBoundary = boundaryLine_.size();
        if (buffer.substr(afterBoundary, 2) == "--") {
            state_ = State::kDone;
            return PollStatus::kDone;
        }
        if (buffer.substr(afterBoundary, 2) == "\r\n") {
            consume(afterBoundary + 2);
            state_ = State::kHeaders;
            return PollStatus::kContinue;
        }
        // The two bytes after the boundary are present and cannot become a valid
        // terminator after more input; reject now instead of buffering unboundedly.
        throw std::invalid_argument("invalid multipart body");
    }
}

MultipartReader::PollStatus MultipartReader::processHeaders() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    constexpr std::size_t kMaxMultipartHeaderBytes = 64 * 1024;
    for (;;) {
        const auto buffer = bufferView();
        const auto headersEnd = buffer.find("\r\n\r\n");
        if (headersEnd == std::string_view::npos) {
            if (buffer.size() > kMaxMultipartHeaderBytes) {
                throw std::invalid_argument("multipart part headers exceed limit");
            }
            return PollStatus::kNeedMore;
        }

        const auto headers = buffer.substr(0, headersEnd);
        detail::HttpMultipartPartHeaders partHeaders;
        switch (detail::httpParseMultipartPartHeaders(headers, partHeaders)) {
            case detail::HttpMultipartPartHeaderStatus::kOk:
                break;
            case detail::HttpMultipartPartHeaderStatus::kInvalidDisposition:
                throw std::invalid_argument("invalid multipart content disposition");
            case detail::HttpMultipartPartHeaderStatus::kMissingName:
                throw std::invalid_argument("invalid multipart field name");
        }

        currentName_.clear();
        detail::httpAppendDecodedQuotedPairs(currentName_, partHeaders.name);
        currentFilename_.clear();
        currentContentType_.clear();
        if (!partHeaders.filename.empty()) {
            detail::httpAppendDecodedQuotedPairs(currentFilename_, partHeaders.filename);
        }
        if (!partHeaders.contentType.empty()) {
            currentContentType_.assign(partHeaders.contentType.data(), partHeaders.contentType.size());
        }
        consume(headersEnd + 4);
        partBegin_ = true;
        state_ = State::kBody;
        return PollStatus::kContinue;
    }
}

MultipartStreamPart MultipartReader::makePart(std::string_view body, bool partEnd) {
    auto part = detail::MultipartStreamPartAccess::make(
        currentName_,
        currentFilename_,
        currentContentType_,
        body,
        partBegin_,
        partEnd);
    partBegin_ = false;
    return part;
}

MultipartReader::PollResult MultipartReader::readBodyChunk() {
    for (;;) {
        const auto buffer = bufferView();
        const auto boundary = detail::httpFindMultipartBoundaryPrefix(
            buffer, std::string_view(boundaryPrefix_).substr(4));
        const bool boundaryConfirmed = boundary != std::string_view::npos &&
            boundary + boundaryPrefix_.size() + 2 <= buffer.size();
        if (boundaryConfirmed) {
            if (boundary > 0 || partBegin_) {
                auto part = makePart(buffer.substr(0, boundary), true);
                pendingEraseBytes_ = boundary;
                state_ = State::kBoundary;
                return {PollStatus::kPart, std::move(part)};
            }
            state_ = State::kBoundary;
            return {PollStatus::kContinue, std::nullopt};
        }

        const auto keepTail = boundaryLine_.size() + 6;
        if (buffer.size() > keepTail) {
            const auto bytes = buffer.size() - keepTail;
            auto part = makePart(buffer.substr(0, bytes), false);
            pendingEraseBytes_ = bytes;
            return {PollStatus::kPart, std::move(part)};
        }

        return {PollStatus::kNeedMore, std::nullopt};
    }
}

}  // namespace ruvia
