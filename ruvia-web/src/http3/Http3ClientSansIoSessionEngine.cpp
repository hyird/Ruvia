#include "ruvia/web/detail/http3/Http3ClientSansIoSessionEngine.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3PeerStreams.h"

namespace ruvia::detail {
namespace {
Http3ClientSansIoSessionResult connectionError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ClientSansIoSessionStatus::kConnectionError,
        Http3ConnectionErrorScope::kConnection, code};
}

std::pmr::memory_resource* checkedResource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/3 client session requires its worker memory resource");
    }
    return resource;
}
}  // namespace

Http3ClientSansIoSessionEngine::Http3ClientSansIoSessionEngine(
    std::pmr::memory_resource* workerResource, Limits limits)
    : resource_(checkedResource(workerResource)),
      limits_(limits),
      localBodyBudget_(limits.maxTotalBodyBytes),
      bodyBudget_(&localBodyBudget_),
      connection_(Http3PeerRole::kClient, resource_, limits.connection),
      responses_(resource_) {
    if (limits_.maxLiveStreams == 0) {
        throw std::invalid_argument("HTTP/3 response session must allow at least one live stream");
    }
}

Http3ClientSansIoSessionEngine::Http3ClientSansIoSessionEngine(
    std::pmr::memory_resource* workerResource, Http3ClientBodyBudget& bodyBudget, Limits limits)
    : Http3ClientSansIoSessionEngine(workerResource, limits) {
    bodyBudget_ = &bodyBudget;
}

Http3ClientSansIoSessionEngine::~Http3ClientSansIoSessionEngine() {
    bodyBudget_->release(retainedBodyBytes_);
}

Http3ClientSansIoSessionEngine::Result Http3ClientSansIoSessionEngine::registerRequest(
    std::uint64_t streamId, HttpKnownMethod method, Http3ClientResponseEventSink sink) {
    if (feeding_) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    if (connectionFailure_.scope == Http3ConnectionErrorScope::kConnection) {
        return connectionFailure_;
    }
    if (responses_.contains(streamId)) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    if (responses_.size() >= limits_.maxLiveStreams) {
        return {Http3ClientSansIoSessionStatus::kStreamLimitExceeded,
            Http3ConnectionErrorScope::kStream, Http3ConnectionErrorCode::kExcessiveLoad};
    }
    auto [entry, inserted] = responses_.try_emplace(streamId, resource_);
    if (!inserted) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    entry->second.sink = sink;
    try {
        const auto registered = connection_.registerClientRequest(streamId, method);
        if (registered.scope != Http3ConnectionErrorScope::kNone) {
            responses_.erase(entry);
            const auto failure = fromConnection(registered);
            if (failure.scope == Http3ConnectionErrorScope::kConnection) {
                failConnection(failure);
            }
            return failure;
        }
    } catch (...) {
        responses_.erase(entry);
        failConnection(connectionError(Http3ConnectionErrorCode::kInternalError));
        throw;
    }
    return {};
}

Http3ClientSansIoSessionEngine::Result Http3ClientSansIoSessionEngine::feed(
    std::uint64_t streamId, std::span<const char> bytes, bool fin, bool reset) {
    if (feeding_) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    if (connectionFailure_.scope == Http3ConnectionErrorScope::kConnection) {
        return connectionFailure_;
    }
    auto found = responses_.find(streamId);
    if (isHttp3RequestStreamId(streamId) &&
        (found == responses_.end() || found->second.terminal)) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    feeding_ = true;
    struct FeedGuard final {
        bool& feeding;
        ~FeedGuard() {
            feeding = false;
        }
    } guard{feeding_};
    try {
        const auto result = connection_.feed(streamId, bytes, fin, reset, onEvent, this);
        if (result.scope == Http3ConnectionErrorScope::kConnection) {
            failConnection(fromConnection(result));
            return connectionFailure_;
        }
        if (found == responses_.end()) {
            return fromConnection(result);  // Peer control/QPACK or invalid peer stream.
        }
        auto& stored = found->second;
        if (stored.limitExceeded) {
            // A later stream error in the same feed must not hide a body
            // overflow that requires closing the entire connection. Only a
            // protocol connection error has higher priority.
            failConnection(connectionError(Http3ConnectionErrorCode::kExcessiveLoad));
            stored.result = {Http3ClientSansIoSessionStatus::kBodyLimitExceeded,
                Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kExcessiveLoad};
            return stored.result;
        }
        if (result.scope == Http3ConnectionErrorScope::kStream) {
            stored.result = fromConnection(result);
            stored.terminal = true;
            return stored.result;
        }
        if (result.status == Http3ConnectionStatus::kMessageEnd) {
            stored.complete = true;
            stored.terminal = true;
            stored.result = {Http3ClientSansIoSessionStatus::kMessageEnd};
            return stored.result;
        }
        if (result.status == Http3ConnectionStatus::kReset) {
            stored.result = {Http3ClientSansIoSessionStatus::kReset};
            stored.reset = true;
            stored.terminal = true;
            return stored.result;
        }
        return {};
    } catch (...) {
        failConnection(connectionError(Http3ConnectionErrorCode::kInternalError));
        throw;
    }
}

