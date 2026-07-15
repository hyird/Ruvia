#include "ruvia/http/MultipartParser.h"

#include "ruvia/http/detail/MultipartReaderInternal.h"
#include "ruvia/http/detail/MultipartParsing.h"
#include "ruvia/http/detail/MultipartPartAccess.h"
#include "ruvia/http/detail/PmrResource.h"
#include "ruvia/http/detail/PmrString.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] std::string_view multipartParseErrorMessage(
    MultipartParseError error) noexcept {
    switch (error) {
        case MultipartParseError::kIncompleteBody:
            return "incomplete multipart body";
        case MultipartParseError::kInvalidDelimiter:
            return "invalid multipart delimiter";
        case MultipartParseError::kPreambleTooLarge:
            return "multipart preamble exceeds limit";
        case MultipartParseError::kPartHeadersTooLarge:
            return "multipart part headers exceed limit";
        case MultipartParseError::kInvalidContentDisposition:
            return "invalid multipart content disposition";
        case MultipartParseError::kMissingFieldName:
            return "invalid multipart field name";
        case MultipartParseError::kDelimiterLineTooLarge:
            return "multipart delimiter line exceeds limit";
    }
    return "invalid multipart body";
}

[[nodiscard]] HttpProtocolError multipartProtocolError(
    MultipartParseError error) noexcept {
    switch (error) {
        case MultipartParseError::kPreambleTooLarge:
        case MultipartParseError::kPartHeadersTooLarge:
        case MultipartParseError::kDelimiterLineTooLarge:
            return HttpProtocolError(
                413, multipartParseErrorMessage(error));
        case MultipartParseError::kIncompleteBody:
        case MultipartParseError::kInvalidDelimiter:
        case MultipartParseError::kInvalidContentDisposition:
        case MultipartParseError::kMissingFieldName:
            return HttpProtocolError(
                400, multipartParseErrorMessage(error));
    }
    return HttpProtocolError(400, "invalid multipart body");
}

}  // namespace

HttpProtocolError MultipartPollFailure::protocolError() const noexcept {
    return multipartProtocolError(error_);
}

HttpProtocolError MultipartBodyParseFailure::protocolError() const noexcept {
    return multipartProtocolError(error_);
}

MultipartParser::MultipartParser(MultipartBoundary boundary, std::pmr::memory_resource* resource)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      boundary_(std::move(boundary)),
      buffer_(resource_),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {}

MultipartParser::MultipartParser(
    std::string_view completeBody,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource,
    CompleteInputTag)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      boundary_(std::move(boundary)),
      buffer_(resource_),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_),
      borrowedInput_(completeBody),
      borrowedInputMode_(true),
      inputFinished_(true) {}

MultipartBodyParseResult parseMultipartBody(
    std::string_view body,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource) {
    resource = detail::httpPmrResourceOrDefault(resource);
    MultipartParser parser(
        body, std::move(boundary), resource, MultipartParser::CompleteInputTag{});
    std::pmr::vector<MultipartPart> parts(resource);
    for (;;) {
        auto result = parser.poll();
        if (const auto* part = result.part()) {
            parts.push_back(detail::MultipartPartAccess::makeDecoded(
                part->name(),
                part->filename(),
                part->contentType(),
                part->body(),
                resource));
            continue;
        }
        if (result.done() != nullptr) {
            return MultipartBodyParseResult(std::move(parts));
        }
        if (const auto* failure = result.failure()) {
            return MultipartBodyParseResult(*failure);
        }
        return MultipartBodyParseResult(MultipartParseError::kIncompleteBody);
    }
}

std::string_view MultipartParser::bufferView() const noexcept {
    const auto source = borrowedInputMode_
        ? borrowedInput_
        : std::string_view(buffer_.data(), buffer_.size());
    if (bufferOffset_ >= source.size()) {
        return {};
    }
    return source.substr(bufferOffset_);
}

void MultipartParser::consume(std::size_t bytes) noexcept {
    const auto available = bufferView().size();
    bufferOffset_ += std::min(bytes, available);
    if (borrowedInputMode_) {
        return;
    }
    if (bufferOffset_ == buffer_.size()) {
        buffer_.clear();
        bufferOffset_ = 0;
    }
}

void MultipartParser::compactConsumedPrefix() {
    if (borrowedInputMode_) {
        return;
    }
    detail::compactConsumedPrefix(buffer_, bufferOffset_, kCompactConsumedPrefixBytes);
}

void MultipartParser::compactPending() {
    if (pendingEraseBytes_ == 0) {
        return;
    }
    consume(pendingEraseBytes_);
    pendingEraseBytes_ = 0;
}

