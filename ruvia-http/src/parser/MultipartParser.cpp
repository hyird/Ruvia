#include "ruvia/http/MultipartParser.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/parser/MultipartPartAccess.h"
#include "ruvia/http/detail/parser/MultipartStreamPartAccess.h"
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/http/detail/parser/MultipartDelimiter.h"
#include "ruvia/http/detail/parser/MultipartPartHeaders.h"

// The multipart state machine: find the next delimiter, read one part's header
// block, then hand out that part's body in chunks -- driven entirely by what is
// currently buffered, so a caller may feed the body in any pieces.

namespace ruvia {

namespace {

constexpr std::size_t kMaxMultipartPreambleBytes = 64 * 1024;
constexpr std::size_t kMaxMultipartHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxMultipartDelimiterLineBytes = 64 * 1024;

[[nodiscard]] std::string_view multipartParseErrorMessage(MultipartParseError error) noexcept {
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

[[nodiscard]] HttpProtocolError multipartProtocolError(MultipartParseError error) noexcept {
    switch (error) {
        case MultipartParseError::kPreambleTooLarge:
        case MultipartParseError::kPartHeadersTooLarge:
        case MultipartParseError::kDelimiterLineTooLarge:
            return HttpProtocolError(http_status::kContentTooLarge, multipartParseErrorMessage(error));
        case MultipartParseError::kIncompleteBody:
        case MultipartParseError::kInvalidDelimiter:
        case MultipartParseError::kInvalidPartHeaders:
        case MultipartParseError::kInvalidContentDisposition:
        case MultipartParseError::kMissingFieldName:
            return HttpProtocolError(http_status::kBadRequest, multipartParseErrorMessage(error));
    }
    return HttpProtocolError(http_status::kBadRequest, "invalid multipart body");
}

}  // namespace

HttpProtocolError MultipartPollFailure::protocolError() const noexcept {
    return multipartProtocolError(error_);
}

HttpProtocolError MultipartBodyParseFailure::protocolError() const noexcept {
    return multipartProtocolError(error_);
}

MultipartParser::MultipartParser(MultipartParseOptions options)
    : resource_(detail::httpPmrResourceOrDefault(options.resource)),
      boundary_(std::move(options.boundary)),
      input_(resource_),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {}

MultipartParser::MultipartParser(std::string_view completeBody, MultipartParseOptions options, CompleteInputTag)
    : resource_(detail::httpPmrResourceOrDefault(options.resource)),
      boundary_(std::move(options.boundary)),
      input_(detail::MultipartBorrowedInput{completeBody}),
      currentName_(resource_),
      currentFilename_(resource_),
      currentContentType_(resource_) {}

MultipartBodyParseResult parseMultipartBody(std::string_view body, MultipartParseOptions options) {
    auto* const resource = detail::httpPmrResourceOrDefault(options.resource);
    options.resource = resource;
    MultipartParser parser(body, std::move(options), MultipartParser::CompleteInputTag{});
    std::pmr::vector<MultipartPart> parts(resource);
    for (;;) {
        auto result = parser.poll();
        if (const auto* part = result.part()) {
            parts.push_back(detail::MultipartPartAccess::makeDecoded(part->name(), part->filename(), part->contentType(), part->body(), part->hasFilename(), resource));
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
    if (input_.streamingOpen() == nullptr || progress == nullptr || *progress == ProgressState::kDone) {
        throw std::logic_error("multipart parser cannot accept input in a terminal state");
    }
    input_.feed(chunk);
}

void MultipartParser::finishInput() noexcept {
    input_.finishInput();
}

MultipartPollResult MultipartParser::fail(MultipartParseError error) noexcept {
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
                        return fail(MultipartParseError::kIncompleteBody);
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
                        return fail(MultipartParseError::kIncompleteBody);
                    }
                    return MultipartPollResult::makeNeedInput();
                }
                break;
            }
            case ProgressState::kBody: {
                auto result = readBodyChunk();
                if (result.needInput() != nullptr && input_.eof()) {
                    return fail(MultipartParseError::kIncompleteBody);
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
            const auto delimiter = detail::httpFindInitialMultipartDelimiter(bufferView(), boundary_, input_.eof());
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
                if (bufferBytes - needInput->offset() > kMaxMultipartDelimiterLineBytes) {
                    return MultipartParseError::kDelimiterLineTooLarge;
                }
                return StepProgress::kNeedInput;
            }
            const auto* part = delimiter.part();
            const auto* close = delimiter.close();
            if (part == nullptr && close == nullptr) {
                return MultipartParseError::kInvalidDelimiter;
            }
            const auto preambleBytes = part != nullptr ? part->offset() : close->offset();
            if (preambleBytes > kMaxMultipartPreambleBytes) {
                return MultipartParseError::kPreambleTooLarge;
            }
            consume(preambleBytes);
            firstBoundary_ = false;
        } else if (bufferView().starts_with("\r\n")) {
            consume(2);
        }

        const auto delimiter = detail::httpMatchMultipartDelimiterLine(bufferView(), boundary_, input_.eof());
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
        currentFilenamePresent_ = partHeaders->hasFilename();
        currentContentType_.clear();
        currentContentTypeView_ = {};
        if (currentFilenamePresent_) {
            detail::httpAppendDecodedQuotedPairs(currentFilename_, partHeaders->filename());
        }
        if (!partHeaders->contentType().empty()) {
            if (input_.borrowed() != nullptr) {
                currentContentTypeView_ = partHeaders->contentType();
            } else {
                currentContentType_.assign(partHeaders->contentType().data(), partHeaders->contentType().size());
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
    const auto phase = nextChunkIsFirst_ ? (partEnd ? MultipartChunkPhase::kComplete : MultipartChunkPhase::kFirst) : (partEnd ? MultipartChunkPhase::kLast : MultipartChunkPhase::kMiddle);
    auto part = detail::MultipartStreamPartAccess::make(currentName_, currentFilename_, currentContentTypeView_, body, phase, currentFilenamePresent_);
    nextChunkIsFirst_ = false;
    return part;
}

MultipartPollResult MultipartParser::readBodyChunk() {
    for (;;) {
        const auto buffer = bufferView();
        const auto delimiter = detail::httpFindMultipartBodyDelimiter(buffer, boundary_, input_.eof());
        const auto* partDelimiter = delimiter.part();
        const auto* closeDelimiter = delimiter.close();
        if (partDelimiter != nullptr || closeDelimiter != nullptr) {
            const auto delimiterLineBytes = partDelimiter != nullptr ? partDelimiter->lineBytes() : closeDelimiter->lineBytes();
            if (delimiterLineBytes > kMaxMultipartDelimiterLineBytes) {
                return fail(MultipartParseError::kDelimiterLineTooLarge);
            }
            const auto delimiterOffset = partDelimiter != nullptr ? partDelimiter->offset() : closeDelimiter->offset();
            auto part = makePart(buffer.substr(0, delimiterOffset), true);
            pendingEraseBytes_ = delimiterOffset;
            state_ = ProgressState::kBoundary;
            return MultipartPollResult::makePart(part);
        }
        if (const auto* needInput = delimiter.needInput()) {
            if (buffer.size() - needInput->offset() > kMaxMultipartDelimiterLineBytes + 2) {
                return fail(MultipartParseError::kDelimiterLineTooLarge);
            }
            if (needInput->offset() > 0) {
                auto part = makePart(buffer.substr(0, needInput->offset()), false);
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
