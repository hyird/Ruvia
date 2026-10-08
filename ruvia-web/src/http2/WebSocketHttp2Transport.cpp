#include "http2/WebSocketHttp2Transport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "ruvia/http/WebSocketClientNegotiation.h"

#include "client/WebSocketClientState.h"

namespace ruvia::detail {

WebSocketHttp2Transport::WebSocketHttp2Transport(WebSocketClientState& owner,
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : owner_(owner),
      connection_(ruvia::Http2Connection::client({.resource = resource})),
      drivers_(worker, {.resource = resource}),
      progress_(worker),
      writerWake_(worker),
      received_(resource) {}

Task<void> WebSocketHttp2Transport::connect() {
    drivers_.spawn(runReader());
    drivers_.spawn(runWriter());
    writerWake_.notify();
    while (!connection_.receivedPeerSettings()) {
        checkFailure();
        if (eof_) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                "upstream closed before HTTP/2 SETTINGS");
        }
        co_await progress_.wait();
    }
    checkFailure();
    auto* resource = received_.get_allocator().resource();
    std::pmr::vector<HttpHeaderView> headers(resource);
    headers.reserve(owner_.config_.headers.size() + 1);
    for (const auto& header : owner_.config_.headers) {
        headers.emplace_back(header.name, header.value);
    }
    if (!owner_.config_.userAgent.empty()) {
        headers.emplace_back("user-agent", owner_.config_.userAgent);
    }
    std::pmr::vector<std::string_view> subprotocols(resource);
    subprotocols.reserve(owner_.config_.subprotocols.size());
    for (const auto& value : owner_.config_.subprotocols) {
        subprotocols.emplace_back(value);
    }
    WebSocketClientNegotiation negotiation({.headers = headers,
                                               .subprotocols = subprotocols,
                                               .deflate = owner_.config_.deflate},
        resource);
    auto authority = clientUriHost(owner_.config_.host, resource);
    ClientPortTextBuffer portBuffer{};
    authority.append(":");
    authority.append(formatClientPort(owner_.port(), portBuffer));
    const auto submitted = negotiation.submitHttp2Request(connection_,
        owner_.config_.scheme == WebSocketScheme::kWss ? "https" : "http",
        authority, owner_.config_.target);
    if (!submitted.submitted()) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "could not open HTTP/2 WebSocket Extended CONNECT");
    }
    streamId_ = submitted.submitted()->streamId();
    writerWake_.notify();
    while (!response_) {
        checkFailure();
        if (eof_) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                "upstream closed before WebSocket handshake response");
        }
        co_await progress_.wait();
    }
    checkFailure();
    const auto negotiated = negotiation.validateResponse(*response_, !eof_);
    if (!negotiated) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "invalid HTTP/2 WebSocket handshake response");
    }
    owner_.selectedSubprotocol_.assign(negotiated->selectedSubprotocol);
    owner_.negotiatedCompression_ = negotiated->compression;
    response_.reset();
    co_await flush();
}

void WebSocketHttp2Transport::checkFailure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
    owner_.throwAbort();
}

void WebSocketHttp2Transport::fail(std::exception_ptr failure) noexcept {
    if (!stopped_ && !failure_) {
        failure_ = std::move(failure);
    }
    owner_.closeOnWorker(WebSocketClientState::AbortReason::kClosing);
}

void WebSocketHttp2Transport::stop() noexcept {
    stopped_ = true;
    drivers_.requestStop();
    progress_.notify();
    writerWake_.notify();
}

Task<void> WebSocketHttp2Transport::join() {
    co_await drivers_.join();
    receivedCredit_.reset();
    std::pmr::string(received_.get_allocator()).swap(received_);
}

void WebSocketHttp2Transport::drainEvents() {
    while (auto event = connection_.nextEvent()) {
        if (auto* head = event->responseHead()) {
            if (head->streamId() != streamId_ || response_) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "unexpected HTTP/2 WebSocket response head");
            }
            response_.emplace(std::move(*head).takeHead());
        } else if (auto* data = event->tunnelData()) {
            if (data->streamId() != streamId_) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "unexpected HTTP/2 WebSocket tunnel stream");
            }
            received_.append(data->bytes());
            auto credit = data->takeCredit();
            if (received_.empty()) {
                (void)connection_.acknowledge(std::move(credit));
                continue;
            }
            if (credit.valid()) {
                if (receivedCredit_) {
                    if (receivedCredit_->merge(std::move(credit)) != Http2ReceivedDataCreditMergeStatus::kMerged) {
                        std::terminate();
                    }
                } else {
                    receivedCredit_.emplace(std::move(credit));
                }
            }
        } else if (auto* end = event->tunnelEnd()) {
            if (end->streamId() == streamId_) {
                eof_ = true;
            }
        } else if (auto* messageEnd = event->messageEnd()) {
            if (messageEnd->streamId() == streamId_) {
                eof_ = true;
            }
        } else if (auto* reset = event->streamClosed()) {
            if (reset->streamId() == streamId_) {
                if (reset->source() == Http2StreamCloseSource::kPeer &&
                    reset->error() == Http2ErrorCode::kNoError) {
                    // NO_ERROR can retire a half-open Extended CONNECT after
                    // the peer's DATA. Deliver those bytes before EOF: only the
                    // WebSocket parser can prove that they contain peer Close.
                    clean_reset_ = true;
                    eof_ = true;
                    continue;
                }
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "upstream reset HTTP/2 WebSocket stream");
            }
        } else if (auto* unprocessed = event->requestUnprocessed()) {
            if (unprocessed->streamId() == streamId_) {
                throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                    "upstream did not process HTTP/2 WebSocket stream");
            }
        } else if (event->pushPromise()) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "unexpected HTTP/2 server push");
        }
    }
    writerWake_.notify();
    progress_.notify();
}

