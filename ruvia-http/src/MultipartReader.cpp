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

constexpr std::size_t kMaxMultipartPreambleBytes = 64 * 1024;
constexpr std::size_t kMaxMultipartHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxMultipartDelimiterLineBytes = 64 * 1024;

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
        case MultipartParseError::kInvalidPartHeaders:
            return "invalid multipart part headers";
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
        case MultipartParseError::kInvalidPartHeaders:
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

detail::MultipartInputLifecycle::MultipartInputLifecycle(
    std::pmr::memory_resource* resource)
    : value_(
          std::in_place_type<MultipartStreamingInputOpen>,
          detail::httpPmrResourceOrDefault(resource)) {}

detail::MultipartInputLifecycle::MultipartInputLifecycle(
    MultipartBorrowedInput input) noexcept
    : value_(input) {}

const detail::MultipartBorrowedInput*
detail::MultipartInputLifecycle::borrowed() const & noexcept {
    return std::get_if<MultipartBorrowedInput>(&value_);
}

const detail::MultipartStreamingInputOpen*
detail::MultipartInputLifecycle::streamingOpen() const & noexcept {
    return std::get_if<MultipartStreamingInputOpen>(&value_);
}

const detail::MultipartStreamingInputEof*
detail::MultipartInputLifecycle::streamingEof() const & noexcept {
    return std::get_if<MultipartStreamingInputEof>(&value_);
}

bool detail::MultipartInputLifecycle::eof() const noexcept {
    return borrowed() != nullptr || streamingEof() != nullptr;
}

std::pmr::string* detail::MultipartInputLifecycle::ownedBytes() noexcept {
    if (auto* open = std::get_if<MultipartStreamingInputOpen>(&value_)) {
        return &open->bytes;
    }
    if (auto* eofState = std::get_if<MultipartStreamingInputEof>(&value_)) {
        return &eofState->bytes;
    }
    return nullptr;
}

const std::pmr::string* detail::MultipartInputLifecycle::ownedBytes() const noexcept {
    if (const auto* open = std::get_if<MultipartStreamingInputOpen>(&value_)) {
        return &open->bytes;
    }
    if (const auto* eofState = std::get_if<MultipartStreamingInputEof>(&value_)) {
        return &eofState->bytes;
    }
    return nullptr;
}

std::string_view detail::MultipartInputLifecycle::view() const & noexcept {
    const auto source = borrowed() != nullptr
        ? borrowed()->bytes
        : std::string_view(ownedBytes()->data(), ownedBytes()->size());
    return offset_ >= source.size() ? std::string_view{} : source.substr(offset_);
}

void detail::MultipartInputLifecycle::feed(std::string_view chunk) {
    auto* open = std::get_if<MultipartStreamingInputOpen>(&value_);
    if (open == nullptr) {
        throw std::logic_error("multipart input is not open for feed");
    }
    compactConsumedPrefix(kCompactConsumedPrefixBytes);
    open = std::get_if<MultipartStreamingInputOpen>(&value_);
    open->bytes.append(chunk.data(), chunk.size());
}

void detail::MultipartInputLifecycle::finishInput() noexcept {
    auto* open = std::get_if<MultipartStreamingInputOpen>(&value_);
    if (open == nullptr) {
        return;
    }
    auto bytes = std::move(open->bytes);
    value_.template emplace<MultipartStreamingInputEof>(std::move(bytes));
}

void detail::MultipartInputLifecycle::consume(std::size_t bytes) noexcept {
    const auto available = view().size();
    offset_ += std::min(bytes, available);
    auto* owned = ownedBytes();
    if (owned != nullptr && offset_ == owned->size()) {
        owned->clear();
        offset_ = 0;
    }
}

void detail::MultipartInputLifecycle::compactConsumedPrefix(std::size_t threshold) {
    auto* owned = ownedBytes();
    if (owned != nullptr) {
        detail::compactConsumedPrefix(*owned, offset_, threshold);
    }
}

MultipartParser::MultipartParser(MultipartBoundary boundary, std::pmr::memory_resource* resource)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      boundary_(std::move(boundary)),
      input_(resource_),
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
      input_(detail::MultipartBorrowedInput{completeBody}),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {}

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
    return input_.view();
}

void MultipartParser::consume(std::size_t bytes) noexcept {
    input_.consume(bytes);
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
    if (input_.streamingOpen() == nullptr || progress == nullptr ||
        *progress == ProgressState::kDone) {
        throw std::logic_error(
            "multipart parser cannot accept input in a terminal state");
    }
    input_.feed(chunk);
}

