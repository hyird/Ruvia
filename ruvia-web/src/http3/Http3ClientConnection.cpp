#include "ruvia/web/detail/http3/Http3ClientConnection.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"

namespace ruvia::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxBufferedConnectionBodyBytes = 64 * 1024 * 1024;

Clock::duration checkedTimeout(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max())) {
        throw std::invalid_argument("HTTP/3 connection timeout must be positive and representable");
    }
    return std::chrono::duration_cast<Clock::duration>(timeout);
}

std::optional<Clock::duration> checkedTimeout(
    std::optional<std::chrono::milliseconds> timeout) {
    return timeout ? std::optional{checkedTimeout(*timeout)} : std::nullopt;
}

Clock::time_point deadlineAfter(Clock::time_point now, Clock::duration timeout) noexcept {
    const auto latestStart = Clock::time_point::max() - timeout;
    return now > latestStart ? Clock::time_point::max() : now + timeout;
}

Http3ClientSansIoResponseLimits responseLimits(
    std::size_t maxRequests, std::size_t maxResponseBytes) {
    if (maxRequests == 0 || maxRequests > Http3QuicClientTransport::kMaxStreamsPerConnection ||
        maxResponseBytes == 0 || maxResponseBytes > kMaxBufferedConnectionBodyBytes) {
        throw std::invalid_argument("HTTP/3 connection request and body bounds must be positive");
    }
    const auto aggregate = maxResponseBytes > std::numeric_limits<std::size_t>::max() / maxRequests
                               ? std::numeric_limits<std::size_t>::max()
                               : maxResponseBytes * maxRequests;
    return {.maxLiveStreams = maxRequests,
        .maxBodyBytesPerStream = maxResponseBytes,
        .maxTotalBodyBytes = std::min(aggregate, kMaxBufferedConnectionBodyBytes),
        .connection = {.maxActiveStreams = maxRequests}};
}

std::pmr::memory_resource* requireResource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/3 connection requires worker-owned storage");
    }
    return resource;
}

HttpClientError::Code clientErrorCode(Http3ClientConnection::Outcome outcome) noexcept {
    using Outcome = Http3ClientConnection::Outcome;
    switch (outcome) {
        case Outcome::kCancelled:
            return HttpClientError::Code::kCancelled;
        case Outcome::kDeadline:
            return HttpClientError::Code::kTimeout;
        case Outcome::kConnectFailed:
            return HttpClientError::Code::kConnectFailed;
        case Outcome::kResponseTooLarge:
            return HttpClientError::Code::kResponseTooLarge;
        case Outcome::kResultBudgetExceeded:
            return HttpClientError::Code::kResultBudgetExceeded;
        case Outcome::kConnectionDraining:
        case Outcome::kQueueFull:
            return outcome == Outcome::kQueueFull ? HttpClientError::Code::kQueueFull
                                                  : HttpClientError::Code::kClosing;
        case Outcome::kInvalidRequest:
            return HttpClientError::Code::kInvalidRequest;
        case Outcome::kProtocolError:
        case Outcome::kRequestRejected:
            return HttpClientError::Code::kProtocolError;
        case Outcome::kTransportError:
        case Outcome::kPending:
        case Outcome::kComplete:
            return HttpClientError::Code::kIoError;
    }
    return HttpClientError::Code::kIoError;
}

Http3ClientConnection::Outcome outcomeForClientError(HttpClientError::Code error) noexcept {
    switch (error) {
        case HttpClientError::Code::kTimeout:
            return Http3ClientConnection::Outcome::kDeadline;
        case HttpClientError::Code::kCancelled:
            return Http3ClientConnection::Outcome::kCancelled;
        case HttpClientError::Code::kResponseTooLarge:
            return Http3ClientConnection::Outcome::kResponseTooLarge;
        case HttpClientError::Code::kResultBudgetExceeded:
            return Http3ClientConnection::Outcome::kResultBudgetExceeded;
        case HttpClientError::Code::kProtocolError:
        case HttpClientError::Code::kInvalidRequest:
            return Http3ClientConnection::Outcome::kProtocolError;
        case HttpClientError::Code::kQueueFull:
            return Http3ClientConnection::Outcome::kQueueFull;
        case HttpClientError::Code::kClosing:
            return Http3ClientConnection::Outcome::kConnectionDraining;
        case HttpClientError::Code::kConnectFailed:
            return Http3ClientConnection::Outcome::kConnectFailed;
        case HttpClientError::Code::kNotConfigured:
        case HttpClientError::Code::kResolveFailed:
        case HttpClientError::Code::kTlsFailed:
        case HttpClientError::Code::kProtocolUnavailable:
        case HttpClientError::Code::kIoError:
            return Http3ClientConnection::Outcome::kTransportError;
    }
    return Http3ClientConnection::Outcome::kTransportError;
}
}  // namespace

Http3ClientConnection::Http3ClientConnection(asio::io_context& io, const WorkerHandle& worker,
    TaskScope& poolTasks, Http3QuicClientTlsContext& tls, HttpOriginView origin,
    std::chrono::milliseconds connectTimeout, std::pmr::memory_resource* resource,
    std::size_t maxRequests, std::size_t maxResponseBytes,
    std::chrono::milliseconds idleTimeout, Http3ClientBodyBudget* receiveBodyBudget,
    std::optional<std::chrono::milliseconds> writeTimeout)
    : Http3ClientConnection(io, worker, poolTasks, tls, origin, connectTimeout, resource,
          maxRequests, maxResponseBytes, idleTimeout, receiveBodyBudget, writeTimeout, {}) {}

