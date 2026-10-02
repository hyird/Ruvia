#include "ruvia/web/detail/http3/Http3SansIoSessionEngine.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/router/RouteEndpoint.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/request/RequestBodyLimit.h"

namespace ruvia::detail {

struct Http3SansIoSessionEngine::Stream final {
    Stream(WorkerMemory& worker, std::uint64_t streamId, std::pmr::memory_resource* inbound_pool)
        : id(streamId),
          memory(worker),
          trailers(memory.resource()),
          tunnelInput(inbound_pool) {}

    const std::uint64_t id;
    std::optional<std::uint64_t> pushId{};
    // RequestMemory must outlive the head, request and body that use its arena.
    RequestMemory memory;
    HttpRequestTrailers trailers;
    std::optional<HttpPriority> priorityUpdate{};
    std::optional<Http3ServerRequest> request;
    RouteResolution resolution;
    StreamState state{StreamState::kReceiving};
    Rejection rejection{Rejection::kNone};
    std::size_t bodyLimit{};
    std::size_t receivedBodyBytes{};
    bool streamingBody{};
    Rejection bodyFailure{Rejection::kNone};
    std::pmr::string tunnelInput;
    std::optional<std::pmr::deque<std::pmr::string>> datagrams;
    std::size_t tunnelInputReadOffset{};
    std::size_t tunnelBufferedBytes{};
    bool connectRequest{};
    bool tunnelInputOverflow{};
    bool tunnelReceiveEnded{};
    bool tunnelReset{};
    bool pendingFinish{false};
    bool receiveEnded{false};
    bool leased{false};
    bool dispatched{false};
    bool retired{false};
};

void Http3SansIoSessionEngine::StreamDeleter::operator()(Stream* stream) const noexcept {
    if (stream == nullptr) {
        return;
    }
    std::pmr::polymorphic_allocator<Stream> allocator(resource);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, stream);
    allocator.deallocate(stream, 1);
}

Http3SansIoSessionEngine::Http3SansIoSessionEngine(const RouteTable& routes,
    WorkerMemory& worker, Http3SansIoSessionLimits limits)
    : routes_(routes),
      worker_(worker),
      limits_(limits),
      inbound_buffers_(limits.inbound_buffer_pool != nullptr ? limits.inbound_buffer_pool : worker.resource(),
          limits.max_inbound_buffer_bytes),
      connection_(Http3PeerRole::kServer, worker.resource(), limits.connection),
      controlOutput_(worker.resource()),
      streams_(worker.resource()) {
    if (limits_.maxBufferedBodyBytes == 0 || limits_.maxLiveStreams == 0 ||
        limits_.maxBufferedBytesInFlight == 0 || limits_.maxTunnelBufferedBytes == 0 || limits_.maxStreamBacklogBytes < 4096) {
        throw std::invalid_argument("HTTP/3 session limits must be greater than zero");
    }
}

Http3SansIoSessionEngine::Http3SansIoSessionEngine(const RouteTable& routes,
    WorkerMemory& worker, Http3ServerBodyBudget& bodyBudget, Http3SansIoSessionLimits limits)
    : Http3SansIoSessionEngine(routes, worker, limits) {
    bodyBudget_ = &bodyBudget;
}

Http3SansIoSessionEngine::~Http3SansIoSessionEngine() {
    if (activeLeases_ != 0) {
        std::terminate();
    }
    if (bodyBudget_ != nullptr) {
        streams_.clear();
        bodyBudget_->release(bufferedBytesInFlight_ + tunnelBytesInFlight_);
        bufferedBytesInFlight_ = 0;
        tunnelBytesInFlight_ = 0;
    }
}

Http3SansIoSessionEngine::RequestLease::~RequestLease() {
    if (owner_) {
        owner_->releaseLease(*stream_);
    }
}