void MultipartParser::finishInput() noexcept {
    input_.finishInput();
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
                const auto step = processBoundary();
                if (const auto* error = std::get_if<MultipartParseError>(&step)) {
                    return fail(*error);
                }
                const auto progress = std::get<StepProgress>(step);
                if (progress == StepProgress::kNeedInput) {
                    if (input_.eof()) {
                        return fail(
                            MultipartParseError::kIncompleteBody);
                    }
                    return MultipartPollResult::makeNeedInput();
                }
                if (progress == StepProgress::kDone) {
                    return MultipartPollResult::makeDone();
                }
                break;
            }
            case ProgressState::kHeaders: {
                const auto step = processHeaders();
                if (const auto* error = std::get_if<MultipartParseError>(&step)) {
                    return fail(*error);
                }
                const auto progress = std::get<StepProgress>(step);
                if (progress == StepProgress::kNeedInput) {
                    if (input_.eof()) {
                        return fail(
                            MultipartParseError::kIncompleteBody);
                    }
                    return MultipartPollResult::makeNeedInput();
                }
                break;
            }
            case ProgressState::kBody: {
                auto result = readBodyChunk();
                if (result.needInput() != nullptr && input_.eof()) {
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

MultipartParser::StepResult MultipartParser::processBoundary() {
    // RFC 2046 section 5.1.1: the first boundary may be preceded by a preamble that
    // is ignored. Skip it once, reusing the buffered parser's boundary finder so
    // the streaming and buffered paths accept exactly the same bodies.
    for (;;) {
        if (firstBoundary_) {
            const auto delimiter = detail::httpFindInitialMultipartDelimiter(
                bufferView(), boundary_, input_.eof());
            if (delimiter.noMatch() != nullptr) {
                if (bufferView().size() > kMaxMultipartPreambleBytes) {
                    return MultipartParseError::kPreambleTooLarge;
                }
                return StepProgress::kNeedInput;
            }
            if (const auto* needInput = delimiter.needInput()) {
                const auto bufferBytes = bufferView().size();
                if (needInput->offset() > kMaxMultipartPreambleBytes) {
                    return MultipartParseError::kPreambleTooLarge;
                }
                if (bufferBytes - needInput->offset() >
                    kMaxMultipartDelimiterLineBytes) {
                    return MultipartParseError::kDelimiterLineTooLarge;
                }
                return StepProgress::kNeedInput;
            }
            const auto* part = delimiter.part();
            const auto* close = delimiter.close();
            if (part == nullptr && close == nullptr) {
                return MultipartParseError::kInvalidDelimiter;
            }
            const auto preambleBytes = part != nullptr
                ? part->offset()
                : close->offset();
            if (preambleBytes > kMaxMultipartPreambleBytes) {
                return MultipartParseError::kPreambleTooLarge;
            }
            consume(preambleBytes);
            firstBoundary_ = false;
        } else if (bufferView().starts_with("\r\n")) {
            consume(2);
        }

        const auto delimiter = detail::httpMatchMultipartDelimiterLine(
            bufferView(), boundary_, input_.eof());
        if (delimiter.needInput() != nullptr) {
            return StepProgress::kNeedInput;
        }
        if (const auto* part = delimiter.part()) {
            if (part->lineBytes() > kMaxMultipartDelimiterLineBytes) {
                return MultipartParseError::kDelimiterLineTooLarge;
            }
            consume(part->lineBytes());
            state_ = ProgressState::kHeaders;
            return StepProgress::kContinue;
        }
        if (const auto* close = delimiter.close()) {
            if (close->lineBytes() > kMaxMultipartDelimiterLineBytes) {
                return MultipartParseError::kDelimiterLineTooLarge;
            }
            consume(close->lineBytes());
            state_ = ProgressState::kDone;
            return StepProgress::kDone;
        }
        return MultipartParseError::kInvalidDelimiter;
    }
}

MultipartParser::StepResult MultipartParser::processHeaders() {
    // Cap on a single part's header block, mirroring the 64KB request-header limit.
    for (;;) {
        const auto buffer = bufferView();
        const auto headersEnd = buffer.find("\r\n\r\n");
        if (headersEnd == std::string_view::npos) {
            if (buffer.size() > kMaxMultipartHeaderBytes) {
                return MultipartParseError::kPartHeadersTooLarge;
            }
            return StepProgress::kNeedInput;
        }
        // Include the terminating CRLF CRLF in the same byte cap. Checking only
        // the incomplete path allowed an oversized but already-terminated block
        // delivered in one feed() to bypass the limit entirely.
        if (headersEnd > kMaxMultipartHeaderBytes - 4) {
            return MultipartParseError::kPartHeadersTooLarge;
        }

        const auto headers = buffer.substr(0, headersEnd);
        const auto parsedHeaders = detail::httpParseMultipartPartHeaders(headers);
        if (const auto* failure = parsedHeaders.failure()) {
            return failure->parseError();
        }
        const auto* partHeaders = parsedHeaders.headers();
        if (partHeaders == nullptr) {
            return MultipartParseError::kInvalidContentDisposition;
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
            if (input_.borrowed() != nullptr) {
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
        return StepProgress::kContinue;
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
            buffer, boundary_, input_.eof());
        const auto* partDelimiter = delimiter.part();
        const auto* closeDelimiter = delimiter.close();
        if (partDelimiter != nullptr || closeDelimiter != nullptr) {
            const auto delimiterLineBytes = partDelimiter != nullptr
                ? partDelimiter->lineBytes()
                : closeDelimiter->lineBytes();
            if (delimiterLineBytes > kMaxMultipartDelimiterLineBytes) {
                return fail(
                    MultipartParseError::kDelimiterLineTooLarge);
            }
            const auto delimiterOffset = partDelimiter != nullptr
                ? partDelimiter->offset()
                : closeDelimiter->offset();
            auto part = makePart(buffer.substr(0, delimiterOffset), true);
            pendingEraseBytes_ = delimiterOffset;
            state_ = ProgressState::kBoundary;
            return MultipartPollResult::makePart(part);
        }
        if (const auto* needInput = delimiter.needInput()) {
            if (buffer.size() - needInput->offset() >
                kMaxMultipartDelimiterLineBytes + 2) {
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