Http3ClientConnection::Http3ClientConnection(asio::io_context& io, const WorkerHandle& worker,
    TaskScope& poolTasks, Http3QuicClientTlsContext& tls, HttpOriginView origin,
    std::chrono::milliseconds connectTimeout,
    std::pmr::memory_resource* resource, std::size_t maxRequests, std::size_t maxResponseBytes,
    std::chrono::milliseconds idleTimeout, Http3ClientBodyBudget* receiveBodyBudget,
    std::optional<std::chrono::milliseconds> writeTimeout,
    LifecycleNotification lifecycleNotification)
    : ownerThread_(std::this_thread::get_id()),
      io_(io),
      worker_(worker),
      poolTasks_(poolTasks),
      tls_(tls),
      resource_(requireResource(resource)),
      host_(origin.host(), resource_),
      authority_(makeHttpOriginAuthority(origin, resource_)),
      port_(origin.port()),
      connectTimeout_(checkedTimeout(connectTimeout)),
      idleTimeout_(checkedTimeout(idleTimeout)),
      writeTimeout_(checkedTimeout(writeTimeout)),
      maxRequests_(maxRequests),
      maxResponseBytes_(maxResponseBytes),
      resolver_(io_, resource_),
      bodyBudget_(kMaxBufferedConnectionBodyBytes),
      receiveBodyBudget_(receiveBodyBudget == nullptr ? &bodyBudget_ : receiveBodyBudget),
      lifecycleNotification_(lifecycleNotification),
      responseEngine_(resource_, bodyBudget_, responseLimits(maxRequests, maxResponseBytes)),
      receiver_(responseEngine_),
      session_(nullptr, PmrObjectDeleter<Http3QuicClientSocketSession>{resource_}),
      receiveBodyBudgetWake_(*receiveBodyBudget_, onReceiveBodyBudgetReleased, this),
      requests_(resource_),
      peerStreams_(resource_) {
    if (!worker_.valid() || origin.scheme() != HttpScheme::kHttps || port_ == 0 ||
        (lifecycleNotification_.context == nullptr) !=
            (lifecycleNotification_.notify == nullptr)) {
        throw std::invalid_argument(
            "HTTP/3 connection requires a worker, HTTPS origin, and complete notification");
    }
    // URI authority retains IP-literal brackets. DNS/TLS receive only the
    // address, never brackets or the origin's port suffix.
    if (host_.front() == '[') {
        host_.erase(host_.size() - 1);
        host_.erase(0, 1);
    }
    peerStreams_.reserve(Http3QuicClientTransport::kMaxStreamsPerConnection);
}

Http3ClientConnection::~Http3ClientConnection() {
    if (std::this_thread::get_id() != ownerThread_ || running_ || starting_) {
        std::terminate();
    }
    for (const auto& request : requests_) {
        if (request.responseState_ != nullptr || request.delivery) {
            // A state contains a typed borrow to this connection. The lifecycle
            // owner must detach it before destroying the connection, even when
            // its body reservation happens to be empty.
            std::terminate();
        }
    }
    receiveBodyBudgetWake_.reset();
    bodyBudget_.release(retainedResultBodyBytes_);
}

void Http3ClientConnection::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        throw std::logic_error("HTTP/3 client connection must run on its owner worker");
    }
}

void Http3ClientConnection::onReceiveBodyBudgetReleased(void* context) noexcept {
    static_cast<Http3ClientConnection*>(context)->wakeReceiveDriver();
}

void Http3ClientConnection::onResponseEvent(void* context, const Http3ConnectionEvent& event) {
    auto& request = *static_cast<Request*>(context);
    if (request.responseState_ != nullptr) {
        switch (event.kind) {
            case Http3ConnectionEventKind::kPushPromise:
            case Http3ConnectionEventKind::kPushCanceled:
            case Http3ConnectionEventKind::kPriorityUpdate:
            case Http3ConnectionEventKind::kOriginAdvertisement:
                break;
            case Http3ConnectionEventKind::kInformationalHead:
            case Http3ConnectionEventKind::kFinalHead:
            case Http3ConnectionEventKind::kBody:
            case Http3ConnectionEventKind::kTunnelData:
            case Http3ConnectionEventKind::kTrailerField:
            case Http3ConnectionEventKind::kMessageEnd:
                request.responseState_->http3ResponseStarted = true;
                break;
            case Http3ConnectionEventKind::kRequestHead:
            case Http3ConnectionEventKind::kReset:
                break;
        }
    }
    if (request.delivery) {
        const auto sink = request.delivery->eventSink();
        sink.callback(sink.context, event);
    }
}

void Http3ClientConnection::wakeReceiveDriver() noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    if (session_) {
        session_->notifyWork();
    } else if (running_ || starting_) {
        resolver_.interrupt();
    }
}

Http3ClientConnection::RequestList::iterator Http3ClientConnection::find(RequestId id) noexcept {
    return std::find_if(requests_.begin(), requests_.end(),
        [id](const Request& request) { return request.id == id; });
}

Http3ClientConnection::RequestList::const_iterator Http3ClientConnection::find(RequestId id) const noexcept {
    return std::find_if(requests_.begin(), requests_.end(),
        [id](const Request& request) { return request.id == id; });
}

Http3ClientConnection::Submission Http3ClientConnection::submit(RejectedRequest request) {
    return submit(std::move(request.request), request.deadline);
}

Http3ClientConnection::Submission Http3ClientConnection::submit(
    HttpClientRequestStorage request, std::optional<TimePoint> deadline) {
    return submitImpl(std::move(request), deadline, nullptr);
}

Http3ClientConnection::Submission Http3ClientConnection::submit(
    HttpClientRequestStorage request, HttpClientResponseState& response,
    std::optional<TimePoint> deadline) {
    return submitImpl(std::move(request), deadline, &response);
}