Http3SansIoSessionEngine::RequestLease::RequestLease(RequestLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      stream_(std::exchange(other.stream_, nullptr)) {}

const Http3ServerRequest& Http3SansIoSessionEngine::RequestLease::request() const& noexcept {
    if (!owner_) {
        std::terminate();
    }
    return *stream_->request;
}

const RouteResolution& Http3SansIoSessionEngine::RequestLease::resolution() const& noexcept {
    if (!owner_) {
        std::terminate();
    }
    return stream_->resolution;
}

std::optional<Http3SansIoSessionEngine::RequestLease> Http3SansIoSessionEngine::acquireRequest(
    std::uint64_t streamId) & noexcept {
    const auto found = streams_.find(streamId);
    if (terminated_ || found == streams_.end() || found->second->dispatched ||
        found->second->retired || found->second->state != StreamState::kReady) {
        return std::nullopt;
    }
    found->second->leased = true;
    found->second->dispatched = true;
    ++activeLeases_;
    return RequestLease(*this, *found->second);
}

std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3SansIoSessionEngine::admitPushStream(
    std::uint64_t streamId, std::uint64_t pushId) {
    const auto* head = connection_.promisedRequest(pushId);
    if (terminated_ || head == nullptr || streams_.contains(streamId) || streams_.size() >= limits_.maxLiveStreams) {
        return std::unexpected(Http3ConnectionErrorCode::kRequestRejected);
    }
    std::pmr::polymorphic_allocator<Stream> allocator(worker_.resource());
    auto* rawStream = allocator.allocate(1);
    try {
        allocator.construct(rawStream, worker_, streamId, &inbound_buffers_);
    } catch (...) {
        allocator.deallocate(rawStream, 1);
        throw;
    }
    StreamPtr stream(rawStream, StreamDeleter{worker_.resource()});
    stream->pushId = pushId;
    stream->request.emplace(*head, stream->memory.resource(), &inbound_buffers_);
    stream->request->finishBody();
    const auto& request = stream->request->request();
    const bool webSocketConnect = request.knownMethod() == HttpKnownMethod::kConnect &&
                                  httpAsciiEqualsIgnoreCase(stream->request->extendedConnectProtocol(), "websocket");
    stream->resolution = request.knownMethod() == HttpKnownMethod::kUnknown
                             ? routes_.resolveExtensionMethod(request.method(), request.path())
                             : routes_.resolve(webSocketConnect ? HttpKnownMethod::kGet : request.knownMethod(), request.path());
    stream->state = StreamState::kReady;
    stream->receiveEnded = true;
    auto [inserted, fresh] = streams_.emplace(streamId, std::move(stream));
    if (!fresh) {
        std::terminate();
    }
    try {
        auto prefix = connection_.preparePushStream(streamId, pushId);
        if (!prefix) {
            streams_.erase(inserted);
        }
        return prefix;
    } catch (...) {
        streams_.erase(inserted);
        throw;
    }
}

void Http3SansIoSessionEngine::observePushCancellation(void* context, void (*cancel)(void*, std::uint64_t) noexcept) {
    if (pushCancellation_ != nullptr || context == nullptr || cancel == nullptr) {
        throw std::invalid_argument("HTTP/3 push cancellation requires a single stable owner");
    }
    pushCancellationContext_ = context;
    pushCancellation_ = cancel;
}

void Http3SansIoSessionEngine::releaseLease(Stream& stream) noexcept {
    if (!stream.leased || activeLeases_ == 0) {
        std::terminate();
    }
    stream.leased = false;
    --activeLeases_;
    if (stream.retired) {
        releaseStream(stream.id);
    }
}

void Http3SansIoSessionEngine::bindControlOutputWake(void* context, void (*wake)(void*) noexcept) {
    if ((context == nullptr) != (wake == nullptr) || controlWake_ != nullptr) {
        throw std::invalid_argument("HTTP/3 control output requires one stable wake owner");
    }
    controlWakeContext_ = context;
    controlWake_ = wake;
}
bool Http3SansIoSessionEngine::queueOriginAdvertisement(std::span<const std::string_view> origins) {
    if (terminated_) {
        return false;
    }
    auto frame = connection_.prepareOriginAdvertisement(origins);
    if (!frame || frame->size() > kMaxHttpHeaderBytes - std::min(controlOutput_.size(), kMaxHttpHeaderBytes)) {
        return false;
    }
    controlOutput_.append(frame->data(), frame->size());
    if (controlWake_ != nullptr) {
        controlWake_(controlWakeContext_);
    }
    return true;
}
bool Http3SansIoSessionEngine::consumeControlOutput(std::size_t bytes) noexcept {
    if (bytes > controlOutput_.size()) {
        return false;
    }
    controlOutput_.erase(0, bytes);
    if (controlOutput_.empty()) {
        std::pmr::string(worker_.resource()).swap(controlOutput_);
    }
    return true;
}

Http3ConnectionResult Http3SansIoSessionEngine::feed(std::uint64_t streamId,
    std::string_view bytes, bool fin, bool reset) noexcept {
    if (terminated_) {
        return failure_;
    }
    try {
        const auto input = std::span<const char>(bytes.data(), bytes.size());
        const auto result = connection_.feed(
            streamId, input, fin, reset, &Http3SansIoSessionEngine::onConnectionEvent, this);
        if (result.scope == Http3ConnectionErrorScope::kConnection ||
            result.status == Http3ConnectionStatus::kConnectionError) {
            terminate(result);
            return result;
        }
        if (result.scope == Http3ConnectionErrorScope::kStream || reset ||
            result.status == Http3ConnectionStatus::kReset) {
            // The core has retired this receive stream. Do not retain its Web
            // body or headers while unrelated requests continue on this QUIC
            // connection. RESET after request FIN need not emit a core event;
            // it still retires Web state, deferring destruction for a lease.
            releaseStream(streamId);
            return result;
        }
        // The callback only marked a candidate. Publish it after this stream's
        // feed returned successfully, without walking unrelated live streams.
        const auto found = streams_.find(streamId);
        if (found != streams_.end() && found->second->pendingFinish &&
            found->second->state == StreamState::kReceiving) {
            found->second->request->finishBody();
            found->second->state = StreamState::kReady;
            found->second->pendingFinish = false;
        }
        return result;
    } catch (const std::length_error&) {
        const Http3ConnectionResult failure{Http3ConnectionStatus::kConnectionError,
            Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kExcessiveLoad};
        terminate(failure);
        return failure;
    } catch (...) {
        const Http3ConnectionResult failure{Http3ConnectionStatus::kConnectionError,
            Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kInternalError};
        terminate(failure);
        return failure;
    }
}

void Http3SansIoSessionEngine::onConnectionEvent(void* context,
    const Http3ConnectionEvent& event) {
    static_cast<Http3SansIoSessionEngine*>(context)->handleEvent(event);
}

void Http3SansIoSessionEngine::handleEvent(const Http3ConnectionEvent& event) {
    if (event.kind == Http3ConnectionEventKind::kPushCanceled) {
        if (event.pushId && pushCancellation_ != nullptr) {
            pushCancellation_(pushCancellationContext_, *event.pushId);
        }
        return;
    }
    if (event.kind == Http3ConnectionEventKind::kPriorityUpdate) {
        if (event.priorityUpdate && !event.priorityUpdate->push) {
            if (const auto found = streams_.find(event.priorityUpdate->elementId); found != streams_.end()) {
                found->second->priorityUpdate = event.priorityUpdate->fields.requestPriority();
            }
        } else if (event.priorityUpdate) {
            for (auto& [id, stream] : streams_) {
                if (stream->pushId == event.priorityUpdate->elementId) {
                    stream->priorityUpdate = event.priorityUpdate->fields.requestPriority();
                    break;
                }
            }
        }
        return;
    }
    if (event.kind == Http3ConnectionEventKind::kReset) {
        if (const auto found = streams_.find(event.streamId); found != streams_.end()) {
            found->second->tunnelReset = true;
        }
        releaseStream(event.streamId);
        return;
    }
    if (event.kind == Http3ConnectionEventKind::kRequestHead) {
        if (event.head == nullptr || streams_.contains(event.streamId)) {
            throw std::logic_error("invalid or duplicate HTTP/3 request head event");
        }
        if (streams_.size() >= limits_.maxLiveStreams) {
            throw std::length_error("HTTP/3 live stream budget exhausted");
        }
        std::pmr::polymorphic_allocator<Stream> allocator(worker_.resource());
        auto* rawStream = allocator.allocate(1);
        try {
            allocator.construct(rawStream, worker_, event.streamId, &inbound_buffers_);
        } catch (...) {
            allocator.deallocate(rawStream, 1);
            throw;
        }
        StreamPtr stream(rawStream, StreamDeleter{worker_.resource()});
        stream->request.emplace(*event.head, stream->memory.resource(),
            &inbound_buffers_);
        const auto& request = stream->request->request();
        stream->connectRequest = request.knownMethod() == HttpKnownMethod::kConnect;
        const bool webSocketConnect = stream->connectRequest &&
                                      httpAsciiEqualsIgnoreCase(stream->request->extendedConnectProtocol(), "websocket");
        if (stream->connectRequest && !webSocketConnect) {
            stream->resolution = routes_.resolveConnect(stream->request->extendedConnectProtocol(),
                stream->request->extendedConnectProtocol().empty() ? request.authority() : request.path());
        } else if (request.knownMethod() == HttpKnownMethod::kUnknown) {
            stream->resolution = routes_.resolveExtensionMethod(request.method(), request.path());
        } else {
            stream->resolution = routes_.resolve(webSocketConnect ? HttpKnownMethod::kGet : request.knownMethod(), request.path());
        }
        stream->bodyLimit = requestBodyByteLimit(RequestBodyMode::kBuffered,
            std::nullopt, limits_.maxBufferedBodyBytes)
                                .readCeiling();

        const auto expectation = stream->request->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
        if (expectation.rejection() != nullptr) {
            stream->state = StreamState::kRejected;
            stream->rejection = Rejection::kExpectationUnsupported;
        } else if (stream->connectRequest) {
            const auto* resolved = stream->resolution.resolved();
            if (resolved == nullptr ||
                (resolved->route().endpoint().tunnel() == nullptr && resolved->route().endpoint().webSocket() == nullptr)) {
                stream->state = StreamState::kRejected;
                stream->rejection = Rejection::kConnectUnsupported;
            } else {
                stream->state = StreamState::kReady;
            }
        } else if (const auto* resolved = stream->resolution.resolved()) {
            const auto& endpoint = resolved->route().endpoint();
            if (endpoint.webSocket() != nullptr) {
                stream->state = StreamState::kRejected;
                stream->rejection = Rejection::kWebSocketUnsupported;
            } else if (const auto* buffered = endpoint.buffered()) {
                stream->streamingBody = buffered->requestBodyMode() == RequestBodyMode::kStream;
                stream->bodyLimit = requestBodyByteLimit(buffered->requestBodyMode(),
                    limits_.maxStreamBodyBytes, limits_.maxBufferedBodyBytes,
                    resolved->route().maxRequestBodyBytes())
                                        .readCeiling();
                if (stream->streamingBody) {
                    stream->state = StreamState::kReady;
                }
            }
        }
        if (stream->state != StreamState::kRejected && !stream->connectRequest &&
            event.head->contentLength.has_value() &&
            *event.head->contentLength > stream->bodyLimit) {
            stream->state = StreamState::kRejected;
            stream->rejection = Rejection::kBodyTooLarge;
        }
        streams_.emplace(event.streamId, std::move(stream));
        return;
    }

    const auto found = streams_.find(event.streamId);
    if (found == streams_.end()) {
        return;
    }
    auto& stream = *found->second;
    if (stream.state == StreamState::kRejected) {
        if (event.kind == Http3ConnectionEventKind::kMessageEnd) {
            stream.receiveEnded = true;
        }
        return;
    }
    switch (event.kind) {
        case Http3ConnectionEventKind::kBody: {
            if (stream.streamingBody) {
                if (stream.bodyFailure != Rejection::kNone) {
                    return;
                }
                if (event.body.size() > stream.bodyLimit - stream.receivedBodyBytes) {
                    stream.bodyFailure = Rejection::kBodyTooLarge;
                    return;
                }
                stream.receivedBodyBytes += event.body.size();
                if (stream.tunnelBufferedBytes > limits_.maxStreamBacklogBytes ||
                    event.body.size() > limits_.maxStreamBacklogBytes - stream.tunnelBufferedBytes ||
                    bufferedBytesInFlight_ > limits_.maxBufferedBytesInFlight ||
                    tunnelBytesInFlight_ > limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ ||
                    event.body.size() > limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ - tunnelBytesInFlight_ ||
                    (bodyBudget_ != nullptr && !bodyBudget_->tryReserve(event.body.size()))) {
                    stream.bodyFailure = Rejection::kInFlightBodyCapacity;
                    return;
                }
                try {
                    if (stream.tunnelInputReadOffset != 0) {
                        stream.tunnelInput.erase(0, stream.tunnelInputReadOffset);
                        stream.tunnelInputReadOffset = 0;
                    }
                    stream.tunnelInput.append(event.body.data(), event.body.size());
                } catch (const inbound_buffer_limit_error&) {
                    if (bodyBudget_ != nullptr) {
                        bodyBudget_->release(event.body.size());
                    }
                    stream.bodyFailure = Rejection::kWorkerBodyBudgetExhausted;
                    return;
                } catch (...) {
                    if (bodyBudget_ != nullptr) {
                        bodyBudget_->release(event.body.size());
                    }
                    throw;
                }
                stream.tunnelBufferedBytes += event.body.size();
                tunnelBytesInFlight_ += event.body.size();
                return;
            }
            if (stream.request == std::nullopt || stream.state != StreamState::kReceiving) {
                throw std::logic_error("HTTP/3 body arrived outside a buffered request");
            }
            const auto current = stream.request->bodyBytes();
            if (current > stream.bodyLimit || event.body.size() > stream.bodyLimit - current) {
                rejectBody(stream, Rejection::kBodyTooLarge);
                return;
            }
            if (bufferedBytesInFlight_ > limits_.maxBufferedBytesInFlight ||
                tunnelBytesInFlight_ > limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ ||
                event.body.size() > limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ - tunnelBytesInFlight_) {
                rejectBody(stream, Rejection::kInFlightBodyCapacity);
                return;
            }
            if (bodyBudget_ != nullptr && !bodyBudget_->tryReserve(event.body.size())) {
                rejectBody(stream, Rejection::kWorkerBodyBudgetExhausted);
                return;
            }
            try {
                stream.request->appendBody(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.body.data()), event.body.size()));
            } catch (const inbound_buffer_limit_error&) {
                if (bodyBudget_ != nullptr) {
                    bodyBudget_->release(event.body.size());
                }
                rejectBody(stream, Rejection::kWorkerBodyBudgetExhausted);
                return;
            } catch (...) {
                if (bodyBudget_ != nullptr) {
                    bodyBudget_->release(event.body.size());
                }
                throw;
            }
            bufferedBytesInFlight_ += event.body.size();
            return;
        }
        case Http3ConnectionEventKind::kTrailerField:
            if (!stream.trailers.append(event.trailer.name, event.trailer.value)) {
                throw std::logic_error("HTTP/3 decoder published invalid request trailers");
            }
            return;
        case Http3ConnectionEventKind::kTunnelData: {
            if (!stream.connectRequest || stream.tunnelInputOverflow ||
                stream.tunnelReceiveEnded) {
                return;
            }
            const auto queued = stream.tunnelBufferedBytes;
            if (queued > limits_.maxTunnelBufferedBytes ||
                event.body.size() > limits_.maxTunnelBufferedBytes - queued ||
                bufferedBytesInFlight_ > limits_.maxBufferedBytesInFlight ||
                tunnelBytesInFlight_ >
                    limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ ||
                event.body.size() > limits_.maxBufferedBytesInFlight -
                                        bufferedBytesInFlight_ - tunnelBytesInFlight_) {
                stream.tunnelInputOverflow = true;
                return;
            }
            if (bodyBudget_ != nullptr && !bodyBudget_->tryReserve(event.body.size())) {
                stream.tunnelInputOverflow = true;
                return;
            }
            try {
                if (stream.tunnelInputReadOffset != 0 &&
                    stream.tunnelInput.capacity() - stream.tunnelInput.size() < event.body.size()) {
                    stream.tunnelInput.erase(0, stream.tunnelInputReadOffset);
                    stream.tunnelInputReadOffset = 0;
                }
                stream.tunnelInput.append(event.body.data(), event.body.size());
            } catch (...) {
                if (bodyBudget_ != nullptr) {
                    bodyBudget_->release(event.body.size());
                }
                stream.tunnelInputOverflow = true;
                return;
            }
            stream.tunnelBufferedBytes += event.body.size();
            tunnelBytesInFlight_ += event.body.size();
            return;
        }
        case Http3ConnectionEventKind::kMessageEnd:
            stream.receiveEnded = true;
            if (stream.connectRequest || stream.streamingBody) {
                stream.tunnelReceiveEnded = true;
                if (stream.streamingBody) {
                    stream.request->finishBody();
                }
            } else {
                stream.pendingFinish = stream.state == StreamState::kReceiving;
            }
            return;
        case Http3ConnectionEventKind::kPushStream:
        case Http3ConnectionEventKind::kPushPromise:
        case Http3ConnectionEventKind::kPushCanceled:
        case Http3ConnectionEventKind::kOriginAdvertisement:
        case Http3ConnectionEventKind::kPriorityUpdate:
        case Http3ConnectionEventKind::kInformationalHead:
        case Http3ConnectionEventKind::kFinalHead:
        case Http3ConnectionEventKind::kRequestHead:
        case Http3ConnectionEventKind::kReset:
            return;
    }
}

