#include "http3/Http3ClientConnection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/HttpClientTypes.h"

#include "client/HttpClientResponseDecoding.h"
#include "client/HttpClientUploadState.h"

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
    std::size_t maxRequests, std::size_t maxResponseBytes, Http3QpackConfig qpack, bool origins, bool datagrams) {
    if (maxRequests == 0 || maxRequests > ruvia::quic_limits{}.max_streams ||
        maxResponseBytes == 0 || maxResponseBytes > kMaxBufferedConnectionBodyBytes) {
        throw std::invalid_argument("HTTP/3 connection request and body bounds must be positive");
    }
    const auto aggregate = maxResponseBytes > std::numeric_limits<std::size_t>::max() / maxRequests
                               ? std::numeric_limits<std::size_t>::max()
                               : maxResponseBytes * maxRequests;
    return {.maxLiveStreams = maxRequests,
        .maxBodyBytesPerStream = maxResponseBytes,
        .maxTotalBodyBytes = std::min(aggregate, kMaxBufferedConnectionBodyBytes),
        .connection = {.maxActiveStreams = maxRequests, .qpackMaxTableCapacity = qpack.maxTableCapacity, .qpackBlockedStreams = qpack.maxBlockedStreams, .enableDatagrams = datagrams, .receiveOriginAdvertisements = origins}};
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
    TaskScope& poolTasks, http3_quic_client_tls_context& tls, HttpOriginView origin,
    std::chrono::milliseconds connectTimeout, std::pmr::memory_resource* resource,
    std::size_t maxRequests, std::size_t maxResponseBytes,
    std::chrono::milliseconds idle_timeout, Http3ClientBodyBudget* receiveBodyBudget,
    std::optional<std::chrono::milliseconds> write_timeout)
    : Http3ClientConnection(io, worker, poolTasks, tls, origin, connectTimeout, resource,
          maxRequests, maxResponseBytes, idle_timeout, receiveBodyBudget, write_timeout, {}, {},
          {}, {}, ruvia::quic_version::v1, false) {}