Http3ClientConnection::Submission Http3ClientConnection::submitImpl(
    HttpClientRequestStorage request, std::optional<TimePoint> deadline,
    HttpClientResponseState* response) {
    requireOwnerThread();
    if (stopping_ || draining_ || terminal_ ||
        nextRequestId_ == std::numeric_limits<RequestId>::max() ||
        (session_ && session_->transport().requestBudgetExhausted())) {
        return {.outcome = Outcome::kConnectionDraining};
    }
    if (deadline && Clock::now() >= *deadline) {
        return {.outcome = Outcome::kDeadline};
    }
    if (requests_.size() >= maxRequests_) {
        return {.outcome = Outcome::kQueueFull};
    }
    if (response != nullptr &&
        (response->references == 0 || response->http3Connection != nullptr || response->complete ||
            response->headReady || response->failure || response->errorCode ||
            response->transport != HttpClientResponseTransport::kUnassigned ||
            response->hasHttp3BodyBudget() || !response->buffered.empty() ||
            !response->pending.empty())) {
        return {.outcome = Outcome::kInvalidRequest};
    }
    const auto method = classifyHttpMethod(request.method());
    auto owned = Http3ClientRequestWrite::create(
        std::move(request), "https", authority_, resource_);
    if (!owned) {
        return {.outcome = Outcome::kInvalidRequest};
    }
    const RequestId id = ++nextRequestId_;
    if (response == nullptr) {
        requests_.emplace_back(id, std::move(*owned), worker_, resource_, deadline);
    } else {
        requests_.emplace_back(
            id, std::move(*owned), worker_, resource_, deadline, *response, *receiveBodyBudget_);
        response->retainReference();
        response->requestMethod = method;
        response->transport = HttpClientResponseTransport::kHttp3;
        response->http3Connection = this;
        response->http3RequestId = id;
        response->requestId = id;
        response->bufferedLimit = std::min(response->bufferedLimit, maxResponseBytes_);
    }
    if (session_) {
        session_->notifyWork();
    } else {
        resolver_.interrupt();  // Re-arm DNS to include a newly earlier deadline.
    }
    return {.id = id};
}

void Http3ClientConnection::start() {
    requireOwnerThread();
    const bool hasPending = std::any_of(requests_.begin(), requests_.end(),
        [](const Request& request) {
            return request.response.outcome == Outcome::kPending;
        });
    if (starting_ || running_ || stopping_ || terminal_ || !hasPending) {
        throw std::logic_error("HTTP/3 connection cannot start without a live request");
    }
    starting_ = true;
    try {
        poolTasks_.spawn(drive());
    } catch (...) {
        starting_ = false;
        throw;
    }
}

void Http3ClientConnection::startIfNeeded() {
    requireOwnerThread();
    if (!running_ && !starting_) {
        start();
    }
}

void Http3ClientConnection::cancel(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response.outcome != Outcome::kPending) {
        return;
    }
    if (!found->writer.streamId()) {
        finishRequest(*found, Outcome::kCancelled);
        if (session_) {
            session_->notifyWork();
        } else {
            resolver_.interrupt();
        }
        return;
    }
    found->cancelRequested = true;
    if (session_) {
        session_->notifyWork();
    }
}

Task<void> Http3ClientConnection::wait(RequestId id) {
    requireOwnerThread();
    const auto found = find(id);
    if (found == requests_.end()) {
        throw std::invalid_argument("HTTP/3 request ID not found");
    }
    auto& request = *found;
    ++request.waiters;
    struct WaitGuard final {
        Http3ClientConnection& connection;
        Request& request;
        ~WaitGuard() {
            --request.waiters;
            connection.maybeReleaseResponseRequest(request);
        }
    } guard{*this, request};
    while (request.response.outcome == Outcome::kPending) {
        co_await request.signal.wait();
    }
}

const Http3ClientConnection::Response* Http3ClientConnection::result(RequestId id) const noexcept {
    const auto found = find(id);
    return found == requests_.end() || found->response.outcome == Outcome::kPending
               ? nullptr
               : &found->response;
}

std::optional<Http3ClientConnection::RejectedRequest> Http3ClientConnection::takeRejectedRequest(RequestId id) {
    requireOwnerThread();
    const auto found = find(id);
    if (found == requests_.end() || found->response.outcome != Outcome::kRequestRejected ||
        found->waiters != 0 || !found->streamRetired) {
        return std::nullopt;
    }
    auto& request = *found;
    auto* const state = request.responseState_;
    if ((state == nullptr) != !request.delivery.has_value()) {
        std::terminate();
    }
    if (state != nullptr &&
        (state->http3Connection != this || state->http3RequestId != request.id ||
            state->transport != HttpClientResponseTransport::kHttp3 || state->abandoned ||
            request.consumerReleased || state->http3ResponseStarted || state->headReady ||
            state->complete ||
            state->failure || state->errorCode ||
            state->producerBodyBytes() != 0 || !state->hasHttp3BodyBudget() ||
            state->http3BodyBudget.retainedBytes() != 0)) {
        return std::nullopt;
    }

    auto ownedRequest = request.writer.takeRequestAfterRetirement();
    if (!ownedRequest) {
        return std::nullopt;
    }
    RejectedRequest handoff{.request = std::move(*ownedRequest), .deadline = request.deadline};
    if (state != nullptr) {
        state->releaseHttp3BodyBudget();
        state->http3Connection = nullptr;
        state->http3RequestId = 0;
        state->requestId = 0;
        state->cancellationId = 0;
        state->transport = HttpClientResponseTransport::kUnassigned;
        request.responseState_ = nullptr;
        request.delivery.reset();
        state->releaseReference();
    }
    requests_.erase(found);
    return handoff;
}

bool Http3ClientConnection::release(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response.outcome == Outcome::kPending ||
        found->waiters != 0 || found->responseState_ != nullptr) {
        return false;
    }
    if (found->response.outcome == Outcome::kComplete) {
        bodyBudget_.release(found->response.body.size());
        retainedResultBodyBytes_ -= found->response.body.size();
        if (session_) {
            session_->notifyWork();
        }
    }
    requests_.erase(found);
    return true;
}

