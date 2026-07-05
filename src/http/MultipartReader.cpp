#include "ruvia/http/MultipartReader.h"

#include "MultipartReaderInternal.h"
#include "MultipartParsing.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/PmrResource.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ruvia {

MultipartReader::MultipartReader(
    BodyReader& bodyReader,
    std::string_view boundary,
    std::pmr::memory_resource* resource)
    : bodyReader_(bodyReader),
      resource_(detail::pmrResourceOrDefault(resource)),
      buffer_(resource_),
      boundaryLine_(resource_),
      boundaryPrefix_(resource_),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {
    detail::httpAssignMultipartBoundaryMarkers(boundaryLine_, boundaryPrefix_, boundary);
}

Task<std::optional<MultipartStreamPart>> MultipartReader::read() {
    for (;;) {
        compactPending();
        switch (state_) {
            case State::kBoundary:
                co_await processBoundary();
                if (state_ == State::kDone) {
                    co_return std::nullopt;
                }
                break;
            case State::kHeaders:
                co_await processHeaders();
                break;
            case State::kBody:
                if (auto part = co_await readBodyChunk()) {
                    co_return part;
                }
                break;
            case State::kDone:
                co_return std::nullopt;
        }
    }
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

Task<bool> MultipartReader::appendMore() {
    compactConsumedPrefix();
    auto chunk = co_await bodyReader_.read();
    if (!chunk) {
        co_return false;
    }
    buffer_.append(chunk->data(), chunk->size());
    co_return true;
}

Task<void> MultipartReader::processBoundary() {
    // RFC 2046 §5.1.1: the first boundary may be preceded by a preamble that "is to
    // be ignored". Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered (parseBody) paths accept exactly the same bodies
    // instead of the streaming path rejecting a preamble the buffered path skips.
    // The discarded preamble is bounded like the per-part header block so a body
    // that never presents a boundary cannot buffer without limit. Inter-part
    // boundaries stay strict (must immediately follow the previous part) so a
    // corrupt inter-part gap cannot be silently skipped.
    constexpr std::size_t kMaxMultipartPreambleBytes = 64 * 1024;
    for (;;) {
        if (firstBoundary_) {
            const auto boundary = std::string_view(boundaryLine_).substr(2);
            const auto pos = detail::httpFindMultipartBoundaryLine(bufferView(), boundary);
            if (pos == std::string_view::npos) {
                if (bufferView().size() > kMaxMultipartPreambleBytes) {
                    throw std::invalid_argument("multipart preamble exceeds limit");
                }
                if (!(co_await appendMore())) {
                    throw std::invalid_argument("invalid multipart body");
                }
                continue;
            }
            consume(pos);
            firstBoundary_ = false;
        } else if (bufferView().starts_with("\r\n")) {
            consume(2);
        }
        while (bufferView().size() < boundaryLine_.size() + 2) {
            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
        }
        const auto buffer = bufferView();
        if (!buffer.starts_with(boundaryLine_)) {
            throw std::invalid_argument("invalid multipart body");
        }
        const auto afterBoundary = boundaryLine_.size();
        if (buffer.substr(afterBoundary, 2) == "--") {
            state_ = State::kDone;
            co_return;
        }
        if (buffer.substr(afterBoundary, 2) == "\r\n") {
            consume(afterBoundary + 2);
            state_ = State::kHeaders;
            co_return;
        }
        if (!(co_await appendMore())) {
            throw std::invalid_argument("invalid multipart body");
        }
    }
}

Task<void> MultipartReader::processHeaders() {
    // Cap on a single part's header block, mirroring the 64KB request-header
    // limit, so a part that opens headers but never terminates them (\r\n\r\n)
    // cannot force the entire streamed body to buffer in memory.
    constexpr std::size_t kMaxMultipartHeaderBytes = 64 * 1024;
    for (;;) {
        const auto buffer = bufferView();
        const auto headersEnd = buffer.find("\r\n\r\n");
        if (headersEnd == std::string_view::npos) {
            if (buffer.size() > kMaxMultipartHeaderBytes) {
                throw std::invalid_argument("multipart part headers exceed limit");
            }
            if (!(co_await appendMore())) {
                throw std::invalid_argument("invalid multipart body");
            }
            continue;
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

        // Decode Content-Disposition quoted-pairs so name/filename match the buffered
        // parser (which does the same in MultipartPartAccess::make).
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
        co_return;
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

Task<std::optional<MultipartStreamPart>> MultipartReader::readBodyChunk() {
    for (;;) {
        const auto buffer = bufferView();
        // Match the buffered parser's boundary detection (httpMultipartBoundaryAt via
        // this helper): a "\r\n--<boundary>" run is a real delimiter only when it ends
        // in CRLF or "--". A raw find() would treat the boundary token appearing inside
        // a part body (e.g. "...\r\n--<boundary>x...") as a delimiter, truncating the
        // part and then rejecting the stream, diverging from the buffered path. Sole
        // owner of boundary detection now shared by both parsers. boundaryPrefix_ is
        // "\r\n--<boundary>", so substr(4) is the bare boundary token.
        const auto boundary = detail::httpFindMultipartBoundaryPrefix(
            buffer, std::string_view(boundaryPrefix_).substr(4));
        // httpFindMultipartBoundaryPrefix returns an EOF-possible match when the
        // terminator (CRLF / "--") is not buffered yet. Commit only once both
        // terminator bytes are present, so a boundary token split across reads -- or a
        // boundary prefix appearing inside a part body -- is not mistaken for a
        // delimiter before its terminator can be confirmed (or rejected as content).
        const bool boundaryConfirmed = boundary != std::string_view::npos &&
            boundary + boundaryPrefix_.size() + 2 <= buffer.size();
        if (boundaryConfirmed) {
            if (boundary > 0 || partBegin_) {
                auto part = makePart(buffer.substr(0, boundary), true);
                pendingEraseBytes_ = boundary;
                state_ = State::kBoundary;
                co_return part;
            }
            state_ = State::kBoundary;
            break;
        }

        const auto keepTail = boundaryLine_.size() + 6;
        if (buffer.size() > keepTail) {
            const auto bytes = buffer.size() - keepTail;
            auto part = makePart(buffer.substr(0, bytes), false);
            pendingEraseBytes_ = bytes;
            co_return part;
        }

        if (!(co_await appendMore())) {
            throw std::invalid_argument("invalid multipart body");
        }
    }

    co_return std::nullopt;
}

}  // namespace ruvia