const Http3ServerRequest* Http3SansIoSessionEngine::request(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() || !found->second->request.has_value()
               ? nullptr
               : &*found->second->request;
}

const RouteResolution* Http3SansIoSessionEngine::resolution(
    std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? nullptr : &found->second->resolution;
}

Http3SansIoSessionEngine::StreamState Http3SansIoSessionEngine::streamState(
    std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? StreamState::kRejected : found->second->state;
}

Http3SansIoSessionEngine::Rejection Http3SansIoSessionEngine::rejection(
    std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? Rejection::kNone : found->second->rejection;
}

const HttpRequestTrailers* Http3SansIoSessionEngine::requestTrailers(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? nullptr : &found->second->trailers;
}
const std::optional<HttpPriority>* Http3SansIoSessionEngine::requestPriorityUpdate(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? nullptr : &found->second->priorityUpdate;
}

bool Http3SansIoSessionEngine::streamingRequest(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found != streams_.end() && found->second->streamingBody;
}

bool Http3SansIoSessionEngine::canAcceptInput(std::uint64_t streamId, std::size_t wireBytes) const noexcept {
    const auto found = streams_.find(streamId);
    if (found == streams_.end() || !found->second->streamingBody || found->second->retired || found->second->bodyFailure != Rejection::kNone) {
        return true;
    }
    return found->second->tunnelBufferedBytes <= limits_.maxStreamBacklogBytes &&
           bufferedBytesInFlight_ <= limits_.maxBufferedBytesInFlight &&
           tunnelBytesInFlight_ <= limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ &&
           wireBytes <= limits_.maxStreamBacklogBytes - found->second->tunnelBufferedBytes &&
           wireBytes <= limits_.maxBufferedBytesInFlight - bufferedBytesInFlight_ - tunnelBytesInFlight_ &&
           (bodyBudget_ == nullptr || wireBytes <= bodyBudget_->available());
}