void Http3ClientSansIoSessionEngine::onEvent(void* context, const Http3ConnectionEvent& event) {
    auto& self = *static_cast<Http3ClientSansIoSessionEngine*>(context);
    auto found = self.responses_.find(event.streamId);
    if (found == self.responses_.end()) {
        return;
    }
    auto& response = found->second;
    switch (event.kind) {
        case Http3ConnectionEventKind::kFinalHead:
            if (event.head == nullptr) {
                return;
            }
            response.responseBodyPlan = event.responseBodyPlan;
            response.status = event.head->status;
            response.headers.clear();
            response.headers.reserve(event.head->headers.size());
            for (const auto& field : event.head->headers) {
                Http3ClientSansIoResponseHeader copied(self.resource_);
                copied.name.assign(field.name);
                copied.value.assign(field.value);
                response.headers.push_back(std::move(copied));
            }
            response.finalHeadSeen = true;
            if (response.sink.callback) {
                response.sink.callback(response.sink.context, event);
            }
            return;
        case Http3ConnectionEventKind::kTrailerField: {
            Http3ClientSansIoResponseHeader copied(self.resource_);
            copied.name.assign(event.trailer.name);
            copied.value.assign(event.trailer.value);
            response.trailers.push_back(std::move(copied));
            if (response.sink.callback) {
                response.sink.callback(response.sink.context, event);
            }
            return;
        }
        case Http3ConnectionEventKind::kBody:
        case Http3ConnectionEventKind::kTunnelData: {
            if (response.limitExceeded) {
                return;
            }
            if (response.sink.callback) {
                response.sink.callback(response.sink.context, event);
                return;
            }
            const auto size = event.body.size();
            if (size > self.limits_.maxBodyBytesPerStream -
                           std::min(response.body.size(), self.limits_.maxBodyBytesPerStream) ||
                size > self.limits_.maxTotalBodyBytes -
                           std::min(self.retainedBodyBytes_, self.limits_.maxTotalBodyBytes) ||
                !self.bodyBudget_->tryRetain(size)) {
                response.result = {Http3ClientSansIoSessionStatus::kBodyLimitExceeded,
                    Http3ConnectionErrorScope::kStream, Http3ConnectionErrorCode::kExcessiveLoad};
                response.limitExceeded = true;
                return;
            }
            try {
                response.body.insert(response.body.end(), event.body.begin(), event.body.end());
            } catch (...) {
                self.bodyBudget_->release(size);
                throw;
            }
            self.retainedBodyBytes_ += size;
            return;
        }
        case Http3ConnectionEventKind::kMessageEnd:
            // The final plan is a value from the protocol layer and is repeated
            // unchanged here. Do not let an absent value erase the observed head.
            if (event.responseBodyPlan) {
                response.responseBodyPlan = event.responseBodyPlan;
            }
            // Feed can still report a later connection error for the same
            // input; only the successful return publishes a complete response.
            return;
        case Http3ConnectionEventKind::kReset:
            // Publish only after feed() confirms there was no higher-priority
            // connection error in this same input batch.
            return;
        case Http3ConnectionEventKind::kInformationalHead:
            if (response.sink.callback) {
                response.sink.callback(response.sink.context, event);
            }
            return;
        case Http3ConnectionEventKind::kPushPromise:
        case Http3ConnectionEventKind::kPushCanceled:
        case Http3ConnectionEventKind::kOriginAdvertisement:
        case Http3ConnectionEventKind::kPriorityUpdate:
        case Http3ConnectionEventKind::kRequestHead:
            return;
    }
}