bool Http3ClientConnection::releaseResponseRequest(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || found->response.outcome == Outcome::kPending ||
        found->response.outcome == Outcome::kRequestRejected || !found->streamRetired ||
        found->waiters != 0 || found->responseState_ == nullptr || !found->delivery) {
        return false;
    }

    auto& request = *found;
    auto* const state = request.responseState_;
    if (state->http3Connection != this || state->http3RequestId != request.id ||
        state->transport != HttpClientResponseTransport::kHttp3) {
        std::terminate();
    }
    const auto bodyBytes = state->producerBodyBytes();
    if (request.delivery->retainedBodyBytes() != bodyBytes ||
        state->http3BodyBudget.retainedBytes() != bodyBytes) {
        return false;
    }
    if (receiveBodyBudget_ == &bodyBudget_ && bodyBytes != 0) {
        return false;
    }
    if (bodyBytes == 0) {
        state->releaseHttp3BodyBudget();
    }

    state->http3Connection = nullptr;
    state->http3RequestId = 0;
    state->requestId = 0;
    state->transport = HttpClientResponseTransport::kUnassigned;
    request.responseState_ = nullptr;
    request.delivery.reset();
    state->releaseReference();
    requests_.erase(found);
    return true;
}

void Http3ClientConnection::maybeReleaseResponseRequest(Request& request) noexcept {
    if (!request.consumerReleased || request.waiters != 0 ||
        request.response.outcome == Outcome::kPending || !request.streamRetired) {
        return;
    }
    auto* const state = request.responseState_;
    if (state == nullptr || !request.delivery) {
        std::terminate();
    }
    state->discardResponseBody();
    if (request.response.outcome == Outcome::kRequestRejected) {
        state->releaseHttp3BodyBudget();
        state->http3Connection = nullptr;
        state->http3RequestId = 0;
        state->requestId = 0;
        state->transport = HttpClientResponseTransport::kUnassigned;
        request.responseState_ = nullptr;
        request.delivery.reset();
        state->releaseReference();
        const auto found = find(request.id);
        if (found == requests_.end()) {
            std::terminate();
        }
        requests_.erase(found);
        return;
    }
    if (!releaseResponseRequest(request.id)) {
        std::terminate();
    }
}

void Http3ClientConnection::reapReleasedResponseRequests() noexcept {
    for (auto it = requests_.begin(); it != requests_.end();) {
        auto current = it++;
        auto& request = *current;
        if (!request.consumerReleased || request.waiters != 0 ||
            request.response.outcome == Outcome::kPending || !request.streamRetired) {
            continue;
        }
        maybeReleaseResponseRequest(request);
    }
}

void Http3ClientConnection::abandonResponse(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || !found->delivery || found->responseState_ == nullptr) {
        return;
    }
    auto& request = *found;
    request.responseState_->abandoned = true;
    if (request.response.outcome != Outcome::kPending) {
        return;
    }
    if (!request.writer.streamId()) {
        finishRequest(request, Outcome::kCancelled);
    } else {
        request.cancelRequested = true;
        if (session_) {
            session_->notifyWork();
        }
    }
}

void Http3ClientConnection::consumerReleased(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    const auto found = find(id);
    if (found == requests_.end() || !found->delivery || found->responseState_ == nullptr) {
        return;
    }
    auto& request = *found;
    request.consumerReleased = true;
    if (request.response.outcome == Outcome::kPending) {
        if (!request.writer.streamId()) {
            finishRequest(request, Outcome::kCancelled);
            maybeReleaseResponseRequest(request);
            return;
        }
        request.cancelRequested = true;
        if (session_) {
            session_->notifyWork();
        }
    }
    maybeReleaseResponseRequest(request);
}

void Http3ClientConnection::requestStop() noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    stopping_ = true;
    resolver_.requestStop();
    if (session_) {
        session_->requestStop();
    }
    if (!running_ && !starting_) {
        finishAll(Outcome::kCancelled);
    }
}

std::optional<Http3ClientConnection::TimePoint> Http3ClientConnection::nextDeadline(
    std::optional<TimePoint> connectDeadline) const noexcept {
    for (const auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending) {
            continue;
        }
        if (request.deadline && (!connectDeadline || *request.deadline < *connectDeadline)) {
            connectDeadline = request.deadline;
        }
        if (request.writeDeadline &&
            (!connectDeadline || *request.writeDeadline < *connectDeadline)) {
            connectDeadline = request.writeDeadline;
        }
    }
    return connectDeadline;
}

bool Http3ClientConnection::retireRequest(Request& request) noexcept {
    if (request.streamRetired) {
        return true;
    }
    const auto id = request.writer.streamId();
    if (!id) {
        request.streamRetired = true;
        return true;
    }
    try {
        if (session_) {
            const auto closed = session_->transport().terminateRequestStream(*id);
            if (closed.close != Http3QuicStreamSet::Error::kNone) {
                // Leave parser storage intact until the driver closes the
                // entire session. Local retirement cannot stop QUIC delivery.
                return false;
            }
        }
        // No session means the sole driver has already destroyed transport.
        if (!request.responseParserRegistered) {
            // The QUIC stream has now been physically retired, but registration
            // never installed parser state (for example, allocation failed).
            request.streamRetired = true;
            return true;
        }
        auto parsed = responseEngine_.response(*id);
        if (!parsed) {
            (void)responseEngine_.cancelRequest(*id);
            parsed = responseEngine_.response(*id);
        }
        if (!parsed) {
            if (session_) {
                return false;
            }
            // The whole QUIC session has already been destroyed; no peer input
            // can reach this now-absent parser node or its former sink.
            request.responseParserRegistered = false;
            request.streamRetired = true;
            return true;
        }
        if (parsed->responseBodyPlan) {
            request.response.responseBodyPlan = parsed->responseBodyPlan;
        }
        request.streamRetired = responseEngine_.release(*id);
        if (request.streamRetired) {
            request.responseParserRegistered = false;
        }
        return request.streamRetired;
    } catch (...) {
        return false;
    }
}