Http3SansIoSessionEngine::Rejection Http3SansIoSessionEngine::streamingBodyFailure(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() ? Rejection::kNone : found->second->bodyFailure;
}

Http3SansIoSessionEngine::TunnelReadResult Http3SansIoSessionEngine::readTunnelData(
    std::uint64_t streamId, std::span<char> output) noexcept {
    const auto found = streams_.find(streamId);
    if (found == streams_.end()) {
        return {.reset = true};
    }
    auto& stream = *found->second;
    const auto available = stream.tunnelInput.size() - stream.tunnelInputReadOffset;
    const auto count = (std::min)(available, output.size());
    if (count != 0) {
        std::memcpy(output.data(), stream.tunnelInput.data() + stream.tunnelInputReadOffset, count);
        stream.tunnelInputReadOffset += count;
        if (count > stream.tunnelBufferedBytes || count > tunnelBytesInFlight_) {
            std::terminate();
        }
        stream.tunnelBufferedBytes -= count;
        tunnelBytesInFlight_ -= count;
        if (bodyBudget_ != nullptr) {
            bodyBudget_->release(count);
        }
        if (stream.tunnelInputReadOffset == stream.tunnelInput.size()) {
            std::pmr::string(stream.tunnelInput.get_allocator()).swap(stream.tunnelInput);
            stream.tunnelInputReadOffset = 0;
        }
    }
    return {.bytes = count,
        .ended = stream.tunnelReceiveEnded,
        .reset = stream.tunnelReset,
        .overflow = stream.tunnelInputOverflow};
}

