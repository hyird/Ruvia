#include "ruvia/web/detail/http3/Http3ClientResponseDelivery.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"

namespace ruvia::detail {

Http3ClientResponseEventSink Http3ClientResponseDelivery::eventSink() noexcept {
    return {.callback = onEvent, .context = this};
}

Http3ClientResponseDelivery::ReadAllowance Http3ClientResponseDelivery::readAllowance(
    std::size_t maxInputBytes) const noexcept {
    if (state_.complete) {
        return {.status = ReadStatus::kTerminal};
    }
    if (retirementReason_ != RetirementReason::kNone) {
        return {.status = ReadStatus::kRetirementRequired};
    }
    if (maxInputBytes == 0) {
        return {.status = ReadStatus::kBackpressured};
    }
    const auto localCapacity = remainingBodyCapacity();
    const auto sharedCapacity = state_.producerBodyBudgetAvailable();
    const auto capacity = std::min({localCapacity, sharedCapacity, maxInputBytes});
    if (state_.collectAll && sharedCapacity == 0) {
        // One wire byte at a time lets the parser reach trailers or FIN without
        // admitting body storage past the shared cap. A DATA event with no
        // reservation is retired locally by deliver().
        return {.status = ReadStatus::kReady, .bytes = 1};
    }
    if (state_.collectAll && localCapacity <= sharedCapacity) {
        // Probe only when this response's own limit is binding. A partially
        // available shared budget must not be misreported as local overflow.
        return {.status = ReadStatus::kReady,
            .bytes = localCapacity < maxInputBytes ? localCapacity + 1 : maxInputBytes};
    }
    if (capacity == 0) {
        return {.status = ReadStatus::kBackpressured};
    }
    return {.status = ReadStatus::kReady, .bytes = capacity};
}

Http3ClientResponseDelivery::CommitStatus Http3ClientResponseDelivery::commit(
    const Http3ClientReceiveDriver::Result& result) noexcept {
    if (state_.complete) {
        return CommitStatus::kAlreadyCommitted;
    }
    if (result.status == Http3ClientReceiveDriver::Status::kConnectionError) {
        commitError(HttpClientError::Code::kProtocolError);
        return CommitStatus::kCommitted;
    }
    if (result.status == Http3ClientReceiveDriver::Status::kTransportError) {
        commitError(HttpClientError::Code::kIoError);
        return CommitStatus::kCommitted;
    }
    if (retirementReason_ != RetirementReason::kNone) {
        return CommitStatus::kRetirementRequired;
    }

    switch (result.status) {
        case Http3ClientReceiveDriver::Status::kBlocked:
        case Http3ClientReceiveDriver::Status::kProgress:
            return CommitStatus::kPending;
        case Http3ClientReceiveDriver::Status::kResponseComplete:
            return commitComplete();
        case Http3ClientReceiveDriver::Status::kStreamReset:
        case Http3ClientReceiveDriver::Status::kPeerStreamEnded:
        case Http3ClientReceiveDriver::Status::kStreamError:
            commitError(HttpClientError::Code::kProtocolError);
            return CommitStatus::kCommitted;
        case Http3ClientReceiveDriver::Status::kConnectionError:
        case Http3ClientReceiveDriver::Status::kTransportError:
            break;
    }
    return CommitStatus::kPending;
}

Http3ClientResponseDelivery::CommitStatus Http3ClientResponseDelivery::commitComplete() noexcept {
    if (state_.complete) {
        return CommitStatus::kAlreadyCommitted;
    }
    if (!state_.headReady || retirementReason_ != RetirementReason::kNone) {
        return CommitStatus::kRetirementRequired;
    }
    commitTerminal();
    return CommitStatus::kCommitted;
}

bool Http3ClientResponseDelivery::commitTerminalError(HttpClientError::Code error) noexcept {
    if (state_.complete) {
        return false;
    }
    commitError(error);
    return true;
}

bool Http3ClientResponseDelivery::commitRetirementFailure(
    HttpClientError::Code error) noexcept {
    if (state_.complete || retirementReason_ == RetirementReason::kNone) {
        return false;
    }
    if (state_.collectAll || state_.bodyDecodeRequired) {
        state_.discardResponseBody();
    } else {
        state_.discardPendingBody();
    }
    commitError(error);
    return true;
}

bool Http3ClientResponseDelivery::commitFailure(std::exception_ptr failure) noexcept {
    if (state_.complete) {
        return false;
    }
    if (failure == nullptr) {
        std::terminate();
    }
    if (state_.bodyDecodeRequired) {
        state_.discardResponseBody();
    } else {
        state_.discardPendingBody();
    }
    state_.failure = std::move(failure);
    commitTerminal();
    return true;
}

void Http3ClientResponseDelivery::onEvent(void* context, const Http3ConnectionEvent& event) {
    auto& self = *static_cast<Http3ClientResponseDelivery*>(context);
    try {
        self.deliver(event);
    } catch (...) {
        // An allocation failure is local to the response stream. Never let it
        // escape Http3Connection::feed(), which would fail sibling streams.
        if (self.callbackFailure_ == nullptr) {
            self.callbackFailure_ = std::current_exception();
        }
        self.requestRetirement(RetirementReason::kCallbackFailure);
    }
}

void Http3ClientResponseDelivery::deliver(const Http3ConnectionEvent& event) {
    if (state_.complete || retirementReason_ != RetirementReason::kNone) {
        return;
    }
    switch (event.kind) {
        case Http3ConnectionEventKind::kPushStream:
        case Http3ConnectionEventKind::kPushPromise:
        case Http3ConnectionEventKind::kPushCanceled:
        case Http3ConnectionEventKind::kPriorityUpdate:
        case Http3ConnectionEventKind::kOriginAdvertisement:
            return;
        case Http3ConnectionEventKind::kFinalHead: {
            if (event.head == nullptr || state_.headReady) {
                requestRetirement(RetirementReason::kProtocolError);
                return;
            }
            const auto status = HttpStatusCode::tryFromValue(event.head->status);
            if (!status) {
                requestRetirement(RetirementReason::kProtocolError);
                return;
            }
            if (!event.responseBodyPlan) {
                requestRetirement(RetirementReason::kProtocolError);
                return;
            }
            responseBodyPlan_ = event.responseBodyPlan;
            state_.responseBodyPlan = responseBodyPlan_;
            state_.headers.clear();
            state_.headers.reserve(event.head->headers.size());
            for (const auto& field : event.head->headers) {
                state_.headers.push_back(
                    HttpHeader::copyOf(field.name, field.value, state_.resource));
            }
            state_.status = *status;
            state_.protocolVersion = HttpProtocolVersion::kHttp3;
            if (state_.tunnel) {
                state_.tunnel->accepted = responseBodyPlan_->contentSemantics() == HttpResponseContentSemantics::kConnectTunnel;
                if (state_.tunnel->accepted && state_.tunnel->udp) {
                    std::pmr::vector<HttpHeaderView> fields(state_.resource);
                    for (const auto& field : state_.headers) {
                        fields.emplace_back(field.name(), field.value());
                    }
                    if (!validateHttpConnectUdpResponse(state_.protocolVersion, state_.status.value(), fields)) {
                        requestRetirement(RetirementReason::kProtocolError);
                        return;
                    }
                }
                if (!state_.tunnel->accepted) {
                    state_.tunnel->stop();
                }
            }
            if (!state_.tunnel || !state_.tunnel->accepted) {
                configureHttpClientResponseDecoding(state_);
            }
            state_.headReady = true;
            state_.headSignal.notify();
            return;
        }
        case Http3ConnectionEventKind::kBody:
        case Http3ConnectionEventKind::kTunnelData:
            if (event.kind == Http3ConnectionEventKind::kTunnelData ? (!state_.tunnel || !state_.tunnel->accepted || !responseBodyPlan_ || responseBodyPlan_->contentSemantics() != HttpResponseContentSemantics::kConnectTunnel) : (responseBodyPlan_ && (responseBodyPlan_->bodySuppressed() || !responseBodyPlan_->statusAllowsBody() || responseBodyPlan_->contentSemantics() != HttpResponseContentSemantics::kWithContent))) {
                requestRetirement(RetirementReason::kProtocolError);
                return;
            }
            if (event.body.size() > remainingBodyCapacity() ||
                !state_.retainProducerBodyBytes(event.body.size())) {
                requestRetirement(RetirementReason::kResponseTooLarge);
                return;
            }
            if (!event.body.empty()) {
                try {
                    state_.pending.append(event.body.data(), event.body.size());
                } catch (...) {
                    state_.releaseProducerBodyBytes(event.body.size());
                    throw;
                }
                state_.dataSignal.notify();
            }
            return;
        case Http3ConnectionEventKind::kTrailerField:
            state_.trailers.push_back(HttpHeader::copyOf(
                event.trailer.name, event.trailer.value, state_.resource));
            return;
        case Http3ConnectionEventKind::kMessageEnd:
            if (event.responseBodyPlan) {
                responseBodyPlan_ = event.responseBodyPlan;
            }
            return;
        case Http3ConnectionEventKind::kInformationalHead: {
            if (event.head == nullptr) {
                requestRetirement(RetirementReason::kProtocolError);
                return;
            }
            std::pmr::vector<HttpHeaderView> fields(state_.resource);
            for (const auto& field : event.head->headers) {
                fields.emplace_back(field.name, field.value);
            }
            state_.retainInformational(HttpStatusCode::fromValue(event.head->status), fields);
            return;
        }
        case Http3ConnectionEventKind::kRequestHead:
        case Http3ConnectionEventKind::kReset:
            return;
    }
}

void Http3ClientResponseDelivery::commitError(HttpClientError::Code error) noexcept {
    if (state_.bodyDecodeRequired) {
        state_.discardResponseBody();
    }
    state_.errorCode = static_cast<std::uint8_t>(error);
    commitTerminal();
}

void Http3ClientResponseDelivery::commitTerminal() noexcept {
    state_.complete = true;
    state_.headSignal.notify();
    state_.dataSignal.notify();
}

void Http3ClientResponseDelivery::requestRetirement(RetirementReason reason) noexcept {
    if (retirementReason_ == RetirementReason::kNone) {
        retirementReason_ = reason;
    }
}

void Http3ClientResponseDelivery::reconcileBodyBytes() noexcept {
    state_.reconcileProducerBodyBytes();
}

std::size_t Http3ClientResponseDelivery::remainingBodyCapacity() const noexcept {
    const auto limit = state_.bufferedLimit;
    const auto retained = state_.producerBodyBytes();
    if (retained >= limit) {
        return 0;
    }
    return limit - retained;
}

}  // namespace ruvia::detail