Task<void> WebSocketHttp2Transport::runReader() {
    std::array<char, 16384> bytes{};
    try {
        while (!stopped_) {
            const auto count = co_await owner_.readSocket(bytes);
            if (count == 0) {
                eof_ = true;
                progress_.notify();
                co_return;
            }
            const auto input = std::string_view(bytes.data(), count);
            for (;;) {
                const auto result = connection_.feed(input);
                drainEvents();
                if (result == Http2FeedResult::kProtocolFailure || connection_.connectionError()) {
                    throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                        "invalid HTTP/2 connection input");
                }
                if (result != Http2FeedResult::kEventsPending) {
                    break;
                }
            }
        }
    } catch (...) {
        fail(std::current_exception());
    }
}

Task<void> WebSocketHttp2Transport::runWriter() {
    std::pmr::string bytes(received_.get_allocator());
    WorkerTimerRegistration timer;
    try {
        while (!stopped_) {
            if (!connection_.wantsWrite()) {
                co_await writerWake_.wait();
                continue;
            }
            bytes.clear();
            (void)connection_.takeOutputBatch(16384, bytes);
            writerActive_ = true;
            owner_.arm(timer, owner_.config_.write_timeout, WebSocketClientState::AbortReason::kTimeout);
            co_await owner_.writeSocket(bytes);
            owner_.disarm(timer);
            writerActive_ = false;
            progress_.notify();
        }
    } catch (...) {
        owner_.disarm(timer);
        writerActive_ = false;
        fail(std::current_exception());
    }
}

Task<void> WebSocketHttp2Transport::flush() {
    writerWake_.notify();
    for (;;) {
        checkFailure();
        if (!connection_.wantsWrite() && !writerActive_ &&
            connection_.dataQueueState(streamId_) != Http2DataQueueState::kQueued) {
            co_return;
        }
        co_await progress_.wait();
    }
}

Task<std::size_t> WebSocketHttp2Transport::read(std::span<char> output) {
    for (;;) {
        checkFailure();
        const auto available = received_.size() - readOffset_;
        if (available != 0) {
            const auto count = std::min(output.size(), available);
            std::memcpy(output.data(), received_.data() + readOffset_, count);
            readOffset_ += count;
            if (readOffset_ == received_.size()) {
                received_.clear();
                readOffset_ = 0;
                receivedCredit_.reset();
                writerWake_.notify();
            }
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        co_await progress_.wait();
    }
}

Task<void> WebSocketHttp2Transport::write(std::string_view bytes) {
    while (!bytes.empty()) {
        checkFailure();
        if (connection_.streamAborted(streamId_)) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "HTTP/2 WebSocket stream is closed");
        }
        const auto count = std::min<std::size_t>(bytes.size(), 16384);
        const auto submitted = connection_.submitData(streamId_, bytes.substr(0, count), Http2EndStream::kKeepOpen);
        if (submitted == Http2DataSubmitStatus::kBackpressured) {
            writerWake_.notify();
            co_await progress_.wait();
            continue;
        }
        if (submitted != Http2DataSubmitStatus::kAccepted && submitted != Http2DataSubmitStatus::kQueued) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "could not submit HTTP/2 WebSocket DATA");
        }
        bytes.remove_prefix(count);
        co_await flush();
    }
}

Task<void> WebSocketHttp2Transport::finish() {
    checkFailure();
    if (clean_reset_) {
        // The peer already retired both HTTP/2 halves. The WebSocket caller
        // reaches finish only after parsing its protocol's transport-end plan.
        co_return;
    }
    const auto submitted = connection_.submitData(streamId_, {}, Http2EndStream::kEndStream);
    if (submitted != Http2DataSubmitStatus::kAccepted && submitted != Http2DataSubmitStatus::kQueued) {
        throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
            "could not conclude HTTP/2 WebSocket stream");
    }
    co_await flush();
}

}  // namespace ruvia::detail