void MultipartParser::feed(std::string_view chunk) {
    const auto* progress = std::get_if<ProgressState>(&state_);
    if (borrowedInputMode_ || inputFinished_ || progress == nullptr ||
        *progress == ProgressState::kDone) {
        throw std::logic_error(
            "multipart parser cannot accept input in a terminal state");
    }
    compactConsumedPrefix();
    buffer_.append(chunk.data(), chunk.size());
}

void MultipartParser::finishInput() noexcept {
    inputFinished_ = true;
}

MultipartParseError MultipartParser::stepError(StepStatus status) noexcept {
    switch (status) {
        case StepStatus::kInvalidDelimiter:
            return MultipartParseError::kInvalidDelimiter;
        case StepStatus::kPreambleTooLarge:
            return MultipartParseError::kPreambleTooLarge;
        case StepStatus::kPartHeadersTooLarge:
            return MultipartParseError::kPartHeadersTooLarge;
        case StepStatus::kInvalidContentDisposition:
            return MultipartParseError::kInvalidContentDisposition;
        case StepStatus::kMissingFieldName:
            return MultipartParseError::kMissingFieldName;
        case StepStatus::kNeedInput:
        case StepStatus::kContinue:
        case StepStatus::kDone:
            break;
    }
    return MultipartParseError::kInvalidDelimiter;
}

MultipartPollResult MultipartParser::fail(
    MultipartParseError error) noexcept {
    auto result = MultipartPollResult::makeFailure(error);
    state_ = error;
    return result;
}

MultipartPollResult MultipartParser::poll() {
    if (const auto* failure = std::get_if<MultipartParseError>(&state_)) {
        return MultipartPollResult::makeFailure(*failure);
    }
    for (;;) {
        compactPending();
        switch (std::get<ProgressState>(state_)) {
            case ProgressState::kBoundary: {
                const auto status = processBoundary();
                if (status == StepStatus::kNeedInput) {
                    if (inputFinished_) {
                        return fail(
                            MultipartParseError::kIncompleteBody);
                    }
                    return MultipartPollResult::makeNeedInput();
                }
                if (status == StepStatus::kDone) {
                    return MultipartPollResult::makeDone();
                }
                if (status != StepStatus::kContinue) {
                    return fail(stepError(status));
                }
                break;
            }
            case ProgressState::kHeaders: {
                const auto status = processHeaders();
                if (status == StepStatus::kNeedInput) {
                    if (inputFinished_) {
                        return fail(
                            MultipartParseError::kIncompleteBody);
                    }
                    return MultipartPollResult::makeNeedInput();
                }
                if (status != StepStatus::kContinue) {
                    return fail(stepError(status));
                }
                break;
            }
            case ProgressState::kBody: {
                auto result = readBodyChunk();
                if (result.needInput() != nullptr && inputFinished_) {
                    return fail(
                        MultipartParseError::kIncompleteBody);
                }
                return result;
            }
            case ProgressState::kDone:
                return MultipartPollResult::makeDone();
        }
    }
}

MultipartParser::StepStatus MultipartParser::processBoundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    constexpr std::size_t kMaxMultipartPreambleBytes = 64 * 1024;
    for (;;) {
        if (firstBoundary_) {
            const auto delimiter = detail::httpFindInitialMultipartDelimiter(
                bufferView(), boundary_, inputFinished_);
            if (delimiter.noMatch() != nullptr ||
                delimiter.needInput() != nullptr) {
                if (bufferView().size() > kMaxMultipartPreambleBytes) {
                    return StepStatus::kPreambleTooLarge;
                }
                return StepStatus::kNeedInput;
            }
            const auto* part = delimiter.part();
            const auto* close = delimiter.close();
            if (part == nullptr && close == nullptr) {
                return StepStatus::kInvalidDelimiter;
            }
            consume(part != nullptr ? part->offset() : close->offset());
            firstBoundary_ = false;
        } else if (bufferView().starts_with("\r\n")) {
            consume(2);
        }

        const auto delimiter = detail::httpMatchMultipartDelimiterLine(
            bufferView(), boundary_, inputFinished_);
        if (delimiter.needInput() != nullptr) {
            return StepStatus::kNeedInput;
        }
        if (const auto* part = delimiter.part()) {
            consume(part->lineBytes());
            state_ = ProgressState::kHeaders;
            return StepStatus::kContinue;
        }
        if (const auto* close = delimiter.close()) {
            consume(close->lineBytes());
            state_ = ProgressState::kDone;
            return StepStatus::kDone;
        }
        return StepStatus::kInvalidDelimiter;
    }
}