void Http3ClientConnection::finishRequest(Request& request, Outcome outcome) {
    if (request.response.outcome != Outcome::kPending) {
        return;
    }
    request.writeDeadline.reset();
    auto* state = request.responseState_;
    auto* delivery = request.delivery ? &*request.delivery : nullptr;
    if (outcome == Outcome::kComplete) {
        const auto id = request.writer.streamId();
        const auto parsed = id ? responseEngine_.response(*id) : std::nullopt;
        if (!parsed || !parsed->complete) {
            throw std::logic_error("HTTP/3 response completion without valid FIN");
        }
        if (parsed->body.size() > kMaxBufferedConnectionBodyBytes - retainedResultBodyBytes_) {
            outcome = Outcome::kResponseTooLarge;
        } else {
            request.response.status = parsed->status;
            request.response.responseBodyPlan = parsed->responseBodyPlan;
            if (delivery != nullptr) {
                if (state == nullptr || !state->headReady || !delivery->responseBodyPlan()) {
                    throw std::logic_error("HTTP/3 incremental response completed without its head");
                }
                // The sink already copied headers and trailers directly into
                // the stable response state. Do not transfer or copy them again.
            } else {
                request.response.headers.reserve(parsed->headers.size());
                for (const auto& field : parsed->headers) {
                    request.response.headers.push_back(
                        HttpHeader::copyOf(field.name, field.value, resource_));
                }
                request.response.trailers.reserve(parsed->trailers.size());
                for (const auto& field : parsed->trailers) {
                    request.response.trailers.push_back(
                        HttpHeader::copyOf(field.name, field.value, resource_));
                }
                auto body = responseEngine_.takeBody(*id);
                if (!body) {
                    throw std::logic_error("HTTP/3 completed body has already been transferred");
                }
                request.response.body = std::move(*body);
            }
        }
    }

    if (!retireRequest(request) && session_ != nullptr) {
        // The only driver owns all QUIC callback storage. If STOP_SENDING or
        // parser retirement failed, close/join that driver before publishing a
        // terminal response or releasing the delivery sink.
        throw std::runtime_error("HTTP/3 request stream could not be retired locally");
    }

    const bool handoffRejected = delivery != nullptr && outcome == Outcome::kRequestRejected &&
                                 terminalFailure_ != Outcome::kProtocolError && state != nullptr &&
                                 !state->http3ResponseStarted && !state->headReady &&
                                 !state->complete && !state->failure &&
                                 !state->errorCode && state->producerBodyBytes() == 0 &&
                                 delivery->callbackFailure() == nullptr &&
                                 delivery->retirementReason() ==
                                     Http3ClientResponseDelivery::RetirementReason::kNone;
    if (delivery != nullptr) {
        if (handoffRejected) {
            // Keep the public response pending until the pool retries this
            // stream on a fresh connection or commits the rejection to it.
        } else if (outcome == Outcome::kRequestRejected) {
            outcome = Outcome::kProtocolError;
            (void)delivery->commitTerminalError(HttpClientError::Code::kProtocolError);
        } else if (terminalFailure_ == Outcome::kProtocolError) {
            outcome = Outcome::kProtocolError;
            (void)delivery->commitTerminalError(HttpClientError::Code::kProtocolError);
        } else if (delivery->callbackFailure() != nullptr) {
            outcome = Outcome::kProtocolError;
            (void)delivery->commitFailure(delivery->callbackFailure());
        } else if (delivery->retirementReason() !=
                   Http3ClientResponseDelivery::RetirementReason::kNone) {
            switch (delivery->retirementReason()) {
                case Http3ClientResponseDelivery::RetirementReason::kResponseTooLarge:
                    outcome = Outcome::kResponseTooLarge;
                    (void)delivery->commitRetirementFailure(
                        HttpClientError::Code::kResponseTooLarge);
                    break;
                case Http3ClientResponseDelivery::RetirementReason::kProtocolError:
                    outcome = Outcome::kProtocolError;
                    (void)delivery->commitRetirementFailure(
                        HttpClientError::Code::kProtocolError);
                    break;
                case Http3ClientResponseDelivery::RetirementReason::kCallbackFailure:
                    outcome = Outcome::kProtocolError;
                    (void)delivery->commitFailure(delivery->callbackFailure());
                    break;
                case Http3ClientResponseDelivery::RetirementReason::kNone:
                    std::terminate();
            }
        } else if (outcome == Outcome::kComplete) {
            try {
                if (request.deadline && Clock::now() >= *request.deadline) {
                    outcome = Outcome::kDeadline;
                } else {
                    const auto plan = delivery->responseBodyPlan();
                    const bool contentSemanticsPresent =
                        plan && plan->contentSemantics() == HttpResponseContentSemantics::kWithContent;
                    decodeHttpClientResponseContentEncoding(
                        *state, contentSemanticsPresent, maxResponseBytes_, resource_);
                    if (request.deadline && Clock::now() >= *request.deadline) {
                        outcome = Outcome::kDeadline;
                        state->discardResponseBody();
                    }
                }
                if (outcome == Outcome::kComplete) {
                    const auto committed = delivery->commitComplete();
                    if (committed != Http3ClientResponseDelivery::CommitStatus::kCommitted) {
                        outcome = Outcome::kProtocolError;
                        (void)delivery->commitTerminalError(
                            HttpClientError::Code::kProtocolError);
                    }
                } else {
                    (void)delivery->commitTerminalError(clientErrorCode(outcome));
                }
            } catch (...) {
                const auto failure = std::current_exception();
                try {
                    std::rethrow_exception(failure);
                } catch (const HttpClientError& error) {
                    outcome = outcomeForClientError(error.code());
                } catch (...) {
                    outcome = Outcome::kProtocolError;
                }
                (void)delivery->commitFailure(failure);
            }
        } else {
            (void)delivery->commitTerminalError(clientErrorCode(outcome));
        }
    } else if (outcome == Outcome::kComplete) {
        // takeBody() relinquished the receive owner's reservation. There is
        // no suspension or further receive between that transfer and this
        // result-owner reservation on the same worker.
        if (!bodyBudget_.tryRetain(request.response.body.size())) {
            throw std::logic_error("HTTP/3 transferred body lost its budget reservation");
        }
        retainedResultBodyBytes_ += request.response.body.size();
    } else {
        std::pmr::string empty(resource_);
        request.response.body.swap(empty);
    }
    request.response.outcome = outcome;
    request.signal.notify();
}

