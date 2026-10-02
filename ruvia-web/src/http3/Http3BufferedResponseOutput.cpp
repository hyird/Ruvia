#include "ruvia/web/detail/http3/Http3BufferedResponseOutput.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

#include "ruvia/http/Http3Frames.h"

namespace ruvia::detail {

Http3BufferedResponseOutput::Http3BufferedResponseOutput(const HttpResponse& response,
    Http3StreamMailbox& mailbox, MessageId messageId,
    ruvia::http3_buffered_response_cursor cursor, std::uint64_t initialPublishedWireBytes) noexcept
    : response_(&response),
      mailbox_(mailbox),
      messageId_(messageId),
      cursor_(std::move(cursor)),
      publishedWireBytes_(initialPublishedWireBytes) {}

std::expected<Http3BufferedResponseOutput, Http3BufferedResponseOutput::Error>
Http3BufferedResponseOutput::create(const HttpResponse& response,
    const HttpBufferedResponseWritePlan& writePlan, WorkerMemory& worker,
    Http3StreamMailbox& mailbox, MessageId messageId,
    std::optional<std::uint64_t> peerMaxFieldSectionSize, std::uint64_t initialPublishedWireBytes) noexcept {
    auto cursor = ruvia::http3_buffered_response_cursor::create(response, writePlan, worker.resource());
    if (!cursor) {
        return std::unexpected(cursorError(cursor.error()));
    }
    if (peerMaxFieldSectionSize &&
        std::cmp_greater(cursor->decoded_field_section_size(), *peerMaxFieldSectionSize)) {
        return std::unexpected(Error::kPeerFieldSectionLimit);
    }
    return Http3BufferedResponseOutput(
        response, mailbox, messageId, std::move(*cursor), initialPublishedWireBytes);
}

std::expected<Http3BufferedResponseOutput, Http3BufferedResponseOutput::Error>
Http3BufferedResponseOutput::create(const HttpResponse& response, const HttpBufferedResponseWritePlan& writePlan, Http3ResponseHead encodedHead,
    WorkerMemory& worker, Http3StreamMailbox& mailbox, MessageId messageId, std::optional<std::uint64_t> peerMaxFieldSectionSize, std::uint64_t initialPublishedWireBytes) noexcept {
    if (peerMaxFieldSectionSize && std::cmp_greater(encodedHead.decodedFieldSectionSize(), *peerMaxFieldSectionSize)) {
        return std::unexpected(Error::kPeerFieldSectionLimit);
    }
    auto cursor = ruvia::http3_buffered_response_cursor::create(response, writePlan, std::move(encodedHead), worker.resource());
    if (!cursor) {
        return std::unexpected(cursorError(cursor.error()));
    }
    return Http3BufferedResponseOutput(response, mailbox, messageId, std::move(*cursor), initialPublishedWireBytes);
}

Http3BufferedResponseOutput::Result Http3BufferedResponseOutput::publishStep() noexcept {
    if (state_ == State::kComplete) {
        return result(Status::kComplete);
    }
    if (state_ == State::kFailed) {
        return result(Status::kFailed, BlockReason::kNone, failure_);
    }
    if (response_ == nullptr || !cursor_) {
        return fail(Error::kInvalidCursorState);
    }
    if (mailbox_.stopped()) {
        return fail(Error::kMailboxStopped);
    }

    (void)mailbox_.drainReturns();
    switch (cursor_->next_step()) {
        case ruvia::http3_buffered_response_cursor::step::complete:
            state_ = State::kComplete;
            cursor_.reset();
            response_ = nullptr;
            return result(Status::kComplete);
        case ruvia::http3_buffered_response_cursor::step::failed:
            return fail(Error::kInvalidCursorState);
        case ruvia::http3_buffered_response_cursor::step::fin: {
            if (publishedWireBytes_ > kHttp3VarIntMax) {
                return fail(Error::kWireByteCountOverflow);
            }
            const Http3StreamControl fin{Http3StreamControl::Kind::kStreamFin,
                messageId_, publishedWireBytes_};
            const auto sent = mailbox_.trySendControl(fin);
            if (sent == Http3StreamMailbox::ControlResult::kFull) {
                return result(Status::kBackpressured, BlockReason::kControl);
            }
            if (sent == Http3StreamMailbox::ControlResult::kStopped) {
                return fail(Error::kMailboxStopped);
            }
            const bool notifyPeer = sent == Http3StreamMailbox::ControlResult::kSentNotifyPeer;
            const auto acknowledged = cursor_->acknowledge_fin(true);
            if (!acknowledged) {
                return fail(cursorError(acknowledged.error()), 0, notifyPeer);
            }
            state_ = State::kComplete;
            cursor_.reset();
            response_ = nullptr;
            return result(Status::kFin, BlockReason::kNone, Error::kNone, 0, notifyPeer);
        }
        case ruvia::http3_buffered_response_cursor::step::bytes:
            break;
    }

    auto segment = cursor_->next();
    if (!segment || segment->empty()) {
        return fail(segment ? Error::kInvalidCursorState : cursorError(segment.error()));
    }
    const auto count = std::min(segment->size(), Http3StreamMailbox::kMaxBlockBytes);
    if (count == 0 || publishedWireBytes_ > kHttp3VarIntMax - count) {
        return fail(Error::kWireByteCountOverflow);
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(segment->data());
    const auto sent = mailbox_.trySend(messageId_, std::span<const std::byte>(bytes, count));
    if (sent == Http3StreamMailbox::SendResult::kFull ||
        sent == Http3StreamMailbox::SendResult::kNoBlock) {
        return result(Status::kBackpressured, BlockReason::kData);
    }
    if (sent == Http3StreamMailbox::SendResult::kStopped) {
        return fail(Error::kMailboxStopped);
    }
    if (sent != Http3StreamMailbox::SendResult::kSent &&
        sent != Http3StreamMailbox::SendResult::kSentNotifyPeer) {
        return fail(Error::kInvalidCursorState);
    }

    const bool notifyPeer = sent == Http3StreamMailbox::SendResult::kSentNotifyPeer;
    publishedWireBytes_ += count;
    const auto acknowledged = cursor_->acknowledge(count);
    if (!acknowledged) {
        return fail(cursorError(acknowledged.error()), count, notifyPeer);
    }
    return result(Status::kBytes, BlockReason::kNone, Error::kNone, count, notifyPeer);
}

void Http3BufferedResponseOutput::stop() noexcept {
    if (state_ != State::kPublishing) {
        return;
    }
    failure_ = Error::kStopped;
    state_ = State::kFailed;
    cursor_.reset();
    response_ = nullptr;
}

Http3BufferedResponseOutput::NextStep Http3BufferedResponseOutput::nextStep() const noexcept {
    if (state_ == State::kComplete) {
        return NextStep::complete;
    }
    if (state_ == State::kFailed || !cursor_) {
        return NextStep::failed;
    }
    return cursor_->next_step();
}

std::size_t Http3BufferedResponseOutput::decodedFieldSectionSize() const noexcept {
    return cursor_ ? cursor_->decoded_field_section_size() : 0;
}

bool Http3BufferedResponseOutput::complete() const noexcept {
    return state_ == State::kComplete;
}

bool Http3BufferedResponseOutput::failed() const noexcept {
    return state_ == State::kFailed;
}

std::uint64_t Http3BufferedResponseOutput::publishedWireBytes() const noexcept {
    return publishedWireBytes_;
}

Http3BufferedResponseOutput::Error Http3BufferedResponseOutput::cursorError(
    ruvia::http3_buffered_response_cursor::error error) noexcept {
    switch (error) {
        case ruvia::http3_buffered_response_error::invalid_response_plan:
            return Error::kInvalidResponsePlan;
        case ruvia::http3_buffered_response_error::file_body_unsupported:
            return Error::kFileBodyUnsupported;
        case ruvia::http3_buffered_response_error::response_encoding:
            return Error::kResponseEncoding;
        case ruvia::http3_buffered_response_error::out_of_memory:
            return Error::kOutOfMemory;
        case ruvia::http3_buffered_response_error::invalid_state:
            return Error::kInvalidCursorState;
        case ruvia::http3_buffered_response_error::excessive_acknowledgement:
            return Error::kCursorAcknowledgement;
        case ruvia::http3_buffered_response_error::data_plan:
            return Error::kCursorDataPlan;
    }
    return Error::kInvalidCursorState;
}

Http3BufferedResponseOutput::Result Http3BufferedResponseOutput::fail(
    Error error, std::size_t bytesAccepted, bool notifyPeer) noexcept {
    if (state_ == State::kPublishing) {
        failure_ = error;
        state_ = State::kFailed;
        cursor_.reset();
        response_ = nullptr;
    }
    return result(Status::kFailed, BlockReason::kNone, failure_, bytesAccepted, notifyPeer);
}

Http3BufferedResponseOutput::Result Http3BufferedResponseOutput::result(Status status,
    BlockReason blockReason, Error error, std::size_t bytesAccepted,
    bool notifyPeer) const noexcept {
    return {.status = status,
        .blockReason = blockReason,
        .error = error,
        .bytesAccepted = bytesAccepted,
        .publishedWireBytes = publishedWireBytes_,
        .notifyPeer = notifyPeer};
}

}  // namespace ruvia::detail
