#include <algorithm>
#include <limits>
#include <stdexcept>

#include "client/HttpClientResponseDecoding.h"
#include "http3/Http3ClientConnection.h"

namespace ruvia::detail {
Http3ClientConnection::PushList::iterator Http3ClientConnection::findPush(RequestId id) noexcept {
    return std::find_if(pushes_.begin(), pushes_.end(), [id](const Push& push) { return push.id == id; });
}
Http3ClientConnection::PushList::iterator Http3ClientConnection::findPushByStream(std::uint64_t streamId) noexcept {
    return std::find_if(pushes_.begin(), pushes_.end(), [streamId](const Push& push) { return push.streamId == streamId; });
}
void Http3ClientConnection::settlePush(std::uint64_t pushId) noexcept {
    if (pushId < kMaxRememberedPushes && !settledPushes_.test(pushId)) {
        settledPushes_.set(pushId);
        ++pendingPushCredits_;
    }
}
void Http3ClientConnection::onPushEvent(void* raw, const Http3ConnectionEvent& event) {
    auto& owner = *static_cast<Http3ClientConnection*>(raw);
    if (!event.pushId || *event.pushId >= kMaxRememberedPushes) {
        return;
    }
    const auto pushId = *event.pushId;
    if (event.kind == Http3ConnectionEventKind::kPushStream) {
        std::optional<TimePoint> deadline;
        if (owner.pushObserver_.config.timeout) {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<TimePoint::duration>(*owner.pushObserver_.config.timeout);
            deadline = now > TimePoint::max() - duration ? TimePoint::max() : now + duration;
        }
        owner.peerPushStreams_.push_back({pushId, event.streamId, deadline});
        const auto found = std::find_if(owner.pushes_.begin(), owner.pushes_.end(),
            [pushId](const Push& push) { return push.pushId == pushId; });
        if (found != owner.pushes_.end()) {
            found->streamId = event.streamId;
        }
        return;
    }
    if (event.kind == Http3ConnectionEventKind::kPushCanceled) {
        const auto found = std::find_if(owner.pushes_.begin(), owner.pushes_.end(),
            [pushId](const Push& push) { return push.pushId == pushId; });
        if (found != owner.pushes_.end()) {
            found->peerCancelled = true;
        } else {
            owner.seenPushes_.set(pushId);
            owner.settlePush(pushId);
        }
        return;
    }
    if (event.kind == Http3ConnectionEventKind::kPushPromise) {
        if (owner.seenPushes_.test(pushId)) {
            return;  // The core already compared repeated promises byte for byte.
        }
        owner.seenPushes_.set(pushId);
        if (event.head == nullptr) {
            throw std::logic_error("HTTP/3 push promise lacks its parsed request");
        }
        // A node exists before publishing the movable consumer. Allocation or
        // queue rejection remains local to the push and is retired after feed.
        auto& push = owner.pushes_.emplace_back();
        push.id = ++owner.nextRequestId_;
        if (push.id == 0) {
            push.id = ++owner.nextRequestId_;
        }
        push.pushId = pushId;
        if (owner.pushObserver_.config.timeout) {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<TimePoint::duration>(*owner.pushObserver_.config.timeout);
            push.deadline = now > TimePoint::max() - duration ? TimePoint::max() : now + duration;
        }
        const auto stream = std::find_if(owner.peerPushStreams_.begin(), owner.peerPushStreams_.end(),
            [pushId](const PeerPushStream& binding) { return binding.pushId == pushId; });
        if (stream != owner.peerPushStreams_.end()) {
            push.streamId = stream->streamId;
            if (stream->deadline && (!push.deadline || *stream->deadline < *push.deadline)) {
                push.deadline = stream->deadline;
            }
        }
        push.state = owner.pushObserver_.receive(owner.pushObserver_.context, owner.pushObserver_.connectionSlot,
            owner, push.id, *event.head);
        if (push.state == nullptr) {
            push.cancelRequested = true;
            return;
        }
        try {
            push.delivery.emplace(*push.state, owner.receiveBodyBudget_);
        } catch (...) {
            push.state->failure = std::current_exception();
            push.cancelRequested = true;
        }
        return;
    }
    const auto push = owner.findPushByStream(event.streamId);
    if (push == owner.pushes_.end() || !push->delivery) {
        return;
    }
    const auto sink = push->delivery->eventSink();
    sink.callback(sink.context, event);
}

void Http3ClientConnection::finishPush(PushList::iterator found, Outcome outcome) {
    auto& push = *found;
    // Only the sole driver stops stream delivery, before releasing the parser
    // and the borrowed response sink. SSL_free stops a peer UNI receive half.
    if (push.streamId) {
        if (session_) {
            const auto closed = session_->transport().close_stream(*push.streamId);
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
                throw std::runtime_error("HTTP/3 push receive stream could not retire");
            }
        }
        if (session_ && !responseEngine_.retirePushStream(*push.streamId)) {
            throw std::runtime_error("HTTP/3 push parser could not retire");
        }
        receiver_.retire(*push.streamId);
        std::erase(peerStreams_, *push.streamId);
        std::erase_if(peerPushStreams_, [&push](const PeerPushStream& binding) { return binding.pushId == push.pushId; });
    } else if (outcome != Outcome::kComplete && !push.peerCancelled && session_) {
        cancelledPushes_.set(push.pushId);
    }
    auto* state = push.state;
    if (state != nullptr) {
        auto error = outcome == Outcome::kCancelled          ? HttpClientError::Code::kCancelled
                     : outcome == Outcome::kDeadline         ? HttpClientError::Code::kTimeout
                     : outcome == Outcome::kResponseTooLarge ? HttpClientError::Code::kResponseTooLarge
                     : outcome == Outcome::kTransportError   ? HttpClientError::Code::kIoError
                                                             : HttpClientError::Code::kProtocolError;
        if (push.delivery) {
            if (push.delivery->callbackFailure()) {
                (void)push.delivery->commitFailure(push.delivery->callbackFailure());
            } else if (push.delivery->retirementReason() != Http3ClientResponseDelivery::RetirementReason::kNone) {
                error = push.delivery->retirementReason() == Http3ClientResponseDelivery::RetirementReason::kResponseTooLarge
                            ? HttpClientError::Code::kResponseTooLarge
                            : HttpClientError::Code::kProtocolError;
                (void)push.delivery->commitRetirementFailure(error);
            } else if (outcome == Outcome::kComplete) {
                try {
                    const auto plan = push.delivery->responseBodyPlan();
                    decodeHttpClientResponseContentEncoding(*state,
                        plan && plan->contentSemantics() == HttpResponseContentSemantics::kWithContent,
                        maxResponseBytes_);
                    if (push.deadline && std::chrono::steady_clock::now() >= *push.deadline) {
                        state->discardResponseBody();
                        (void)push.delivery->commitTerminalError(HttpClientError::Code::kTimeout);
                    } else if (push.delivery->commitComplete() != Http3ClientResponseDelivery::CommitStatus::kCommitted) {
                        (void)push.delivery->commitTerminalError(HttpClientError::Code::kProtocolError);
                    }
                } catch (...) {
                    (void)push.delivery->commitFailure(std::current_exception());
                }
            } else {
                (void)push.delivery->commitTerminalError(error);
            }
        } else {
            state->errorCode = static_cast<std::uint8_t>(error);
            state->complete = true;
            state->headSignal.notify();
            state->dataSignal.notify();
            state->spaceSignal.notify();
        }
        // Results and body reservations belong to the pool's separate memory
        // and budget domains and can outlive this connection or the client.
        state->transport = HttpClientResponseTransport::kUnassigned;
        state->http3Connection = nullptr;
        state->http3RequestId = 0;
        state->requestId = 0;
        push.delivery.reset();
        push.state = nullptr;
        state->releaseReference();
        pushObserver_.finished(pushObserver_.context);
    }
    settlePush(push.pushId);
    pushes_.erase(found);
}