void Http3ClientConnection::finishAll(Outcome outcome) noexcept {
    for (auto& request : requests_) {
        if (request.response.outcome == Outcome::kPending) {
            try {
                finishRequest(request, outcome);
            } catch (...) {
                // The driver has already closed QUIC before connection-wide
                // terminal publication. Preserve the failure for any bound
                // response state and never strand a waiter.
                const auto failure = std::current_exception();
                if (!retireRequest(request)) {
                    std::terminate();
                }
                if (request.delivery && !request.delivery->commitFailure(failure)) {
                    std::terminate();
                }
                request.response.outcome = Outcome::kTransportError;
                request.signal.notify();
            }
        }
    }
    reapReleasedResponseRequests();
}

Task<void> Http3ClientConnection::drive() {
    running_ = true;
    starting_ = false;
    Outcome failure = Outcome::kConnectFailed;
    try {
        const auto connectDeadline = deadlineAfter(Clock::now(), connectTimeout_);
        std::optional<Http3QuicClientEndpointResolver::Result> resolved;
        while (!stopping_) {
            auto attempt = co_await resolver_.resolve(host_, port_, nextDeadline(connectDeadline));
            const auto now = Clock::now();
            for (auto& request : requests_) {
                if (request.response.outcome == Outcome::kPending && request.deadline &&
                    now >= *request.deadline) {
                    finishRequest(request, Outcome::kDeadline);
                }
            }
            const bool stillPending = std::any_of(requests_.begin(), requests_.end(),
                [](const Request& request) {
                    return request.response.outcome == Outcome::kPending;
                });
            if (!stillPending) {
                break;
            }
            if (now >= connectDeadline ||
                (attempt.status != Http3QuicClientEndpointResolver::Status::kInterrupted &&
                    attempt.status != Http3QuicClientEndpointResolver::Status::kTimeout)) {
                resolved.emplace(std::move(attempt));
                break;
            }
        }
        if (stopping_ || (resolved && resolved->status == Http3QuicClientEndpointResolver::Status::kStopped)) {
            failure = Outcome::kCancelled;
        } else if (!resolved || resolved->status == Http3QuicClientEndpointResolver::Status::kTimeout ||
                   Clock::now() >= connectDeadline) {
            failure = Outcome::kDeadline;
        } else if (resolved->status == Http3QuicClientEndpointResolver::Status::kResolved) {
            for (std::size_t index = 0; index < resolved->endpoints.size(); ++index) {
                const auto now = Clock::now();
                if (stopping_ || now >= connectDeadline) {
                    failure = stopping_ ? Outcome::kCancelled : Outcome::kDeadline;
                    break;
                }
                const auto remainingAddresses = resolved->endpoints.size() - index;
                const auto share = Clock::duration(
                    (connectDeadline - now).count() /
                    static_cast<Clock::duration::rep>(remainingAddresses));
                const auto attemptBudget = std::min(connectDeadline - now,
                    std::max(share, std::chrono::duration_cast<Clock::duration>(
                                        std::chrono::milliseconds(100))));
                const bool tryNext = co_await driveEndpoint(
                    resolved->endpoints[index], now + attemptBudget);
                session_.reset();
                peerStreams_.clear();
                if (!tryNext || stopping_) {
                    failure = stopping_ ? Outcome::kCancelled : Outcome::kTransportError;
                    break;
                }
                const bool stillPending = std::any_of(requests_.begin(), requests_.end(),
                    [](const Request& request) {
                        return request.response.outcome == Outcome::kPending;
                    });
                if (!stillPending) {
                    break;
                }
            }
            if (!stopping_ && Clock::now() >= connectDeadline &&
                failure == Outcome::kConnectFailed) {
                failure = Outcome::kDeadline;
            }
        }
    } catch (...) {
        failure = stopping_ ? Outcome::kCancelled : terminalFailure_;
    }
    if (session_) {
        session_->requestStop();
        session_->close();
        session_.reset();
    }
    // Close all QUIC delivery before retiring parser storage or notifying
    // terminal response waiters. No socket wait survives the driver await.
    (void)responseEngine_.stop();
    finishAll(failure);
    terminal_ = true;
    running_ = false;
    if (lifecycleNotification_.notify != nullptr) {
        lifecycleNotification_.notify(lifecycleNotification_.context);
    }
}