Http3DatagramReceiveStatus Http3SansIoSessionEngine::receiveDatagram(Http3DatagramView datagram) {
    auto found = streams_.find(datagram.streamId);
    auto* stream = found == streams_.end() ? nullptr : found->second.get();
    const auto* resolved = stream ? stream->resolution.resolved() : nullptr;
    const auto* tunnel = resolved ? resolved->route().endpoint().tunnel() : nullptr;
    const auto status = planHttp3DatagramReceive(datagram, {.localH3Datagram = limits_.connection.enableDatagrams,
                                                               .streamExists = stream && stream->request.has_value(),
                                                               .receiveOpen = stream && !stream->retired && !stream->receiveEnded && !stream->tunnelReceiveEnded && !stream->tunnelReset,
                                                               .supportsDatagrams = tunnel && tunnel->config().datagrams});
    if (status == Http3DatagramReceiveStatus::kDeliver && (!stream->datagrams || stream->datagrams->size() < 16)) {
        // Store the decoded opaque value. Quarter Stream ID is already validated
        // by the network/connection owner and is reconstructed for the adapter.
        std::array<char, 8> prefix{};
        auto size = encodeHttp3DatagramPrefix(prefix, datagram.streamId);
        if (!size) {
            throw std::runtime_error("invalid HTTP Datagram stream ID");
        }
        std::pmr::string bytes(prefix.data(), *size, worker_.resource());
        bytes.append(datagram.payload.data(), datagram.payload.size());
        if (!stream->datagrams) {
            stream->datagrams.emplace(worker_.resource());
        }
        stream->datagrams->push_back(std::move(bytes));
    }
    return status;
}
std::optional<std::pmr::string> Http3SansIoSessionEngine::takeDatagram(std::uint64_t streamId) {
    const auto found = streams_.find(streamId);
    if (found == streams_.end() || (!found->second->datagrams || found->second->datagrams->empty())) {
        return std::nullopt;
    }
    auto bytes = std::move(found->second->datagrams->front());
    found->second->datagrams->pop_front();
    return bytes;
}
HttpDatagramSessionConfig Http3SansIoSessionEngine::datagramConfig(std::uint64_t streamId) const {
    return {.http3StreamId = streamId, .localH3Datagram = limits_.connection.enableDatagrams, .peerH3Datagram = connection_.peerSettings() && connection_.peerSettings()->h3Datagram, .quicDatagram = limits_.maxQuicDatagramPayloadBytes != 0, .maxQuicPayloadBytes = limits_.maxQuicDatagramPayloadBytes};
}
bool Http3SansIoSessionEngine::tunnelInputOverflowed(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found != streams_.end() && found->second->tunnelInputOverflow;
}