bool Http3ClientConnection::sweepPushes() {
    bool progress = false;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pushes_.begin(); it != pushes_.end();) {
        auto current = it++;
        if (current->cancelRequested || current->peerCancelled ||
            (current->state != nullptr && current->state->abandoned)) {
            finishPush(current, Outcome::kCancelled);
            progress = true;
        } else if (current->deadline && now >= *current->deadline) {
            finishPush(current, Outcome::kDeadline);
            progress = true;
        } else if (current->delivery && current->delivery->retirementReason() != Http3ClientResponseDelivery::RetirementReason::kNone) {
            finishPush(current, Outcome::kProtocolError);
            progress = true;
        }
    }
    for (std::size_t index = 0; index < peerPushStreams_.size();) {
        const auto binding = peerPushStreams_[index];
        if (findPushByStream(binding.streamId) == pushes_.end() &&
            (settledPushes_.test(binding.pushId) || (binding.deadline && now >= *binding.deadline))) {
            if (session_) {
                const auto closed = session_->transport().close_stream(binding.streamId);
                if ((closed != ruvia::quic_operation_status::accepted &&
                        closed != ruvia::quic_operation_status::completed &&
                        closed != ruvia::quic_operation_status::retired) ||
                    !responseEngine_.retirePushStream(binding.streamId)) {
                    throw std::runtime_error("HTTP/3 unpromised push stream could not retire");
                }
            }
            receiver_.retire(binding.streamId);
            std::erase(peerStreams_, binding.streamId);
            peerPushStreams_.erase(peerPushStreams_.begin() + static_cast<std::ptrdiff_t>(index));
            seenPushes_.set(binding.pushId);
            settlePush(binding.pushId);
            progress = true;
        } else {
            ++index;
        }
    }
    return progress;
}

bool Http3ClientConnection::flushPushControl() {
    if (pushObserver_.receive == nullptr) {
        return false;
    }
    bool progress = false;
    for (std::size_t id = 0; id < cancelledPushes_.size(); ++id) {
        if (cancelledPushes_.test(id)) {
            if (!responseEngine_.queueCancelPush(id)) {
                return progress;
            }
            cancelledPushes_.reset(id);
            progress = true;
        }
    }
    // Remembered promise metadata is deliberately finite per connection. It
    // cannot be forgotten while a valid repeated promise may still arrive.
    while (pendingPushCredits_ != 0 && authorizedPushId_ + 1 < kMaxRememberedPushes) {
        if (!responseEngine_.queueMaxPushId(authorizedPushId_ + 1)) {
            break;
        }
        ++authorizedPushId_;
        --pendingPushCredits_;
        progress = true;
    }
    return progress;
}
}  // namespace ruvia::detail