Task<bool> Http3ClientConnection::driveEndpoint(
    const asio::ip::udp::endpoint& peer, TimePoint connectDeadline) {
    session_ = makePmrObject<Http3QuicClientSocketSession>(resource_, io_, peer, host_, tls_);
    bool hadHttp3 = false;
    std::optional<TimePoint> idleDeadline;
    std::size_t consecutiveWorkTicks = 0;
    while (!stopping_) {
        (void)session_->consumeWorkNotification();
        const auto tick = session_->pump();
        if (tick.status == Http3QuicClientSocketSession::PumpStatus::kFatal ||
            tick.status == Http3QuicClientSocketSession::PumpStatus::kClosed) {
            break;
        }
        const bool ready = session_->transport().connectionInfo() ==
                           Http3QuicClientTransport::State::kH3Ready;
        hadHttp3 |= ready;
        if (ready && session_->transport().requestBudgetExhausted()) {
            draining_ = true;
        }
        const auto now = Clock::now();
        for (auto& request : requests_) {
            if (!ready && request.response.outcome == Outcome::kPending && request.deadline &&
                now >= *request.deadline) {
                finishRequest(request, Outcome::kDeadline);
            }
        }
        if (now >= connectDeadline && !ready) {
            break;
        }
        const bool protocolProgress = ready && sweep(tick.criticalStreamsReady);
        bool active = false;
        for (const auto& request : requests_) {
            active |= request.response.outcome == Outcome::kPending;
        }
        if (active || !ready || draining_) {
            idleDeadline.reset();
        } else if (!idleDeadline) {
            idleDeadline = deadlineAfter(now, idleTimeout_);
        }
        if (!active && (draining_ || !ready)) {
            break;
        }
        if (idleDeadline && now >= *idleDeadline) {
            break;
        }
        if (tick.received || tick.sent || tick.criticalOutputProgress || protocolProgress) {
            if (++consecutiveWorkTicks < 8) {
                continue;  // Drain only a bounded amount before yielding to the worker.
            }
            consecutiveWorkTicks = 0;
            auto yielded = co_await ruvia::asyncAsio([this](auto completion) {
                asio::post(io_, [completion = std::move(completion)]() mutable {
                    completion(asio::error_code{});
                });
            });
            if (yielded.errorCode()) {
                break;
            }
            continue;
        }
        consecutiveWorkTicks = 0;
        auto deadline = nextDeadline(ready ? std::nullopt : std::optional{connectDeadline});
        if (idleDeadline && (!deadline || *idleDeadline < *deadline)) {
            deadline = idleDeadline;
        }
        const auto wake = co_await session_->waitForActivity(tick, deadline);
        if (wake == Http3QuicClientSocketSession::WakeReason::kStopped ||
            wake == Http3QuicClientSocketSession::WakeReason::kFatal ||
            (wake == Http3QuicClientSocketSession::WakeReason::kDeadline &&
                !ready && Clock::now() >= connectDeadline)) {
            break;
        }
        // An application submission can race the timer callback on this
        // worker. Re-evaluate pending requests at the top of the loop before
        // committing idle retirement; a newly admitted request clears it.
    }
    session_->requestStop();
    session_->close();
    co_return !hadHttp3 && !stopping_;
}

bool Http3ClientConnection::sweep(bool requestsMayStart) {
    bool progress = false;
    const auto now = Clock::now();
    for (auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending) {
            continue;
        }
        if (request.cancelRequested) {
            finishRequest(request, Outcome::kCancelled);
            progress = true;
        } else if ((request.deadline && now >= *request.deadline) ||
                   (request.writeDeadline && now >= *request.writeDeadline)) {
            finishRequest(request, Outcome::kDeadline);
            progress = true;
        }
    }
    progress |= receivePeerStreams();
    // Read already-available request streams before applying a newly observed
    // GOAWAY cutoff. A final response head is evidence that the request was
    // processed, even if the body stream has not reached FIN yet.
    progress |= receiveRequests();
    if (const auto goaway = responseEngine_.peerGoawayId()) {
        draining_ = true;
        for (auto& request : requests_) {
            const bool responseStarted = request.responseState_ != nullptr &&
                                         request.responseState_->http3ResponseStarted;
            if (request.response.outcome == Outcome::kPending && !responseStarted &&
                (!request.writer.streamId() || *request.writer.streamId() >= *goaway)) {
                const auto id = request.writer.streamId();
                const bool unprocessed = !id || responseEngine_.peerReportsUnprocessed(*id);
                finishRequest(request, unprocessed ? Outcome::kRequestRejected : Outcome::kProtocolError);
                progress = true;
            }
        }
    }
    if (requestsMayStart) {
        progress |= driveRequestWriters();
    }
    return progress;
}

bool Http3ClientConnection::receivePeerStreams() {
    auto& quic = session_->transport();
    const auto accepted = quic.acceptPeerStreams();
    if (accepted.error != Http3QuicStreamSet::Error::kNone) {
        throw std::runtime_error("QUIC client cannot accept peer critical streams");
    }
    for (std::size_t i = 0; i < accepted.size; ++i) {
        const auto& stream = accepted.streams[i];
        if (!stream.readable || stream.writeable ||
            peerStreams_.size() == Http3QuicClientTransport::kMaxStreamsPerConnection) {
            throw std::runtime_error("invalid or excessive peer HTTP/3 stream");
        }
        peerStreams_.push_back(stream.id);
    }
    bool progress = accepted.size != 0;
    for (auto it = peerStreams_.begin(); it != peerStreams_.end();) {
        const auto part = receiver_.drive(*it, [&quic](std::uint64_t id, std::span<char> bytes) {
            return quic.readStream(id, bytes);
        });
        if (part.status == Http3ClientReceiveDriver::Status::kProgress) {
            progress = true;
            ++it;
        } else if (part.status == Http3ClientReceiveDriver::Status::kPeerStreamEnded) {
            (void)quic.closeStream(*it);
            it = peerStreams_.erase(it);
            progress = true;
        } else if (part.status == Http3ClientReceiveDriver::Status::kBlocked) {
            ++it;
        } else {
            terminalFailure_ = part.status == Http3ClientReceiveDriver::Status::kConnectionError
                                   ? Outcome::kProtocolError
                                   : Outcome::kTransportError;
            throw std::runtime_error("HTTP/3 peer stream failed");
        }
    }
    return progress;
}