bool Http3SansIoSessionEngine::tunnelReceiveEnded(std::uint64_t streamId) const noexcept {
    const auto found = streams_.find(streamId);
    return found == streams_.end() || found->second->tunnelReceiveEnded ||
           found->second->tunnelReset;
}

std::optional<std::uint64_t>
Http3SansIoSessionEngine::peerMaxFieldSectionSize() const noexcept {
    const auto& settings = connection_.peerSettings();
    return settings.has_value() ? settings->maxFieldSectionSize : std::nullopt;
}

std::size_t Http3SansIoSessionEngine::activeStreamCount() const noexcept {
    return streams_.size();
}

bool Http3SansIoSessionEngine::terminated() const noexcept {
    return terminated_;
}

void Http3SansIoSessionEngine::releaseStream(std::uint64_t streamId) noexcept {
    const auto found = streams_.find(streamId);
    if (found == streams_.end()) {
        return;
    }
    if (found->second->leased) {
        found->second->retired = true;
        return;
    }
    if (found->second->request) {
        const auto bytes = found->second->request->bodyBytes();
        if (bytes > bufferedBytesInFlight_) {
            std::terminate();
        }
        bufferedBytesInFlight_ -= bytes;
        if (bodyBudget_ != nullptr) {
            bodyBudget_->release(bytes);
        }
    }
    if (found->second->tunnelBufferedBytes > tunnelBytesInFlight_) {
        std::terminate();
    }
    tunnelBytesInFlight_ -= found->second->tunnelBufferedBytes;
    if (bodyBudget_ != nullptr) {
        bodyBudget_->release(found->second->tunnelBufferedBytes);
    }
    streams_.erase(found);
}