std::optional<Http3ClientSansIoResponseView> Http3ClientSansIoSessionEngine::response(
    std::uint64_t streamId) const noexcept {
    const auto found = responses_.find(streamId);
    if (found == responses_.end() || !found->second.terminal) {
        return std::nullopt;
    }
    const auto& response = found->second;
    return Http3ClientSansIoResponseView{response.status, response.responseBodyPlan,
        response.headers, response.trailers, response.body, response.complete,
        response.reset, response.result};
}

std::optional<std::pmr::string> Http3ClientSansIoSessionEngine::takeBody(std::uint64_t streamId) {
    if (feeding_) {
        return std::nullopt;
    }
    const auto found = responses_.find(streamId);
    if (found == responses_.end()) {
        return std::nullopt;
    }
    auto& stored = found->second;
    if (!stored.complete || stored.bodyTransferred || stored.sink.callback) {
        return std::nullopt;
    }
    const auto bytes = stored.body.size();
    std::optional<std::pmr::string> body(std::in_place, std::move(stored.body));
    stored.body.clear();
    stored.bodyTransferred = true;
    retainedBodyBytes_ -= bytes;
    bodyBudget_->release(bytes);
    return body;
}

bool Http3ClientSansIoSessionEngine::release(std::uint64_t streamId) noexcept {
    if (feeding_) {
        return false;
    }
    const auto found = responses_.find(streamId);
    if (found == responses_.end() || !found->second.terminal) {
        return false;
    }
    retainedBodyBytes_ -= found->second.body.size();
    bodyBudget_->release(found->second.body.size());
    responses_.erase(found);
    return true;
}

bool Http3ClientSansIoSessionEngine::cancelRequest(std::uint64_t streamId) noexcept {
    if (feeding_ || connectionFailure_.scope == Http3ConnectionErrorScope::kConnection) {
        return false;
    }
    const auto found = responses_.find(streamId);
    if (found == responses_.end() || found->second.terminal) {
        return false;
    }
    if (!connection_.retireClientRequest(streamId)) {
        failConnection(connectionError(Http3ConnectionErrorCode::kInternalError));
        return false;
    }
    found->second.terminal = true;
    found->second.result = {Http3ClientSansIoSessionStatus::kLocalCancelled};
    return true;
}

void Http3ClientSansIoSessionEngine::failConnection(Result failure) noexcept {
    if (connectionFailure_.scope == Http3ConnectionErrorScope::kConnection) {
        return;
    }
    connectionFailure_ = failure;
    for (auto& [id, response] : responses_) {
        (void)id;
        if (!response.terminal) {
            response.result = failure;
            response.terminal = true;
        }
    }
}

std::size_t Http3ClientSansIoSessionEngine::liveStreamCount() const noexcept {
    return responses_.size();
}

std::size_t Http3ClientSansIoSessionEngine::retainedBodyBytes() const noexcept {
    return retainedBodyBytes_;
}

Http3ClientSansIoSessionEngine::Result Http3ClientSansIoSessionEngine::stop() noexcept {
    if (feeding_) {
        return {Http3ClientSansIoSessionStatus::kInvalidState};
    }
    (void)connection_.retire();
    failConnection({Http3ClientSansIoSessionStatus::kTransportError,
        Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kNoError});
    return connectionFailure_;
}

Http3ClientSansIoSessionEngine::Result Http3ClientSansIoSessionEngine::fromConnection(
    Http3ConnectionResult result) const noexcept {
    switch (result.status) {
        case Http3ConnectionStatus::kQpackBlocked:
        case Http3ConnectionStatus::kPushPromisePending:
            return {Http3ClientSansIoSessionStatus::kConnectionError, Http3ConnectionErrorScope::kConnection,
                Http3ConnectionErrorCode::kInternalError};
        case Http3ConnectionStatus::kNeedMoreData:
            return {};
        case Http3ConnectionStatus::kMessageEnd:
            return {Http3ClientSansIoSessionStatus::kMessageEnd, result.scope, result.code};
        case Http3ConnectionStatus::kReset:
            return {Http3ClientSansIoSessionStatus::kReset, result.scope, result.code};
        case Http3ConnectionStatus::kStreamError:
            return {Http3ClientSansIoSessionStatus::kStreamError, result.scope, result.code};
        case Http3ConnectionStatus::kConnectionError:
            return {Http3ClientSansIoSessionStatus::kConnectionError, result.scope, result.code};
    }
    return {};
}

}  // namespace ruvia::detail