bool Http3ClientConnection::receiveRequests() {
    bool progress = false;
    auto& quic = session_->transport();
    for (auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending || !request.writer.streamId()) {
            continue;
        }
        std::size_t readBudget = Http3ClientReceiveDriver::kReadBlockBytes;
        if (request.delivery) {
            const auto allowance = request.delivery->readAllowance(readBudget);
            if (allowance.status == Http3ClientResponseDelivery::ReadStatus::kBackpressured ||
                allowance.status == Http3ClientResponseDelivery::ReadStatus::kTerminal) {
                continue;
            }
            if (allowance.status == Http3ClientResponseDelivery::ReadStatus::kRetirementRequired) {
                const auto reason = request.delivery->retirementReason();
                finishRequest(request, reason == Http3ClientResponseDelivery::RetirementReason::kResponseTooLarge
                                           ? Outcome::kResponseTooLarge
                                       : reason == Http3ClientResponseDelivery::RetirementReason::kCallbackFailure
                                           ? Outcome::kProtocolError
                                           : Outcome::kProtocolError);
                progress = true;
                continue;
            }
            readBudget = allowance.bytes;
        }
        const auto part = receiver_.drive(*request.writer.streamId(), [&quic](std::uint64_t id, std::span<char> bytes) { return quic.readStream(id, bytes); }, readBudget);
        if (part.status == Http3ClientReceiveDriver::Status::kConnectionError) {
            terminalFailure_ = part.protocol.status == Http3ClientSansIoSessionStatus::kBodyLimitExceeded
                                   ? Outcome::kResponseTooLarge
                                   : Outcome::kProtocolError;
            throw std::runtime_error("HTTP/3 response connection protocol failed");
        }
        if (part.status == Http3ClientReceiveDriver::Status::kTransportError) {
            terminalFailure_ = Outcome::kTransportError;
            throw std::runtime_error("HTTP/3 response transport failed");
        }
        if (request.delivery &&
            request.delivery->retirementReason() !=
                Http3ClientResponseDelivery::RetirementReason::kNone) {
            const auto reason = request.delivery->retirementReason();
            finishRequest(request, reason == Http3ClientResponseDelivery::RetirementReason::kResponseTooLarge
                                       ? Outcome::kResponseTooLarge
                                   : reason == Http3ClientResponseDelivery::RetirementReason::kCallbackFailure
                                       ? Outcome::kProtocolError
                                       : Outcome::kProtocolError);
            progress = true;
            continue;
        }
        switch (part.status) {
            case Http3ClientReceiveDriver::Status::kProgress:
                progress = true;
                break;
            case Http3ClientReceiveDriver::Status::kBlocked:
                break;
            case Http3ClientReceiveDriver::Status::kResponseComplete:
                finishRequest(request, Outcome::kComplete);
                progress = true;
                break;
            case Http3ClientReceiveDriver::Status::kStreamReset:
                request.response.peerResetErrorCode = part.peerResetErrorCode;
                finishRequest(request, part.peerReportsUnprocessed &&
                                               (request.responseState_ == nullptr ||
                                                   !request.responseState_->http3ResponseStarted)
                                           ? Outcome::kRequestRejected
                                           : Outcome::kProtocolError);
                progress = true;
                break;
            case Http3ClientReceiveDriver::Status::kStreamError:
                finishRequest(request, Outcome::kProtocolError);
                progress = true;
                break;
            case Http3ClientReceiveDriver::Status::kConnectionError:
                terminalFailure_ = part.protocol.status == Http3ClientSansIoSessionStatus::kBodyLimitExceeded
                                       ? Outcome::kResponseTooLarge
                                       : Outcome::kProtocolError;
                throw std::runtime_error("HTTP/3 response connection protocol failed");
            default:
                throw std::runtime_error("HTTP/3 response stream or QUIC connection failed");
        }
    }
    reapReleasedResponseRequests();
    return progress;
}

bool Http3ClientConnection::driveRequestWriters() {
    bool progress = false;
    auto& quic = session_->transport();
    for (auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending || request.writer.finished()) {
            continue;
        }
        if (draining_ && !request.writer.streamId()) {
            // Local admission stopped before a stream or any request bytes
            // existed. Preserve the same handoff path as explicit peer rejection.
            finishRequest(request, Outcome::kRequestRejected);
            progress = true;
            continue;
        }
        if (writeTimeout_ && !request.writeDeadline) {
            request.writeDeadline = deadlineAfter(Clock::now(), *writeTimeout_);
        }
        const auto write = request.writer.drive(
            [&quic] { return quic.openLocalBidirectionalStream(); },
            [this, &request](std::uint64_t id, HttpKnownMethod method) {
                const auto sink = request.delivery
                                      ? Http3ClientResponseEventSink{
                                            .callback = onResponseEvent, .context = &request}
                                      : Http3ClientResponseEventSink{};
                const auto registered = responseEngine_.registerRequest(id, method, sink);
                if (registered.scope != Http3ConnectionErrorScope::kNone) {
                    return false;
                }
                request.responseParserRegistered = true;
                return true;
            },
            [&quic](std::uint64_t id, std::span<const char> bytes) {
                return quic.writeStream(id, bytes);
            },
            [&quic](std::uint64_t id) { return quic.finishStream(id); });
        if (write == Http3ClientRequestDriver::Result::kProgress ||
            write == Http3ClientRequestDriver::Result::kFinished) {
            request.writeDeadline = write == Http3ClientRequestDriver::Result::kFinished ||
                                            !writeTimeout_
                                        ? std::nullopt
                                        : std::optional{deadlineAfter(Clock::now(), *writeTimeout_)};
            progress = true;
        } else if (write == Http3ClientRequestDriver::Result::kConnectionDraining) {
            draining_ = true;
            finishRequest(request, Outcome::kRequestRejected);
            progress = true;
        } else if (write == Http3ClientRequestDriver::Result::kFatal) {
            throw std::runtime_error("HTTP/3 request write or registration failed");
        }
    }
    return progress;
}

}  // namespace ruvia::detail