MultipartParser::StepStatus MultipartParser::processHeaders() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    constexpr std::size_t kMaxMultipartHeaderBytes = 64 * 1024;
    for (;;) {
        const auto buffer = bufferView();
        const auto headersEnd = buffer.find("\r\n\r\n");
        if (headersEnd == std::string_view::npos) {
            if (buffer.size() > kMaxMultipartHeaderBytes) {
                return StepStatus::kPartHeadersTooLarge;
            }
            return StepStatus::kNeedInput;
        }

        const auto headers = buffer.substr(0, headersEnd);
        const auto parsedHeaders = detail::httpParseMultipartPartHeaders(headers);
        if (const auto* failure = parsedHeaders.failure()) {
            switch (failure->error()) {
            case detail::HttpMultipartPartHeaderParseError::kInvalidDisposition:
                return StepStatus::kInvalidContentDisposition;
            case detail::HttpMultipartPartHeaderParseError::kMissingName:
                return StepStatus::kMissingFieldName;
            }
        }
        const auto* partHeaders = parsedHeaders.headers();
        if (partHeaders == nullptr) {
            return StepStatus::kInvalidContentDisposition;
        }

        currentName_.clear();
        detail::httpAppendDecodedQuotedPairs(currentName_, partHeaders->name());
        currentFilename_.clear();
        currentContentType_.clear();
        currentContentTypeView_ = {};
        if (!partHeaders->filename().empty()) {
            detail::httpAppendDecodedQuotedPairs(
                currentFilename_, partHeaders->filename());
        }
        if (!partHeaders->contentType().empty()) {
            if (borrowedInputMode_) {
                currentContentTypeView_ = partHeaders->contentType();
            } else {
                currentContentType_.assign(
                    partHeaders->contentType().data(),
                    partHeaders->contentType().size());
                currentContentTypeView_ = currentContentType_;
            }
        }
        consume(headersEnd + 4);
        nextChunkIsFirst_ = true;
        state_ = ProgressState::kBody;
        return StepStatus::kContinue;
    }
}

MultipartStreamPart MultipartParser::makePart(std::string_view body, bool partEnd) {
    const auto phase = nextChunkIsFirst_
        ? (partEnd ? MultipartChunkPhase::kComplete : MultipartChunkPhase::kFirst)
        : (partEnd ? MultipartChunkPhase::kLast : MultipartChunkPhase::kMiddle);
    auto part = detail::MultipartStreamPartAccess::make(
        currentName_,
        currentFilename_,
        currentContentTypeView_,
        body,
        phase);
    nextChunkIsFirst_ = false;
    return part;
}

MultipartPollResult MultipartParser::readBodyChunk() {
    for (;;) {
        const auto buffer = bufferView();
        const auto delimiter = detail::httpFindMultipartBodyDelimiter(
            buffer, boundary_, inputFinished_);
        const auto* partDelimiter = delimiter.part();
        const auto* closeDelimiter = delimiter.close();
        if (partDelimiter != nullptr || closeDelimiter != nullptr) {
            const auto delimiterOffset = partDelimiter != nullptr
                ? partDelimiter->offset()
                : closeDelimiter->offset();
            auto part = makePart(buffer.substr(0, delimiterOffset), true);
            pendingEraseBytes_ = delimiterOffset;
            state_ = ProgressState::kBoundary;
            return MultipartPollResult::makePart(part);
        }
        if (const auto* needInput = delimiter.needInput()) {
            constexpr std::size_t kMaxMultipartDelimiterLineBytes = 64 * 1024;
            if (buffer.size() - needInput->offset() >
                kMaxMultipartDelimiterLineBytes) {
                return fail(
                    MultipartParseError::kDelimiterLineTooLarge);
            }
            if (needInput->offset() > 0) {
                auto part = makePart(
                    buffer.substr(0, needInput->offset()), false);
                pendingEraseBytes_ = needInput->offset();
                return MultipartPollResult::makePart(part);
            }
            return MultipartPollResult::makeNeedInput();
        }

        const auto keepTail = boundary_.value().size() + 8;
        if (buffer.size() > keepTail) {
            const auto bytes = buffer.size() - keepTail;
            auto part = makePart(buffer.substr(0, bytes), false);
            pendingEraseBytes_ = bytes;
            return MultipartPollResult::makePart(part);
        }

        return MultipartPollResult::makeNeedInput();
    }
}

}  // namespace ruvia