Http3ClientConnection::Http3ClientConnection(asio::io_context& io, const WorkerHandle& worker,
    TaskScope& poolTasks, http3_quic_client_tls_context& tls, HttpOriginView origin,
    std::chrono::milliseconds connectTimeout,
    std::pmr::memory_resource* resource, std::size_t maxRequests, std::size_t maxResponseBytes,
    std::chrono::milliseconds idle_timeout, Http3ClientBodyBudget* receiveBodyBudget,
    std::optional<std::chrono::milliseconds> write_timeout,
    LifecycleNotification lifecycleNotification, Http3QpackConfig qpack,
    Http3ClientOriginObserver originObserver, Http3ClientPushObserver pushObserver,
    ruvia::quic_version initial_version, bool enable_early_data)
    : ownerThread_(std::this_thread::get_id()),
      io_(io),
      worker_(worker),
      poolTasks_(poolTasks),
      tls_(tls),
      resource_(requireResource(resource)),
      host_(origin.host(), resource_),
      authority_(makeHttpOriginAuthority(origin, resource_)),
      port_(origin.port()),
      initial_version_(initial_version),
      enable_early_data_(enable_early_data),
      connectTimeout_(checkedTimeout(connectTimeout)),
      idleTimeout_(checkedTimeout(idle_timeout)),
      writeTimeout_(checkedTimeout(write_timeout)),
      maxRequests_(maxRequests),
      maxResponseBytes_(maxResponseBytes),
      resolver_(io_, resource_),
      bodyBudget_(kMaxBufferedConnectionBodyBytes),
      receiveBodyBudget_(receiveBodyBudget == nullptr ? &bodyBudget_ : receiveBodyBudget),
      lifecycleNotification_(lifecycleNotification),
      originObserver_(originObserver),
      pushObserver_(pushObserver),
      responseEngine_(resource_, bodyBudget_, responseLimits(maxRequests, maxResponseBytes, qpack, originObserver.receive != nullptr, true)),
      receiver_(responseEngine_),
      criticalOutput_{std::pmr::string(resource_), std::pmr::string(resource_), std::pmr::string(resource_)},
      session_(nullptr, PmrObjectDeleter<Http3QuicClientSocketSession>{resource_}),
      receiveBodyBudgetWake_(*receiveBodyBudget_, onReceiveBodyBudgetReleased, this),
      requests_(resource_),
      pushes_(resource_),
      peerPushStreams_(resource_),
      peerStreams_(resource_) {
    if (!worker_.valid() || origin.scheme() != HttpScheme::kHttps || port_ == 0 ||
        (lifecycleNotification_.context == nullptr) !=
            (lifecycleNotification_.notify == nullptr) ||
        (originObserver_.context == nullptr) != (originObserver_.receive == nullptr)) {
        throw std::invalid_argument(
            "HTTP/3 connection requires a worker, HTTPS origin, and complete notification");
    }
    if ((pushObserver_.context == nullptr) != (pushObserver_.receive == nullptr) ||
        (pushObserver_.receive == nullptr) != (pushObserver_.finished == nullptr) ||
        (pushObserver_.receive != nullptr && (!pushObserver_.config.enabled || receiveBodyBudget == nullptr ||
                                                 pushObserver_.config.maxConcurrentPushes == 0 ||
                                                 pushObserver_.config.maxConcurrentPushes > ruvia::quic_limits{}.max_streams))) {
        throw std::invalid_argument("HTTP/3 push requires bounded admission and a result-stable body budget");
    }
    if (pushObserver_.receive != nullptr) {
        if (pushObserver_.config.timeout) {
            (void)checkedTimeout(*pushObserver_.config.timeout);
        }
        authorizedPushId_ = pushObserver_.config.maxConcurrentPushes - 1;
        if (!responseEngine_.queueMaxPushId(authorizedPushId_)) {
            throw std::invalid_argument("HTTP/3 initial push authorization failed");
        }
        peerPushStreams_.reserve(ruvia::quic_limits{}.max_streams);
        responseEngine_.observePushes(onPushEvent, this);
    }
    // URI authority retains IP-literal brackets. DNS/TLS receive only the
    // address, never brackets or the origin's port suffix.
    if (host_.front() == '[') {
        host_.erase(host_.size() - 1);
        host_.erase(0, 1);
    }
    peerStreams_.reserve(ruvia::quic_limits{}.max_streams);
    if (originObserver_.receive != nullptr) {
        responseEngine_.observeOrigins([](void* raw, const Http3ConnectionEvent& event) {
            auto& owner = *static_cast<Http3ClientConnection*>(raw);
            if (event.originAdvertisement != nullptr) {
                owner.originObserver_.receive(owner.originObserver_.context, owner.originObserver_.connectionSlot, *event.originAdvertisement);
            }
        },
            this);
    }
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
    if (!pushes_.empty()) {
        std::terminate();
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
        if (auto& upload = request.responseState_->upload; upload && event.requestContentSignal) {
            if (*event.requestContentSignal == HttpClientRequestContentSignal::kContinue) {
                upload->contentReleased = true;
                request.continueDeadline.reset();
                upload->output.notifyData();
            } else if (!upload->output.ended) {
                upload->output.stop();
                request.continueDeadline.reset();
            }
        }
        switch (event.kind) {
            case Http3ConnectionEventKind::kPushStream:
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
        nextRequestId_ == std::numeric_limits<RequestId>::max()) {
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
    const bool replay_safe = request.replay_safe();
    const bool early_data_eligible = replay_safe && !request.isTunnel() &&
                                     request.upload() == nullptr && !HttpClientRequestStorageAccess::hasBody(request) &&
                                     (method == HttpKnownMethod::kGet || method == HttpKnownMethod::kHead);
    auto owned = Http3ClientRequestWrite::create(
        std::move(request), "https", authority_, resource_);
    if ((owned.index() != 0)) {
        return {.outcome = Outcome::kInvalidRequest};
    }
    const RequestId id = ++nextRequestId_;
    if (response == nullptr) {
        requests_.emplace_back(id, std::move(std::get<0>(owned)), worker_, resource_, deadline, replay_safe);
        requests_.back().early_data_eligible = early_data_eligible;
    } else {
        requests_.emplace_back(
            id, std::move(std::get<0>(owned)), worker_, resource_, deadline, replay_safe, *response,
            *receiveBodyBudget_);
        requests_.back().early_data_eligible = early_data_eligible;
        response->retainReference();
        response->requestMethod = method;
        response->transport = HttpClientResponseTransport::kHttp3;
        response->http3Connection = this;
        response->http3RequestId = id;
        response->requestId = id;
        response->bufferedLimit = std::min(response->bufferedLimit, maxResponseBytes_);
        if (auto* output = response->output()) {
            output->wakeTarget = this;
            output->wake = [](void* target) noexcept { static_cast<Http3ClientConnection*>(target)->wakeReceiveDriver(); };
        }
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

bool Http3ClientConnection::reprioritize(RequestId id, HttpPriority priority) {
    requireOwnerThread();
    if (auto push = findPush(id); push != pushes_.end()) {
        if (stopping_ || terminal_ || push->cancelRequested || !responseEngine_.queuePushPriorityUpdate(push->pushId, priority)) {
            return false;
        }
        wakeReceiveDriver();
        return true;
    }
    const auto request = find(id);
    if (stopping_ || terminal_ || request == requests_.end() ||
        request->response.outcome != Outcome::kPending || !request->writer.streamId()) {
        return false;
    }
    if (session_ && session_->transport().info().state !=
                        ruvia::quic_connection_state::ready) {
        request->pending_priority_update = priority;
        wakeReceiveDriver();
        return true;
    }
    if (!responseEngine_.queuePriorityUpdate(*request->writer.streamId(), priority)) {
        return false;
    }
    wakeReceiveDriver();
    return true;
}

void Http3ClientConnection::cancel(RequestId id) noexcept {
    if (std::this_thread::get_id() != ownerThread_ || !worker_.isCurrent()) {
        std::terminate();
    }
    if (auto push = findPush(id); push != pushes_.end()) {
        push->cancelRequested = true;
        wakeReceiveDriver();
        return;
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
    if (auto push = findPush(id); push != pushes_.end()) {
        push->cancelRequested = true;
        wakeReceiveDriver();
        return;
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
    if (auto push = findPush(id); push != pushes_.end()) {
        push->cancelRequested = true;
        wakeReceiveDriver();
        return;
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

ruvia::quic_path_migration Http3ClientConnection::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    requireOwnerThread();
    if (stopping_ || !session_ || !running_) {
        return {.status = ruvia::quic_migration_status::rejected};
    }
    auto migration = session_->start_path_migration(local_endpoint);
    if (migration.status == ruvia::quic_migration_status::started ||
        migration.status == ruvia::quic_migration_status::validated) {
        quic_connection_migration_id_ = migration.id;
        quic_path_migration_generation_ = quic_generation_;
        auto id = next_path_migration_id_++;
        if (id == 0) {
            id = next_path_migration_id_++;
        }
        migration.id = id;
        quic_path_migration_ = migration;
    }
    return migration;
}

std::optional<ruvia::quic_path_migration> Http3ClientConnection::path_migration(
    std::uint64_t id) const noexcept {
    if (!quic_path_migration_ || quic_path_migration_->id != id) {
        return std::nullopt;
    }
    if (session_ && quic_path_migration_generation_ == quic_generation_) {
        if (const auto migration = session_->path_migration(quic_connection_migration_id_)) {
            auto result = *migration;
            result.id = id;
            return result;
        }
    }
    return quic_path_migration_;
}

std::optional<ruvia::quic_path_migration> Http3ClientConnection::active_path_migration() const noexcept {
    if (quic_path_migration_ && session_ &&
        quic_path_migration_generation_ == quic_generation_) {
        if (const auto migration = session_->path_migration(quic_connection_migration_id_)) {
            auto result = *migration;
            result.id = quic_path_migration_->id;
            return result;
        }
    }
    return quic_path_migration_;
}

ruvia::quic_operation_status Http3ClientConnection::cancel_path_migration(std::uint64_t id) {
    requireOwnerThread();
    if (!session_ || !quic_path_migration_ || quic_path_migration_->id != id) {
        return ruvia::quic_operation_status::retired;
    }
    const auto status = session_->cancel_path_migration(quic_connection_migration_id_);
    if (status == ruvia::quic_operation_status::accepted) {
        requestStop();
    }
    return status;
}

void Http3ClientConnection::cache_path_migration() noexcept {
    if (!session_ || !quic_path_migration_ ||
        quic_path_migration_generation_ != quic_generation_) {
        return;
    }
    if (const auto migration = session_->path_migration(quic_connection_migration_id_)) {
        quic_path_migration_->status = migration->status;
        quic_path_migration_->local_address = migration->local_address;
    }
    if (quic_path_migration_->status == ruvia::quic_migration_status::started) {
        quic_path_migration_->status = ruvia::quic_migration_status::aborted;
    }
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
        if (request.continueDeadline && (!connectDeadline || *request.continueDeadline < *connectDeadline)) {
            connectDeadline = request.continueDeadline;
        }
        if (request.writeDeadline &&
            (!connectDeadline || *request.writeDeadline < *connectDeadline)) {
            connectDeadline = request.writeDeadline;
        }
    }
    for (const auto& push : pushes_) {
        if (push.deadline && (!connectDeadline || *push.deadline < *connectDeadline)) {
            connectDeadline = push.deadline;
        }
    }
    for (const auto& stream : peerPushStreams_) {
        if (stream.deadline && std::none_of(pushes_.begin(), pushes_.end(), [&stream](const Push& push) { return push.streamId == stream.streamId; }) &&
            (!connectDeadline || *stream.deadline < *connectDeadline)) {
            connectDeadline = stream.deadline;
        }
    }
    for (const auto& candidate : criticalWriteDeadlines_) {
        if (candidate && (!connectDeadline || *candidate < *connectDeadline)) {
            connectDeadline = candidate;
        }
    }
    return connectDeadline;
}

bool Http3ClientConnection::retireRequest(Request& request, bool graceful) noexcept {
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
            const bool graceful_tunnel = graceful && request.writer.finished() && request.responseState_ != nullptr &&
                                         request.responseState_->tunnel && request.responseState_->tunnel->accepted &&
                                         request.responseState_->tunnel->receiveEnded;
            const auto closed = graceful_tunnel
                                    ? session_->transport().retire_completed_stream(*id)
                                    : session_->transport().terminate_bidirectional_stream(
                                          *id, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
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
        receiver_.retire(*id);
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
    request.continueDeadline.reset();
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

    if (!retireRequest(request, outcome == Outcome::kComplete) && session_ != nullptr) {
        if (outcome == Outcome::kComplete && state != nullptr && state->tunnel && state->tunnel->accepted) {
            // Queuing END_STREAM is not physical FIN submission. Keep driving
            // queued tunnel output before releasing the stream/parser owner.
            return;
        }
        // The only driver owns all QUIC callback storage. If STOP_SENDING or
        // parser retirement failed, close/join that driver before publishing a
        // terminal response or releasing the delivery sink.
        throw std::runtime_error("HTTP/3 request stream could not be retired locally");
    }

    if (state != nullptr) {
        if (auto* output = state->output()) {
            output->wake = nullptr;
            output->wakeTarget = nullptr;
            output->stop();
        }
    }
    const bool handoffRejected = delivery != nullptr && outcome == Outcome::kRequestRejected &&
                                 terminalFailure_ != Outcome::kProtocolError && state != nullptr &&
                                 state->output() == nullptr && !state->http3ResponseStarted && !state->headReady &&
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
                        *state, contentSemanticsPresent, maxResponseBytes_);
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
        cache_path_migration();
        session_->requestStop();
        session_->close();
        session_.reset();
    }
    // Close all QUIC delivery before retiring parser storage or notifying
    // terminal response waiters. No socket wait survives the driver await.
    (void)responseEngine_.stop();
    for (std::size_t i = 0; i < criticalOutput_.size(); ++i) {
        std::pmr::string(criticalOutput_[i].get_allocator()).swap(criticalOutput_[i]);
        criticalOutputOffset_[i] = 0;
        criticalWriteDeadlines_[i].reset();
    }
    while (!pushes_.empty()) {
        finishPush(pushes_.begin(), failure);
    }
    peerPushStreams_.clear();
    finishAll(failure);
    terminal_ = true;
    running_ = false;
    if (lifecycleNotification_.notify != nullptr) {
        lifecycleNotification_.notify(lifecycleNotification_.context);
    }
}

Task<bool> Http3ClientConnection::driveEndpoint(
    const asio::ip::udp::endpoint& peer, TimePoint connectDeadline) {
    session_ = makePmrObject<Http3QuicClientSocketSession>(resource_, io_, peer, host_, tls_,
        responseEngine_.localSettings(), initial_version_, enable_early_data_, resource_);
    if (++quic_generation_ == 0) {
        ++quic_generation_;
    }
    bool hadHttp3 = false;
    std::optional<TimePoint> idleDeadline;
    std::size_t consecutiveWorkTicks = 0;
    while (!stopping_) {
        (void)session_->consumeWorkNotification();
        const auto tick = session_->pump();
        const bool rejected_early_streams = replay_rejected_early_streams();
        if (tick.status == Http3QuicClientSocketSession::PumpStatus::kFatal ||
            tick.status == Http3QuicClientSocketSession::PumpStatus::kClosed) {
            break;
        }
        const bool ready = session_->transport().info().state ==
                           ruvia::quic_connection_state::ready;
        hadHttp3 |= ready;
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
        const bool early = !ready && session_->early_data_enabled() &&
                           session_->transport().info().early_data ==
                               ruvia::quic_early_data_state::available;
        const bool protocolProgress = rejected_early_streams ||
                                      ((ready || early) && sweep(tick.criticalStreamsReady, early));
        if (responseEngine_.peerSettings()) {
            session_->remember_resumption_ticket(responseEngine_.peerSettings());
        }
        bool active = false;
        for (const auto& request : requests_) {
            active |= request.response.outcome == Outcome::kPending;
        }
        active |= !pushes_.empty();
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
    cache_path_migration();
    session_->requestStop();
    session_->close();
    co_return !hadHttp3 && !stopping_;
}

bool Http3ClientConnection::sweep(bool requestsMayStart, bool early_data_only) {
    bool progress = false;
    const auto now = Clock::now();
    for (auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending) {
            continue;
        }
        if (request.continueDeadline && now >= *request.continueDeadline) {
            request.continueDeadline.reset();
            if (request.responseState_ != nullptr && request.responseState_->upload && !request.responseState_->upload->output.stopped) {
                request.responseState_->upload->contentReleased = true;
                request.responseState_->upload->output.notifyData();
                progress = true;
            }
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
    progress |= sweepPushes();
    progress |= receivePeerStreams();
    // Read already-available request streams before applying a newly observed
    // GOAWAY cutoff. A final response head is evidence that the request was
    // processed, even if the body stream has not reached FIN yet.
    progress |= receiveRequests();
    progress |= receiveDatagrams();
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
    progress |= sweepPushes();
    progress |= flushPushControl();
    if (requestsMayStart) {
        progress |= driveRequestWriters(early_data_only);
        if (!early_data_only) {
            progress |= driveCriticalOutput();
        }
    }
    return progress;
}

HttpDatagramSessionConfig Http3ClientConnection::datagramConfig(RequestId id) const {
    requireOwnerThread();
    const auto request = find(id);
    if (request == requests_.end() || request->responseState_ == nullptr ||
        !request->responseState_->tunnel || !request->responseState_->tunnel->config.datagrams ||
        !request->writer.streamId() || !session_) {
        return {};
    }
    const auto& peer = responseEngine_.peerSettings();
    const auto limit = session_->transport().max_datagram_payload_size();
    return {
        .http3StreamId = request->writer.streamId(),
        .localH3Datagram = responseEngine_.localSettings().h3Datagram,
        .peerH3Datagram = peer && peer->h3Datagram,
        .quicDatagram = limit != 0,
        .maxQuicPayloadBytes = limit};
}
bool Http3ClientConnection::sendDatagram(RequestId id, std::span<const std::byte> wire) {
    requireOwnerThread();
    const auto request = find(id);
    if (request == requests_.end() || request->responseState_ == nullptr || !request->responseState_->tunnel ||
        !request->responseState_->tunnel->accepted || request->responseState_->tunnel->output.ended || request->responseState_->tunnel->output.stopped || !session_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto queued = session_->transport().write_datagram(wire);
    if (queued == ruvia::quic_datagram_write_status::queued) {
        session_->notifyWork();
        return true;
    }
    if (queued == ruvia::quic_datagram_write_status::dropped) {
        return false;
    }
    throw std::runtime_error("HTTP Datagram transport is unavailable");
}
bool Http3ClientConnection::receiveDatagrams() {
    bool progress{};
    auto& transport = session_->transport();
    const auto failConnection = [&] {
        terminalFailure_ = Outcome::kProtocolError;
        static constexpr std::string_view reason = "invalid HTTP/3 Datagram";
        (void)transport.close({.kind = ruvia::quic_close_kind::application,
            .code = static_cast<std::uint64_t>(kHttp3DatagramErrorCode),
            .reason = {reason.data(), reason.size()}});
        session_->notifyWork();
        throw std::runtime_error("invalid HTTP/3 Datagram");
    };
    std::array<std::byte, 65536> wire{};
    for (std::size_t count = 0; count < ruvia::quic_limits{}.max_datagrams; ++count) {
        const auto datagram = transport.read_datagram(wire);
        if (datagram.status == ruvia::quic_datagram_status::would_block ||
            datagram.status == ruvia::quic_datagram_status::unavailable) {
            break;
        }
        if (datagram.status == ruvia::quic_datagram_status::too_large ||
            datagram.size > wire.size()) {
            continue;
        }
        progress = true;
        const auto bytes = std::span<const char>(
            reinterpret_cast<const char*>(wire.data()), datagram.size);
        const auto decoded = decodeHttp3Datagram(bytes);
        if ((decoded.index() != 0)) {
            failConnection();
        }
        const auto request = std::find_if(requests_.begin(), requests_.end(), [&](const auto& current) {
            return current.writer.streamId() && *current.writer.streamId() == std::get<0>(decoded).streamId && !current.streamRetired;
        });
        auto* state = request == requests_.end() ? nullptr : request->responseState_;
        const auto planned = planHttp3DatagramReceive(std::get<0>(decoded),
            {.localH3Datagram = responseEngine_.localSettings().h3Datagram,
                .streamExists = request != requests_.end(),
                .receiveOpen = state != nullptr && !state->receiveComplete() && !state->abandoned,
                .supportsDatagrams = state != nullptr && state->tunnel && state->tunnel->config.datagrams});
        if (planned == Http3DatagramReceiveStatus::kConnectionError) {
            failConnection();
        }
        if (planned == Http3DatagramReceiveStatus::kStreamError) {
            (void)transport.reset_stream(std::get<0>(decoded).streamId, kHttp3DatagramErrorCode);
            finishRequest(*request, Outcome::kProtocolError);
        } else if (planned == Http3DatagramReceiveStatus::kDeliver && state->tunnel->accepted &&
                   state->tunnel->datagrams.size() < ruvia::quic_limits{}.max_datagrams) {
            state->tunnel->datagrams.emplace_back(bytes.data(), bytes.size());
            state->dataSignal.notify();
        }
    }
    return progress;
}

bool Http3ClientConnection::replay_rejected_early_streams() {
    std::array<std::uint64_t, 32> rejected{};
    bool progress = false;
    while (true) {
        const auto count = session_->take_rejected_early_streams(rejected);
        for (std::size_t index = 0; index < count; ++index) {
            const auto stream_id = rejected[index];
            const auto request = std::find_if(requests_.begin(), requests_.end(),
                [stream_id](const Request& candidate) {
                    return candidate.writer.streamId() == stream_id;
                });
            if (request == requests_.end() || request->response.outcome != Outcome::kPending) {
                continue;
            }
            const bool response_started = request->responseState_ != nullptr &&
                                          request->responseState_->http3ResponseStarted;
            if (!request->early_data_eligible || response_started ||
                !request->responseParserRegistered ||
                !responseEngine_.cancelRequest(stream_id)) {
                throw std::runtime_error("HTTP/3 rejected an ineligible or committed 0-RTT request");
            }
            receiver_.retire(stream_id);
            if (!responseEngine_.release(stream_id)) {
                throw std::logic_error("HTTP/3 rejected 0-RTT response state could not be retired");
            }
            request->responseParserRegistered = false;
            request->streamRetired = true;
            request->writeDeadline.reset();
            if (request->cancelRequested) {
                finishRequest(*request, Outcome::kCancelled);
                progress = true;
                continue;
            }
            if (!request->writer.replay_after_rejected_early_stream(
                    "https", authority_, resource_)) {
                finishRequest(*request, Outcome::kTransportError);
            } else {
                request->streamRetired = false;
            }
            progress = true;
        }
        if (count < rejected.size()) {
            break;
        }
    }
    return progress;
}

bool Http3ClientConnection::receivePeerStreams() {
    auto& quic = session_->transport();
    const auto accepted = quic.accept_streams();
    if (accepted.status != ruvia::quic_operation_status::accepted &&
        accepted.status != ruvia::quic_operation_status::need_input &&
        accepted.status != ruvia::quic_operation_status::would_block) {
        throw std::runtime_error("QUIC client cannot accept peer critical streams");
    }
    for (std::size_t i = 0; i < accepted.size; ++i) {
        const auto& stream = accepted.streams[i];
        if (!stream.readable || stream.writable ||
            peerStreams_.size() == ruvia::quic_limits{}.max_streams) {
            throw std::runtime_error("invalid or excessive peer HTTP/3 stream");
        }
        peerStreams_.push_back(stream.stream_id);
    }
    bool progress = accepted.size != 0;
    for (std::size_t index = 0; index < peerStreams_.size();) {
        const auto id = peerStreams_[index];
        auto push = findPushByStream(id);
        std::size_t readBudget = Http3ClientReceiveDriver::kReadBlockBytes;
        if (push != pushes_.end() && push->delivery) {
            const auto allowance = push->delivery->readAllowance(readBudget);
            if (allowance.status == Http3ClientResponseDelivery::ReadStatus::kBackpressured ||
                allowance.status == Http3ClientResponseDelivery::ReadStatus::kTerminal) {
                ++index;
                continue;
            }
            if (allowance.status == Http3ClientResponseDelivery::ReadStatus::kRetirementRequired) {
                finishPush(push, Outcome::kProtocolError);
                progress = true;
                continue;
            }
            readBudget = allowance.bytes;
        }
        const auto health = quic.read_health(id);
        const auto part = health.status == ruvia::quic_stream_read_status::reset
                              ? receiver_.acceptReset(id, health.peer_reset_error_code)
                              : receiver_.drive(id, [&quic](std::uint64_t streamId, std::span<char> bytes) { return quic.read_stream(streamId, std::as_writable_bytes(bytes)); }, readBudget);
        // The prefix may have associated this physical stream during feed.
        push = findPushByStream(id);
        if (part.status == Http3ClientReceiveDriver::Status::kConnectionError ||
            part.status == Http3ClientReceiveDriver::Status::kTransportError) {
            terminalFailure_ = part.status == Http3ClientReceiveDriver::Status::kConnectionError
                                   ? Outcome::kProtocolError
                                   : Outcome::kTransportError;
            throw std::runtime_error("HTTP/3 peer stream failed");
        }
        if (push != pushes_.end() && (part.status == Http3ClientReceiveDriver::Status::kResponseComplete ||
                                         part.status == Http3ClientReceiveDriver::Status::kStreamReset ||
                                         part.status == Http3ClientReceiveDriver::Status::kStreamError ||
                                         part.status == Http3ClientReceiveDriver::Status::kPeerStreamEnded ||
                                         (push->delivery && push->delivery->retirementReason() != Http3ClientResponseDelivery::RetirementReason::kNone))) {
            finishPush(push, part.status == Http3ClientReceiveDriver::Status::kResponseComplete ? Outcome::kComplete : Outcome::kProtocolError);
            progress = true;
            continue;
        }
        if (part.status == Http3ClientReceiveDriver::Status::kPeerStreamEnded ||
            part.status == Http3ClientReceiveDriver::Status::kStreamReset ||
            part.status == Http3ClientReceiveDriver::Status::kStreamError) {
            const auto closed = quic.close_stream(id);
            if (closed != ruvia::quic_operation_status::accepted &&
                closed != ruvia::quic_operation_status::completed &&
                closed != ruvia::quic_operation_status::retired) {
                throw std::runtime_error("HTTP/3 peer stream could not close");
            }
            receiver_.retire(id);
            std::erase_if(peerPushStreams_, [id](const PeerPushStream& binding) { return binding.streamId == id; });
            peerStreams_.erase(peerStreams_.begin() + static_cast<std::ptrdiff_t>(index));
            progress = true;
        } else if (part.status == Http3ClientReceiveDriver::Status::kResponseComplete) {
            throw std::runtime_error("HTTP/3 push response has no promised owner");
        } else {
            progress |= part.status == Http3ClientReceiveDriver::Status::kProgress;
            ++index;
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
        if (request.responseState_ != nullptr && request.responseState_->tunnel && request.responseState_->tunnel->accepted && request.responseState_->tunnel->receiveEnded) {
            if (request.writer.finished()) {
                finishRequest(request, Outcome::kComplete);
                progress = true;
            }
            continue;
        }
        const auto health = quic.read_health(*request.writer.streamId());
        if (health.status == ruvia::quic_stream_read_status::reset) {
            const auto reset = receiver_.acceptReset(*request.writer.streamId(), health.peer_reset_error_code);
            if (reset.status != Http3ClientReceiveDriver::Status::kStreamReset) {
                throw std::runtime_error("HTTP/3 reset failed to retire blocked response");
            }
            request.response.peerResetErrorCode = reset.peer_reset_error_code;
            finishRequest(request, reset.peerReportsUnprocessed ? Outcome::kRequestRejected : Outcome::kProtocolError);
            progress = true;
            continue;
        }
        if (health.status != ruvia::quic_stream_read_status::would_block &&
            health.status != ruvia::quic_stream_read_status::fin &&
            health.status != ruvia::quic_stream_read_status::data) {
            throw std::runtime_error("HTTP/3 response receive transport failed");
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
        const auto part = receiver_.drive(*request.writer.streamId(), [&quic](std::uint64_t id, std::span<char> bytes) { return quic.read_stream(id, std::as_writable_bytes(bytes)); }, readBudget);
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
                if (request.responseState_ != nullptr && request.responseState_->tunnel && request.responseState_->tunnel->accepted) {
                    request.responseState_->tunnel->receiveEnded = true;
                    request.responseState_->dataSignal.notify();
                }
                if (request.responseState_ == nullptr || !request.responseState_->tunnel || !request.responseState_->tunnel->accepted || request.writer.finished()) {
                    finishRequest(request, Outcome::kComplete);
                }
                progress = true;
                break;
            case Http3ClientReceiveDriver::Status::kStreamReset:
                request.response.peerResetErrorCode = part.peer_reset_error_code;
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

bool Http3ClientConnection::driveCriticalOutput() {
    bool progress = false;
    for (std::size_t i = 0; i < criticalOutput_.size(); ++i) {
        if (criticalWriteDeadlines_[i] && Clock::now() >= *criticalWriteDeadlines_[i]) {
            terminalFailure_ = Outcome::kDeadline;
            throw std::runtime_error("HTTP/3 critical stream write timeout");
        }
        auto& output = criticalOutput_[i];
        auto& offset = criticalOutputOffset_[i];
        if (output.empty()) {
            const auto pending = i == 0 ? responseEngine_.pendingEncoderOutput() : i == 1 ? responseEngine_.pendingDecoderOutput()
                                                                                          : responseEngine_.pendingControlOutput();
            if (pending.empty()) {
                continue;
            }
            output.assign(pending.data(), std::min(pending.size(), std::size_t{16 * 1024}));
            offset = 0;
            if (writeTimeout_ && !criticalWriteDeadlines_[i]) {
                criticalWriteDeadlines_[i] = deadlineAfter(Clock::now(), *writeTimeout_);
            }
        }
        const auto remaining = std::span<const char>(output.data(), output.size()).subspan(offset);
        const auto kind = i == 0 ? ruvia::http3_critical_stream_output::stream_kind::qpack_encoder : i == 1 ? ruvia::http3_critical_stream_output::stream_kind::qpack_decoder
                                                                                                            : ruvia::http3_critical_stream_output::stream_kind::control;
        const auto sent = session_->writeCriticalStream(kind, remaining);
        if (sent.status == ruvia::quic_operation_status::would_block ||
            sent.status == ruvia::quic_operation_status::need_input) {
            continue;
        }
        if (sent.status != ruvia::quic_operation_status::accepted ||
            sent.accepted > remaining.size()) {
            throw std::runtime_error("HTTP/3 critical stream write failed");
        }
        const bool consumed = i == 0 ? responseEngine_.consumeEncoderOutput(sent.accepted) : i == 1 ? responseEngine_.consumeDecoderOutput(sent.accepted)
                                                                                                    : responseEngine_.consumeControlOutput(sent.accepted);
        if (!consumed) {
            throw std::logic_error("HTTP/3 critical stream acknowledgement mismatch");
        }
        offset += sent.accepted;
        progress |= sent.accepted != 0;
        if (sent.accepted != 0 && writeTimeout_) {
            criticalWriteDeadlines_[i] = deadlineAfter(Clock::now(), *writeTimeout_);
        }
        if (offset == output.size()) {
            std::pmr::string(output.get_allocator()).swap(output);
            offset = 0;
            criticalWriteDeadlines_[i].reset();
        }
    }
    return progress;
}

bool Http3ClientConnection::driveRequestWriters(bool early_data_only) {
    bool progress = false;
    auto& quic = session_->transport();
    for (auto& request : requests_) {
        if (request.response.outcome != Outcome::kPending ||
            (early_data_only && !request.early_data_eligible)) {
            continue;
        }
        // Response priority remains meaningful after the request FIN. In
        // particular, an accepted 0-RTT writer can already be finished when its
        // deferred control output becomes eligible at handshake completion.
        if (!early_data_only && request.pending_priority_update &&
            request.responseParserRegistered && request.writer.streamId()) {
            if (!responseEngine_.queuePriorityUpdate(
                    *request.writer.streamId(), *request.pending_priority_update)) {
                throw std::runtime_error("HTTP/3 request priority update could not be queued");
            }
            request.pending_priority_update.reset();
            progress = true;
        }
        if (request.writer.finished()) {
            continue;
        }
        if (draining_ && !request.writer.streamId()) {
            // Local admission stopped before a stream or any request bytes
            // existed. Preserve the same handoff path as explicit peer rejection.
            finishRequest(request, Outcome::kRequestRejected);
            progress = true;
            continue;
        }
        if (request.writer.requiresConnectSettings()) {
            if (!responseEngine_.peerSettings()) {
                continue;
            }
            if (!responseEngine_.peerSettings()->enableConnectProtocol) {
                finishRequest(request, Outcome::kInvalidRequest);
                progress = true;
                continue;
            }
        }
        auto* upload = request.responseState_ != nullptr ? request.responseState_->output() : nullptr;
        if (upload != nullptr && upload->stopped && !upload->ended) {
            if (const auto id = request.writer.streamId()) {
                const auto reset = quic.reset_stream(
                    *id, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kNoError));
                if (reset == ruvia::quic_operation_status::would_block ||
                    reset == ruvia::quic_operation_status::need_input) {
                    continue;
                }
                if (reset != ruvia::quic_operation_status::accepted) {
                    throw std::runtime_error("HTTP/3 upload send reset failed");
                }
            }
            request.writer.stopSending();
            request.writeDeadline.reset();
            request.continueDeadline.reset();
            progress = true;
            continue;
        }
        if (writeTimeout_ && !request.writeDeadline && !request.writer.waitingForContent()) {
            request.writeDeadline = deadlineAfter(Clock::now(), *writeTimeout_);
        }
        const auto write = request.writer.drive(
            [&quic] { return quic.open_stream(false); },
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
                return request.writer.prepareConnectionHead(id, responseEngine_);
            },
            [&quic](std::uint64_t id, std::span<const char> bytes) {
                return quic.write_stream(id, std::as_bytes(bytes));
            },
            [&quic](std::uint64_t id) { return quic.finish_stream(id); });
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
        if (request.writer.waitingForContent()) {
            request.writeDeadline.reset();
            if (request.responseState_ != nullptr && request.responseState_->upload && !request.responseState_->upload->contentReleased && !request.continueDeadline) {
                request.continueDeadline = deadlineAfter(Clock::now(), std::chrono::duration_cast<Clock::duration>(request.responseState_->upload->config.continueTimeout));
            }
        }
    }
    return progress;
}

}  // namespace ruvia::detail