void Http3SansIoSessionEngine::rejectBody(Stream& stream, Rejection reason) noexcept {
    const auto bytes = stream.request->bodyBytes();
    if (bytes > bufferedBytesInFlight_) {
        std::terminate();
    }
    bufferedBytesInFlight_ -= bytes;
    if (bodyBudget_ != nullptr) {
        bodyBudget_->release(bytes);
    }
    stream.request->abortBody();
    stream.state = StreamState::kRejected;
    stream.rejection = reason;
}

bool Http3SansIoSessionEngine::release(std::uint64_t streamId) noexcept {
    const auto found = streams_.find(streamId);
    if (found == streams_.end() || !found->second->receiveEnded || found->second->leased) {
        return false;
    }
    releaseStream(streamId);
    return true;
}

bool Http3SansIoSessionEngine::cancelRequest(std::uint64_t streamId) noexcept {
    if (terminated_) {
        return false;
    }
    const auto found = streams_.find(streamId);
    if (found != streams_.end() && found->second->retired) {
        return false;
    }
    const bool parserRetired = connection_.retireServerRequest(streamId);
    if (!parserRetired && (found == streams_.end() || !found->second->receiveEnded)) {
        return false;
    }
    releaseStream(streamId);
    return true;
}

void Http3SansIoSessionEngine::stop() noexcept {
    terminate({Http3ConnectionStatus::kConnectionError,
        Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kInternalError});
}

void Http3SansIoSessionEngine::terminate(Http3ConnectionResult failure) noexcept {
    if (terminated_) {
        return;
    }
    failure_ = failure;
    terminated_ = true;
    std::pmr::string(worker_.resource()).swap(controlOutput_);
    if (!connection_.retire()) {
        std::terminate();
    }
    for (auto iterator = streams_.begin(); iterator != streams_.end();) {
        const auto id = iterator->first;
        ++iterator;
        releaseStream(id);
    }
}

}  // namespace ruvia::detail
