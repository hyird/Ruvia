#include "ruvia/http/Http3Connection.h"

#include <algorithm>
#include <array>
#include <memory_resource>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/Http3ControlStream.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3QpackStreams.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpResponseStream.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"

#include "http3/http3_trailer_collector.h"

namespace ruvia {
namespace {

Http3ConnectionResult connectionError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ConnectionStatus::kConnectionError, Http3ConnectionErrorScope::kConnection, code};
}

Http3ConnectionResult streamError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ConnectionStatus::kStreamError, Http3ConnectionErrorScope::kStream, code};
}

Http3ConnectionResult requestError(Http3ConnectionErrorCode code) noexcept {
    if (code == Http3ConnectionErrorCode::kQpackDecompressionFailed ||
        code == Http3ConnectionErrorCode::kExcessiveLoad) {
        return connectionError(code);
    }
    return streamError(code);
}

Http3ConnectionErrorCode mapPeerError(Http3PeerStreamError error) noexcept {
    switch (error) {
        case Http3PeerStreamError::kStreamCreationError:
            return Http3ConnectionErrorCode::kStreamCreationError;
        case Http3PeerStreamError::kClosedCriticalStream:
            return Http3ConnectionErrorCode::kClosedCriticalStream;
        case Http3PeerStreamError::kExcessiveLoad:
            return Http3ConnectionErrorCode::kExcessiveLoad;
    }
    return Http3ConnectionErrorCode::kGeneralProtocolError;
}

Http3ConnectionErrorCode mapControlError(Http3ControlStreamStatus status) noexcept {
    switch (status) {
        case Http3ControlStreamStatus::kClosedCriticalStream:
            return Http3ConnectionErrorCode::kClosedCriticalStream;
        case Http3ControlStreamStatus::kMissingSettings:
            return Http3ConnectionErrorCode::kMissingSettings;
        case Http3ControlStreamStatus::kFrameUnexpected:
            return Http3ConnectionErrorCode::kFrameUnexpected;
        case Http3ControlStreamStatus::kSettingsError:
            return Http3ConnectionErrorCode::kSettingsError;
        case Http3ControlStreamStatus::kIdError:
            return Http3ConnectionErrorCode::kIdError;
        case Http3ControlStreamStatus::kFrameError:
            return Http3ConnectionErrorCode::kFrameError;
        case Http3ControlStreamStatus::kLimit:
            return Http3ConnectionErrorCode::kExcessiveLoad;
        case Http3ControlStreamStatus::kNeedMoreData:
            break;
    }
    return Http3ConnectionErrorCode::kNoError;
}

bool valid_request_trailer_policy(Http3FieldSectionFieldView field) noexcept {
    return isValidHttpHeaderValue(field.value) &&
           !detail::isForbiddenHttpRequestTrailerName(field.name);
}

}  // namespace

struct Http3Connection::Impl final {
    struct Request final {
        Request(std::pmr::memory_resource* resource, const Http3ConnectionConfig& limits, Http3QpackDecoder& decoder)
            : frames(Http3StreamKind::kRequest, resource,
                  {.maxFieldSectionSize = limits.maxEncodedFieldSectionBytes}),
              resource(resource),
              limits(limits),
              decoder(decoder) {}

        Http3StreamFrames frames;
        std::pmr::memory_resource* resource;
        Http3ConnectionConfig limits;
        Http3QpackDecoder& decoder;
        std::optional<Http3MessageBody> body;
        bool connect{false};
        bool terminal{false};
        Http3ConnectionResult callbackResult{};
        Http3ConnectionCallback callback{nullptr};
        void* callbackContext{nullptr};
        std::uint64_t streamId{0};
    };

    struct ClientRequest final {
        ClientRequest(std::uint64_t streamId, HttpKnownMethod method, std::pmr::memory_resource* resource,
            const Http3ConnectionConfig& limits, Http3QpackDecoder& decoder, Impl& owner)
            : response(streamId, method, resource,
                  {.maxFieldSectionSize = limits.maxFieldSectionSize,
                      .maxFields = limits.maxFields,
                      .maxEncodedFieldSectionBytes = limits.maxEncodedFieldSectionBytes,
                      .maxPushId = limits.maxPushId},
                  &decoder),
              owner(owner) {}
        Http3ClientResponse response;
        Impl& owner;
        bool responseObserved{false};
        Http3ConnectionCallback callback{nullptr};
        void* callbackContext{nullptr};
    };

    using trailer_collector = detail::http3_trailer_collector<valid_request_trailer_policy>;

    struct FeedGuard final {
        explicit FeedGuard(Impl& impl)
            : impl(impl) {
            if (impl.feeding) {
                throw std::logic_error("recursive Http3Connection::feed()");
            }
            impl.feeding = true;
        }
        ~FeedGuard() {
            impl.feeding = false;
        }
        Impl& impl;
    };

    Impl(Http3PeerRole role, std::pmr::memory_resource* resource, Http3ConnectionConfig limits)
        : role(role),
          resource(resource),
          limits(limits),
          peerStreams(role, resource, {.maxActiveStreams = limits.maxPeerUnidirectionalStreams}),
          requests(resource),
          clientRequests(resource),
          pushes(resource),
          pushStreams(resource),
          control(role == Http3PeerRole::kServer ? Http3ControlRole::kServer : Http3ControlRole::kClient,
              resource,
              {.maxFieldSectionSize = limits.maxEncodedFieldSectionBytes}),
          receiveQpack({.maxTableCapacity = limits.qpackMaxTableCapacity,
                           .maxBlockedStreams = limits.qpackBlockedStreams,
                           .fields = {limits.maxEncodedFieldSectionBytes, limits.maxFieldSectionSize, limits.maxFields}},
              resource),
          transmitQpack(std::in_place, Http3QpackEncoderConfig{.maxTableCapacity = 0, .maxBlockedStreams = 0}, resource) {}

    [[nodiscard]] Http3FieldSectionLimits local_field_limits(Http3FieldSectionLimits requested) const noexcept {
        requested.maxEncodedBytes = std::min(requested.maxEncodedBytes, limits.maxEncodedFieldSectionBytes);
        requested.maxDecodedBytes = std::min(requested.maxDecodedBytes, limits.maxFieldSectionSize);
        requested.maxFields = std::min(requested.maxFields, limits.maxFields);
        return requested;
    }

    [[nodiscard]] Http3FieldSectionLimits outbound_field_limits(Http3FieldSectionLimits requested) const noexcept {
        requested = local_field_limits(requested);
        // SETTINGS_MAX_FIELD_SECTION_SIZE limits decoded names, values and the
        // per-field overhead, not compressed QPACK bytes. Before SETTINGS (or
        // when the setting is absent), only the local budget applies.
        const auto& settings = control.peerSettings();
        if (settings && settings->maxFieldSectionSize &&
            std::cmp_less(*settings->maxFieldSectionSize, requested.maxDecodedBytes)) {
            requested.maxDecodedBytes = static_cast<std::size_t>(*settings->maxFieldSectionSize);
        }
        return requested;
    }

    template <typename encoded_type>
    [[nodiscard]] std::expected<encoded_type, Http3ResponseHeadFailure> response_encoding_result(
        std::expected<encoded_type, Http3ResponseHeadFailure> result, Http3FieldSectionLimits local_limits) const {
        const auto& settings = control.peerSettings();
        if (!result && result.error().kind == Http3ResponseHeadError::kFieldSectionError &&
            result.error().fieldSectionError == Http3FieldSectionError::kFieldListTooLarge &&
            settings && settings->maxFieldSectionSize &&
            std::cmp_less_equal(*settings->maxFieldSectionSize, local_limits.maxDecodedBytes)) {
            result.error().kind = Http3ResponseHeadError::peer_field_section_limit;
        }
        return result;
    }

    static void onRequestFrame(void* opaque, Http3StreamFrameEvent frame) {
        auto& request = *static_cast<Request*>(opaque);
        if (request.terminal || request.callbackResult.scope != Http3ConnectionErrorScope::kNone) {
            return;
        }
        if (frame.kind == Http3StreamFrameEventKind::kData) {
            if (!frame.payload.empty()) {
                if (request.connect) {
                    const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kTunnelData,
                        .streamId = request.streamId,
                        .body = frame.payload};
                    request.callback(request.callbackContext, event);
                } else {
                    const auto bodyResult = request.body->feed(frame.payload.size(), false);
                    if (bodyResult == Http3MessageBodyResult::kPayloadNotAllowed ||
                        bodyResult == Http3MessageBodyResult::kContentLengthExceeded ||
                        bodyResult == Http3MessageBodyResult::kLengthOverflow) {
                        request.callbackResult = streamError(Http3ConnectionErrorCode::kMessageError);
                        request.terminal = true;
                        return;
                    }
                    const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kBody,
                        .streamId = request.streamId,
                        .body = frame.payload};
                    request.callback(request.callbackContext, event);
                }
            }
            if (frame.fin) {
                finish(request);
            }
            return;
        }
        if (frame.kind != Http3StreamFrameEventKind::kHeaders || !frame.endFrame) {
            return;
        }
        if (!frame.trailers) {
            const Http3MessageHeadLimits headLimits{.maxFieldSectionSize = request.limits.maxFieldSectionSize,
                .maxFields = request.limits.maxFields,
                .maxEncodedBytes = request.limits.maxEncodedFieldSectionBytes};
            auto result = decodeHttp3MessageHead(request.decoder, request.streamId, frame.payload,
                Http3MessageHeadKind::kRequest, request.resource, headLimits);
            if (result && std::holds_alternative<Http3QpackBlocked>(*result)) {
                request.frames.pause();
                return;
            }
            auto decoded = [&]() -> std::expected<Http3MessageHead, Http3MessageHeadError> {
                if (!result) {
                    return std::unexpected(result.error());
                }
                return std::move(std::get<Http3MessageHead>(*result));
            }();
            if (!decoded) {
                request.callbackResult = requestError(decoded.error() == Http3MessageHeadError::kQpackDecompressionFailed
                                                          ? Http3ConnectionErrorCode::kQpackDecompressionFailed
                                                      : decoded.error() == Http3MessageHeadError::kFieldSectionTooLarge
                                                          ? Http3ConnectionErrorCode::kExcessiveLoad
                                                          : Http3ConnectionErrorCode::kMessageError);
                request.terminal = true;
                return;
            }
            if (!decoded->protocol.empty() && !request.limits.enableConnectProtocol) {
                request.callbackResult = streamError(Http3ConnectionErrorCode::kMessageError);
                request.terminal = true;
                return;
            }
            request.connect = decoded->method == "CONNECT";
            if (!request.connect) {
                request.body.emplace(decoded->contentLength, true);
            }
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kRequestHead,
                .streamId = request.streamId,
                .head = &*decoded};
            request.callback(request.callbackContext, event);
            return;
        }

        if (request.connect) {
            request.callbackResult = streamError(Http3ConnectionErrorCode::kMessageError);
            request.terminal = true;
            return;
        }
        trailer_collector collector(request.resource);
        const auto decoded = request.decoder.decode(request.streamId, frame.payload, trailer_collector::collect, &collector);
        if (decoded && decoded->status == Http3QpackDecodeStatus::kBlocked) {
            request.frames.pause();
            return;
        }
        if (!decoded || !collector.valid_) {
            request.callbackResult = requestError(!decoded
                                                      ? (decoded.error() == Http3QpackConnectionError::kLimit ? Http3ConnectionErrorCode::kExcessiveLoad
                                                                                                              : Http3ConnectionErrorCode::kQpackDecompressionFailed)
                                                      : Http3ConnectionErrorCode::kMessageError);
            request.terminal = true;
            return;
        }
        for (const auto& field : collector.fields_) {
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kTrailerField,
                .streamId = request.streamId,
                .trailer = {field.name_, field.value_, field.never_indexed_}};
            request.callback(request.callbackContext, event);
        }
    }

    static void finish(Request& request) {
        if (request.terminal) {
            return;
        }
        if (!request.connect) {
            const auto result = request.body->feed(0, true);
            if (result == Http3MessageBodyResult::kContentLengthMismatch ||
                result == Http3MessageBodyResult::kPayloadNotAllowed) {
                request.callbackResult = streamError(Http3ConnectionErrorCode::kMessageError);
                request.terminal = true;
                return;
            }
        }
        request.terminal = true;
        const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kMessageEnd,
            .streamId = request.streamId};
        request.callback(request.callbackContext, event);
    }

    static void onClientResponse(void* opaque, const Http3ClientResponseEvent& responseEvent) {
        auto& request = *static_cast<ClientRequest*>(opaque);
        if (responseEvent.kind == Http3ClientResponseEventKind::kPushPromise) {
            if (!request.owner.acceptPromise(*responseEvent.pushId, *responseEvent.head)) {
                return;
            }
        }
        if (responseEvent.kind == Http3ClientResponseEventKind::kInformationalHead ||
            responseEvent.kind == Http3ClientResponseEventKind::kFinalHead) {
            request.responseObserved = true;
        }
        const auto kind = [&] {
            switch (responseEvent.kind) {
                case Http3ClientResponseEventKind::kPushPromise:
                    return Http3ConnectionEventKind::kPushPromise;
                case Http3ClientResponseEventKind::kInformationalHead:
                    return Http3ConnectionEventKind::kInformationalHead;
                case Http3ClientResponseEventKind::kFinalHead:
                    return Http3ConnectionEventKind::kFinalHead;
                case Http3ClientResponseEventKind::kTunnelData:
                    return Http3ConnectionEventKind::kTunnelData;
                case Http3ClientResponseEventKind::kBody:
                    return Http3ConnectionEventKind::kBody;
                case Http3ClientResponseEventKind::kTrailerField:
                    return Http3ConnectionEventKind::kTrailerField;
                case Http3ClientResponseEventKind::kMessageEnd:
                    return Http3ConnectionEventKind::kMessageEnd;
                case Http3ClientResponseEventKind::kReset:
                    return Http3ConnectionEventKind::kReset;
            }
            return Http3ConnectionEventKind::kBody;
        }();
        const Http3ConnectionEvent event{.kind = kind,
            .streamId = responseEvent.streamId,
            .head = responseEvent.head,
            .trailer = responseEvent.trailer,
            .body = responseEvent.body,
            .responseBodyPlan = responseEvent.responseBodyPlan,
            .pushId = responseEvent.pushId,
            .requestContentSignal = responseEvent.requestContentSignal};
        request.callback(request.callbackContext, event);
    }

    bool acceptPromise(std::uint64_t id, const Http3MessageHead& head) {
        if (!pushes.contains(id) && pushes.size() >= limits.maxRememberedPushes) {
            failed = connectionError(Http3ConnectionErrorCode::kExcessiveLoad);
            return false;
        }
        auto& push = pushes[id];
        if (push.head) {
            const auto& prior = *push.head;
            bool same = prior.method == head.method && prior.scheme == head.scheme &&
                        prior.authority == head.authority && prior.path == head.path && prior.headers.size() == head.headers.size();
            for (std::size_t i = 0; same && i < head.headers.size(); ++i) {
                same = prior.headers[i].name == head.headers[i].name && prior.headers[i].value == head.headers[i].value;
            }
            if (!same) {
                failed = connectionError(Http3ConnectionErrorCode::kGeneralProtocolError);
            }
            return same;
        }
        push.head.emplace(resource);
        push.head->method = head.method;
        push.head->scheme = head.scheme;
        push.head->authority = head.authority;
        push.head->path = head.path;
        push.head->contentLength = head.contentLength;
        for (const auto& field : head.headers) {
            push.head->headers.emplace_back(field.name, field.value, resource);
        }
        return true;
    }
    struct ControlCallback final {
        Impl& owner;
        Http3ConnectionCallback callback;
        void* context;
    };
    static void onControl(void* opaque, Http3ControlStreamEvent event) {
        auto& target = *static_cast<ControlCallback*>(opaque);
        auto& owner = target.owner;
        if (owner.failed.scope != Http3ConnectionErrorScope::kNone) {
            return;
        }
        if (event.kind == Http3StreamFrameEventKind::kOrigin) {
            if (owner.limits.receiveOriginAdvertisements && event.originAdvertisement) {
                target.callback(target.context, {.kind = Http3ConnectionEventKind::kOriginAdvertisement,
                                                    .originAdvertisement = event.originAdvertisement});
            }
            return;
        }
        if (event.kind == Http3StreamFrameEventKind::kRequestPriorityUpdate || event.kind == Http3StreamFrameEventKind::kPushPriorityUpdate) {
            if (event.kind == Http3StreamFrameEventKind::kPushPriorityUpdate &&
                (!owner.pushes.contains(event.id) || !owner.pushes.at(event.id).head)) {
                owner.failed = connectionError(Http3ConnectionErrorCode::kIdError);
                return;
            }
            if (!event.priorityUpdate) {
                return;
            }
            target.callback(target.context, {.kind = Http3ConnectionEventKind::kPriorityUpdate,
                                                .streamId = event.priorityUpdate->push ? 0 : event.id,
                                                .pushId = event.priorityUpdate->push ? std::optional(event.id) : std::nullopt,
                                                .priorityUpdate = event.priorityUpdate});
            return;
        }
        if (event.kind != Http3StreamFrameEventKind::kCancelPush) {
            return;
        }
        auto found = owner.pushes.find(event.id);
        const auto maximum = owner.role == Http3PeerRole::kClient ? owner.limits.maxPushId : owner.control.maxPushId();
        if (!maximum || event.id > *maximum ||
            (owner.role == Http3PeerRole::kServer && (found == owner.pushes.end() || !found->second.head))) {
            owner.failed = connectionError(Http3ConnectionErrorCode::kIdError);
            return;
        }
        if (found == owner.pushes.end()) {
            if (owner.pushes.size() >= owner.limits.maxRememberedPushes) {
                owner.failed = connectionError(Http3ConnectionErrorCode::kExcessiveLoad);
                return;
            }
            found = owner.pushes.try_emplace(event.id).first;
        }
        found->second.canceled = true;
        const Http3ConnectionEvent canceled{.kind = Http3ConnectionEventKind::kPushCanceled,
            .streamId = found->second.streamId.value_or(0),
            .pushId = event.id};
        target.callback(target.context, canceled);
    }
    struct PushCallback final {
        Http3ConnectionCallback callback;
        void* context;
        std::uint64_t pushId;
    };
    static void onPushResponse(void* opaque, const Http3ClientResponseEvent& source) {
        const auto& target = *static_cast<PushCallback*>(opaque);
        const auto kind = source.kind == Http3ClientResponseEventKind::kInformationalHead ? Http3ConnectionEventKind::kInformationalHead
                          : source.kind == Http3ClientResponseEventKind::kFinalHead       ? Http3ConnectionEventKind::kFinalHead
                          : source.kind == Http3ClientResponseEventKind::kBody            ? Http3ConnectionEventKind::kBody
                          : source.kind == Http3ClientResponseEventKind::kTrailerField    ? Http3ConnectionEventKind::kTrailerField
                          : source.kind == Http3ClientResponseEventKind::kReset           ? Http3ConnectionEventKind::kReset
                                                                                          : Http3ConnectionEventKind::kMessageEnd;
        const Http3ConnectionEvent event{.kind = kind, .streamId = source.streamId, .head = source.head, .trailer = source.trailer, .body = source.body, .responseBodyPlan = source.responseBodyPlan, .pushId = target.pushId};
        target.callback(target.context, event);
    }
    Http3ConnectionResult feedPush(std::uint64_t streamId, std::span<const char> bytes, bool fin, bool reset,
        Http3ConnectionCallback callback, void* context) {
        if (!limits.maxPushId) {
            return failed = connectionError(Http3ConnectionErrorCode::kIdError);
        }
        if (!pushStreams.contains(streamId) && pushStreams.size() >= limits.maxActiveStreams) {
            return failed = connectionError(Http3ConnectionErrorCode::kExcessiveLoad);
        }
        auto& stream = pushStreams[streamId];
        std::size_t consumed = 0;
        while (!stream.id && consumed < bytes.size() && stream.used < 8) {
            stream.idBytes[stream.used++] = bytes[consumed++];
            const auto id = decodeHttp3VarInt(std::span(stream.idBytes).first(stream.used));
            if (!id) {
                continue;
            }
            if (id->value > *limits.maxPushId) {
                return failed = connectionError(Http3ConnectionErrorCode::kIdError);
            }
            if (!pushes.contains(id->value) && pushes.size() >= limits.maxRememberedPushes) {
                return failed = connectionError(Http3ConnectionErrorCode::kExcessiveLoad);
            }
            auto& push = pushes[id->value];
            if (push.streamId && *push.streamId != streamId) {
                return failed = connectionError(Http3ConnectionErrorCode::kIdError);
            }
            push.streamId = streamId;
            stream.id = id->value;
            callback(context, {.kind = Http3ConnectionEventKind::kPushStream, .streamId = streamId, .pushId = id->value});
        }
        if (!stream.id) {
            if (fin || reset) {
                (void)peerStreams.retireNonCriticalStream(streamId);
                pushStreams.erase(streamId);
                return fin && !reset ? streamError(Http3ConnectionErrorCode::kFrameError) : Http3ConnectionResult{};
            }
            return {};
        }
        auto& push = pushes.at(*stream.id);
        if (reset) {
            const auto id = *stream.id;
            (void)receiveQpack.cancel(streamId);
            push.completed = true;
            (void)peerStreams.retireNonCriticalStream(streamId);
            pushStreams.erase(streamId);
            callback(context, {.kind = Http3ConnectionEventKind::kReset, .streamId = streamId, .pushId = id});
            return {Http3ConnectionStatus::kReset};
        }
        if (push.canceled) {
            (void)receiveQpack.cancel(streamId);
            (void)peerStreams.retireNonCriticalStream(streamId);
            pushStreams.erase(streamId);
            return streamError(Http3ConnectionErrorCode::kRequestCancelled);
        }
        if (!push.head) {
            return {Http3ConnectionStatus::kPushPromisePending, Http3ConnectionErrorScope::kNone,
                Http3ConnectionErrorCode::kNoError, consumed};
        }
        if (!stream.response) {
            stream.response.emplace(streamId, classifyHttpMethod(push.head->method), resource,
                Http3ClientResponseLimits{.maxFieldSectionSize = limits.maxFieldSectionSize, .maxFields = limits.maxFields, .maxEncodedFieldSectionBytes = limits.maxEncodedFieldSectionBytes, .pushStream = true}, &receiveQpack);
        }
        PushCallback target{callback, context, *stream.id};
        const auto result = stream.response->feed(bytes.subspan(consumed), fin, false, onPushResponse, &target);
        if (result.status == Http3ClientResponseStatus::kQpackBlocked) {
            return {Http3ConnectionStatus::kQpackBlocked,
                Http3ConnectionErrorScope::kNone, Http3ConnectionErrorCode::kNoError, consumed + result.consumedBytes};
        }
        if (result.scope == Http3ConnectionErrorScope::kConnection) {
            return failed = connectionError(result.code);
        }
        if (result.status == Http3ClientResponseStatus::kMessageEnd || result.scope == Http3ConnectionErrorScope::kStream) {
            push.completed = true;
            (void)peerStreams.retireNonCriticalStream(streamId);
            pushStreams.erase(streamId);
            return result.scope == Http3ConnectionErrorScope::kStream ? streamError(result.code)
                                                                      : Http3ConnectionResult{Http3ConnectionStatus::kMessageEnd};
        }
        return {};
    }

    Http3PeerRole role;
    std::pmr::memory_resource* resource;
    Http3ConnectionConfig limits;
    Http3PeerStreams peerStreams;
    std::pmr::unordered_map<std::uint64_t, Request> requests;
    std::pmr::unordered_map<std::uint64_t, ClientRequest> clientRequests;
    struct Push final {
        std::optional<Http3MessageHead> head;
        std::optional<std::uint64_t> streamId;
        bool completed{false};
        bool canceled{false};
    };
    struct PushStream final {
        std::array<char, 8> idBytes{};
        std::size_t used{0};
        std::optional<std::uint64_t> id;
        std::optional<Http3ClientResponse> response;
    };
    std::pmr::unordered_map<std::uint64_t, Push> pushes;
    std::pmr::unordered_map<std::uint64_t, PushStream> pushStreams;
    bool feeding{false};
    Http3ControlStream control;
    Http3QpackDecoder receiveQpack;
    std::optional<Http3QpackEncoder> transmitQpack;
    bool transmitQpackConfigured{false};
    Http3ConnectionResult failed{};
    std::optional<std::uint64_t> localGoaway;
};

Http3Connection::Http3Connection(Http3PeerRole localRole, std::pmr::memory_resource* resource,
    Http3ConnectionConfig limits)
    : resource_(resource),
      impl_(nullptr) {
    if (resource == nullptr || limits.maxActiveStreams == 0 || limits.maxPeerUnidirectionalStreams == 0 || limits.maxRememberedPushes == 0 ||
        (limits.maxPushId && *limits.maxPushId > kHttp3VarIntMax)) {
        throw std::invalid_argument("HTTP/3 connection resource and limits must be valid");
    }
    std::pmr::polymorphic_allocator<Impl> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, localRole, resource, limits);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        impl_ = nullptr;
        throw;
    }
}

Http3Connection::~Http3Connection() {
    if (impl_ != nullptr && !retire()) {
        std::terminate();
    }
}

Http3Connection::Http3Connection(Http3Connection&& other) noexcept
    : resource_(other.resource_),
      impl_(std::exchange(other.impl_, nullptr)) {}

Http3Connection& Http3Connection::operator=(Http3Connection&& other) noexcept {
    if (this != &other) {
        Http3Connection temporary(std::move(other));
        std::swap(resource_, temporary.resource_);
        std::swap(impl_, temporary.impl_);
    }
    return *this;
}

Http3ConnectionResult Http3Connection::registerClientRequest(std::uint64_t streamId, HttpKnownMethod method) {
    if (impl_ == nullptr || impl_->role != Http3PeerRole::kClient ||
        !isHttp3RequestStreamId(streamId)) {
        return streamError(Http3ConnectionErrorCode::kStreamCreationError);
    }
    auto& impl = *impl_;
    if (impl.feeding) {
        throw std::logic_error("registerClientRequest() during HTTP/3 feed");
    }
    if (impl.failed.scope == Http3ConnectionErrorScope::kConnection) {
        return impl.failed;
    }
    if (impl.control.goawayId()) {
        return streamError(Http3ConnectionErrorCode::kRequestRejected);
    }
    if (impl.clientRequests.contains(streamId)) {
        return streamError(Http3ConnectionErrorCode::kStreamCreationError);
    }
    if (impl.clientRequests.size() >= impl.limits.maxActiveStreams) {
        return streamError(Http3ConnectionErrorCode::kExcessiveLoad);
    }
    impl.clientRequests.try_emplace(streamId, streamId, method, impl.resource, impl.limits, impl.receiveQpack, impl);
    return {};
}

Http3ConnectionResult Http3Connection::cancelRequest(std::uint64_t streamId) {
    if (!impl_ || impl_->feeding) {
        return streamError(Http3ConnectionErrorCode::kGeneralProtocolError);
    }
    auto& owner = *impl_;
    if (owner.failed.scope != Http3ConnectionErrorScope::kNone) {
        return owner.failed;
    }
    const bool live = owner.requests.contains(streamId) || owner.clientRequests.contains(streamId) || owner.pushStreams.contains(streamId);
    if (!live) {
        return streamError(Http3ConnectionErrorCode::kStreamCreationError);
    }
    try {
        if (const auto canceled = owner.receiveQpack.cancel(streamId); !canceled) {
            return owner.failed = connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed);
        }
        owner.requests.erase(streamId);
        owner.clientRequests.erase(streamId);
        if (const auto found = owner.pushStreams.find(streamId); found != owner.pushStreams.end()) {
            if (found->second.id) {
                owner.pushes.at(*found->second.id).completed = true;
            }
            (void)owner.peerStreams.retireNonCriticalStream(streamId);
            owner.pushStreams.erase(found);
        }
        return {Http3ConnectionStatus::kReset};
    } catch (...) {
        owner.failed = connectionError(Http3ConnectionErrorCode::kInternalError);
        throw;
    }
}

bool Http3Connection::retireClientRequest(std::uint64_t streamId) noexcept {
    if (impl_ == nullptr || impl_->role != Http3PeerRole::kClient || impl_->feeding ||
        !isHttp3RequestStreamId(streamId)) {
        return false;
    }
    return impl_->clientRequests.erase(streamId) != 0;
}

bool Http3Connection::retireServerRequest(std::uint64_t streamId) noexcept {
    if (impl_ == nullptr || impl_->role != Http3PeerRole::kServer || impl_->feeding ||
        !isHttp3RequestStreamId(streamId)) {
        return false;
    }
    return impl_->requests.erase(streamId) != 0;
}

bool Http3Connection::retire() noexcept {
    if (impl_ == nullptr || impl_->feeding) {
        return false;
    }
    auto* storage = std::exchange(impl_, nullptr);
    std::pmr::polymorphic_allocator<Impl> allocator(resource_);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, storage);
    allocator.deallocate(storage, 1);
    return true;
}

Http3ConnectionResult Http3Connection::feed(std::uint64_t streamId, std::span<const char> bytes,
    bool fin, bool reset, Http3ConnectionCallback callback, void* context) {
    if (impl_ == nullptr) {
        return connectionError(Http3ConnectionErrorCode::kGeneralProtocolError);
    }
    auto& impl = *impl_;
    Impl::FeedGuard feedGuard(impl);
    try {
        if (impl.failed.scope == Http3ConnectionErrorScope::kConnection) {
            return impl.failed;
        }
        if (callback == nullptr) {
            return connectionError(Http3ConnectionErrorCode::kGeneralProtocolError);
        }
        if (impl.pushStreams.contains(streamId)) {
            return impl.feedPush(streamId, bytes, fin, reset, callback, context);
        }
        if (isHttp3UnidirectionalStreamId(streamId)) {
            const auto peer = impl.peerStreams.feed(streamId, bytes, fin, reset);
            if (!peer) {
                impl.failed = connectionError(mapPeerError(peer.error()));
                return impl.failed;
            }
            if (peer->kind == Http3PeerStreamKind::kPush && impl.role == Http3PeerRole::kClient) {
                auto result = impl.feedPush(streamId, peer->remaining, fin, reset, callback, context);
                if (result.status == Http3ConnectionStatus::kQpackBlocked || result.status == Http3ConnectionStatus::kPushPromisePending) {
                    result.consumedBytes += peer->consumed;
                }
                return result;
            }
            if (peer->closed) {
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (peer->kind == Http3PeerStreamKind::kUnclassified || peer->kind == Http3PeerStreamKind::kUnknown ||
                peer->kind == Http3PeerStreamKind::kPush) {
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (peer->kind == Http3PeerStreamKind::kControl) {
                Impl::ControlCallback target{impl, callback, context};
                const auto status = impl.control.feed(peer->remaining, fin || reset, Impl::onControl, &target);
                if (impl.failed.scope == Http3ConnectionErrorScope::kConnection) {
                    return impl.failed;
                }
                if (status != Http3ControlStreamStatus::kNeedMoreData) {
                    impl.failed = connectionError(mapControlError(status));
                    return impl.failed;
                }
                if (!impl.transmitQpackConfigured && impl.control.peerSettings()) {
                    const auto& settings = *impl.control.peerSettings();
                    impl.transmitQpack.emplace(Http3QpackEncoderConfig{
                                                   .maxTableCapacity = static_cast<std::size_t>(settings.qpackMaxTableCapacity),
                                                   .maxBlockedStreams = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpackBlockedStreams, impl.limits.maxActiveStreams)),
                                                   .fields = {impl.limits.maxEncodedFieldSectionBytes,
                                                       impl.limits.maxFieldSectionSize,
                                                       impl.limits.maxFields},
                                                   .tableCapacity = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpackMaxTableCapacity, 4096))},
                        impl.resource);
                    impl.transmitQpackConfigured = true;
                }
            } else if (peer->kind == Http3PeerStreamKind::kQpackEncoder) {
                const auto decoded = impl.receiveQpack.consumeEncoder(peer->remaining, fin || reset);
                if (!decoded) {
                    impl.failed = connectionError(decoded.error() == Http3QpackConnectionError::kClosedCriticalStream
                                                      ? Http3ConnectionErrorCode::kClosedCriticalStream
                                                      : Http3ConnectionErrorCode::kQpackEncoderStreamError);
                    return impl.failed;
                }
            } else if (peer->kind == Http3PeerStreamKind::kQpackDecoder) {
                if (const auto consumed = impl.transmitQpack->consumeDecoder(peer->remaining, fin || reset); !consumed) {
                    impl.failed = connectionError(Http3ConnectionErrorCode::kQpackDecoderStreamError);
                    return impl.failed;
                }
            }
            return {Http3ConnectionStatus::kNeedMoreData};
        }

        if (impl.role == Http3PeerRole::kClient) {
            // Client response state exists only for explicitly registered local bidi streams.
            if (!isHttp3RequestStreamId(streamId)) {
                impl.failed = connectionError(Http3ConnectionErrorCode::kStreamCreationError);
                return impl.failed;
            }
            auto found = impl.clientRequests.find(streamId);
            if (found == impl.clientRequests.end()) {
                return streamError(Http3ConnectionErrorCode::kStreamCreationError);
            }
            auto& request = found->second;
            request.callback = callback;
            request.callbackContext = context;
            if (reset) {
                if (auto canceled = impl.receiveQpack.cancel(streamId); !canceled) {
                    impl.failed = connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed);
                    return impl.failed;
                }
            }
            const auto response = request.response.feed(bytes, fin, reset, Impl::onClientResponse, &request);
            if (impl.failed.scope == Http3ConnectionErrorScope::kConnection) {
                return impl.failed;
            }
            if (response.status == Http3ClientResponseStatus::kQpackBlocked) {
                return {Http3ConnectionStatus::kQpackBlocked, Http3ConnectionErrorScope::kNone,
                    Http3ConnectionErrorCode::kNoError, response.consumedBytes};
            }
            if (response.scope == Http3ConnectionErrorScope::kConnection) {
                impl.failed = connectionError(response.code);
                impl.clientRequests.clear();
                return impl.failed;
            }
            if (response.scope == Http3ConnectionErrorScope::kStream) {
                impl.clientRequests.erase(found);
                return streamError(response.code);
            }
            if (response.status == Http3ClientResponseStatus::kMessageEnd) {
                impl.clientRequests.erase(found);
                return {Http3ConnectionStatus::kMessageEnd};
            }
            if (response.status == Http3ClientResponseStatus::kReset) {
                impl.clientRequests.erase(found);
                return {Http3ConnectionStatus::kReset};
            }
            return {Http3ConnectionStatus::kNeedMoreData};
        }
        if (const auto valid = Http3PeerStreams::acceptBidirectional(impl.role, streamId); !valid) {
            impl.failed = connectionError(Http3ConnectionErrorCode::kStreamCreationError);
            return impl.failed;
        }

        auto found = impl.requests.find(streamId);
        if (found == impl.requests.end()) {
            if (reset) {
                // A sender can encode dynamic references and publish encoder
                // instructions before any HEADERS bytes reach this stream.
                // Cancellation must release those references even though there
                // is no request/parser node. No stream tombstone is required.
                if (auto canceled = impl.receiveQpack.cancel(streamId); !canceled) {
                    impl.failed = connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed);
                    return impl.failed;
                }
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (impl.localGoaway && streamId >= *impl.localGoaway) {
                return streamError(Http3ConnectionErrorCode::kRequestRejected);
            }
            if (impl.requests.size() >= impl.limits.maxActiveStreams) {
                return streamError(Http3ConnectionErrorCode::kExcessiveLoad);
            }
            found = impl.requests.try_emplace(streamId, impl.resource, impl.limits, impl.receiveQpack).first;
            found->second.streamId = streamId;
        }
        auto& request = found->second;
        if (reset) {
            if (auto canceled = impl.receiveQpack.cancel(streamId); !canceled) {
                impl.failed = connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed);
                return impl.failed;
            }
            impl.requests.erase(found);
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kReset, .streamId = streamId};
            callback(context, event);
            return {Http3ConnectionStatus::kNeedMoreData};
        }
        request.callback = callback;
        request.callbackContext = context;
        request.callbackResult = {};
        const auto status = request.frames.feed(bytes, fin, Impl::onRequestFrame, &request);
        if (status == Http3StreamFrameStatus::kPaused) {
            return {Http3ConnectionStatus::kQpackBlocked, Http3ConnectionErrorScope::kNone,
                Http3ConnectionErrorCode::kNoError, request.frames.consumedBytes()};
        }
        // Framing continues to consume the same input after a message callback
        // reports a stream error. A later connection-level frame error must not be
        // hidden by that earlier stream failure.
        if (const auto code = http3ConnectionErrorCodeForStreamFrameStatus(status)) {
            impl.requests.erase(found);
            impl.failed = connectionError(*code);
            return impl.failed;
        }
        if (request.callbackResult.scope != Http3ConnectionErrorScope::kNone) {
            const auto result = request.callbackResult;
            if (result.scope == Http3ConnectionErrorScope::kConnection) {
                impl.failed = result;
            }
            impl.requests.erase(found);
            return result;
        }
        if (status == Http3StreamFrameStatus::kMessageEnd && !request.terminal) {
            Impl::finish(request);
            if (request.callbackResult.scope != Http3ConnectionErrorScope::kNone) {
                const auto result = request.callbackResult;
                if (result.scope == Http3ConnectionErrorScope::kConnection) {
                    impl.failed = result;
                }
                impl.requests.erase(found);
                return result;
            }
        }
        if (status == Http3StreamFrameStatus::kMessageEnd || request.terminal) {
            impl.requests.erase(found);
            return {Http3ConnectionStatus::kMessageEnd};
        }
        return {Http3ConnectionStatus::kNeedMoreData};
    } catch (...) {
        // A callback or allocator may have left an incomplete frame/event in
        // this feed. Replaying the input is unsafe, even on another stream.
        impl.failed = connectionError(Http3ConnectionErrorCode::kInternalError);
        throw;
    }
}

namespace {
std::pmr::vector<char> controlIdFrame(std::uint64_t type, std::uint64_t id, std::pmr::memory_resource* resource) {
    std::array<char, 24> buffer{};
    const auto frame = encodeHttp3FrameHeader(buffer, type, http3VarIntEncodedSize(id));
    const auto value = encodeHttp3VarInt(std::span(buffer).subspan(*frame), id);
    return {buffer.begin(), buffer.begin() + *frame + *value, resource};
}
}  // namespace
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::prepareMaxPushId(std::uint64_t maximum) {
    if (!impl_ || impl_->feeding || impl_->role != Http3PeerRole::kClient || maximum > kHttp3VarIntMax ||
        impl_->failed.scope != Http3ConnectionErrorScope::kNone ||
        (impl_->limits.maxPushId && maximum < *impl_->limits.maxPushId)) {
        return std::unexpected(Http3ConnectionErrorCode::kIdError);
    }
    auto output = controlIdFrame(0xd, maximum, resource_);
    impl_->limits.maxPushId = maximum;
    for (auto& [id, request] : impl_->clientRequests) {
        (void)request.response.authorizePush(maximum);
    }
    return output;
}
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::prepareCancelPush(std::uint64_t pushId) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone ||
        !impl_->pushes.contains(pushId) || !impl_->pushes.at(pushId).head) {
        return std::unexpected(Http3ConnectionErrorCode::kIdError);
    }
    auto output = controlIdFrame(3, pushId, resource_);
    impl_->pushes.at(pushId).canceled = true;
    return output;
}
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::prepareGoaway(std::uint64_t id) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone || id > kHttp3VarIntMax ||
        (impl_->role == Http3PeerRole::kServer && !isHttp3RequestStreamId(id)) ||
        (impl_->localGoaway && id > *impl_->localGoaway)) {
        return std::unexpected(Http3ConnectionErrorCode::kIdError);
    }
    auto output = controlIdFrame(7, id, resource_);
    impl_->localGoaway = id;
    return output;
}
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::preparePriorityUpdate(HttpPriorityUpdate update) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone ||
        impl_->role != Http3PeerRole::kClient ||
        (update.push && (!impl_->pushes.contains(update.elementId) || !impl_->pushes.at(update.elementId).head))) {
        return std::unexpected(Http3ConnectionErrorCode::kIdError);
    }
    std::array<char, 32> buffer{};
    const auto size = encodeHttp3PriorityUpdate(buffer, update);
    if (!size) {
        return std::unexpected(Http3ConnectionErrorCode::kMessageError);
    }
    return std::pmr::vector<char>(buffer.begin(), buffer.begin() + *size, resource_);
}
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::preparePushStream(std::uint64_t streamId, std::uint64_t pushId) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone ||
        impl_->role != Http3PeerRole::kServer || streamId > kHttp3VarIntMax || (streamId & 3) != 3 ||
        !impl_->pushes.contains(pushId) || !impl_->pushes.at(pushId).head) {
        return std::unexpected(Http3ConnectionErrorCode::kIdError);
    }
    auto& push = impl_->pushes.at(pushId);
    if (push.streamId || push.canceled) {
        return std::unexpected(Http3ConnectionErrorCode::kRequestCancelled);
    }
    for (const auto& [id, previous] : impl_->pushes) {
        if (previous.streamId == streamId) {
            return std::unexpected(Http3ConnectionErrorCode::kStreamCreationError);
        }
    }
    std::array<char, 9> buffer{1};
    const auto size = encodeHttp3VarInt(std::span(buffer).subspan(1), pushId);
    std::pmr::vector<char> output(buffer.begin(), buffer.begin() + 1 + *size, resource_);
    push.streamId = streamId;
    return output;
}

std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::prepareOriginAdvertisement(
    std::span<const std::string_view> origins) {
    if (!impl_ || impl_->feeding || impl_->role != Http3PeerRole::kServer || impl_->failed.scope != Http3ConnectionErrorScope::kNone) {
        return std::unexpected(Http3ConnectionErrorCode::kGeneralProtocolError);
    }
    auto bytes = encodeHttp3OriginFrame(origins, impl_->limits.maxEncodedFieldSectionBytes, resource_);
    if (!bytes) {
        return std::unexpected(Http3ConnectionErrorCode::kMessageError);
    }
    return std::move(*bytes);
}
std::optional<std::uint64_t> Http3Connection::peerMaxPushId() const noexcept {
    return impl_ ? impl_->control.maxPushId() : std::nullopt;
}
const Http3MessageHead* Http3Connection::promisedRequest(std::uint64_t pushId) const& noexcept {
    if (!impl_) {
        return nullptr;
    }
    const auto found = impl_->pushes.find(pushId);
    return found == impl_->pushes.end() || !found->second.head ? nullptr : &*found->second.head;
}
std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> Http3Connection::preparePushPromise(
    std::uint64_t associatedStreamId, std::uint64_t pushId, HttpPushRequestView request) {
    using Error = Http3ConnectionErrorCode;
    if (!impl_ || impl_->role != Http3PeerRole::kServer || impl_->feeding ||
        impl_->failed.scope != Http3ConnectionErrorScope::kNone || !isHttp3RequestStreamId(associatedStreamId)) {
        return std::unexpected(Error::kGeneralProtocolError);
    }
    auto& owner = *impl_;
    const auto maxId = owner.control.maxPushId();
    if (!maxId || pushId > *maxId) {
        return std::unexpected(Error::kIdError);
    }
    if (owner.control.goawayId()) {
        return std::unexpected(Error::kRequestRejected);
    }
    if (const auto found = owner.pushes.find(pushId); found != owner.pushes.end() && found->second.canceled) {
        return std::unexpected(Error::kRequestCancelled);
    }
    if (request.method != "GET" && request.method != "HEAD") {
        return std::unexpected(Error::kMessageError);
    }
    std::pmr::vector<Http3FieldSectionFieldView> fields(resource_);
    for (const auto& field : request.headers) {
        fields.push_back({field.name(), field.value(), false});
    }
    const auto limits = owner.outbound_field_limits({owner.limits.maxEncodedFieldSectionBytes,
        owner.limits.maxFieldSectionSize, owner.limits.maxFields});
    auto encoded = encodeHttp3ClientRequestHead({.method = request.method, .scheme = request.scheme, .authority = request.authority, .path = request.path, .fields = fields}, limits, resource_);
    if (!encoded) {
        return std::unexpected(Error::kMessageError);
    }
    auto head = decodeHttp3MessageHead(encoded->fieldSection, Http3MessageHeadKind::kRequest, resource_,
        {owner.limits.maxFieldSectionSize, owner.limits.maxFields, owner.limits.maxEncodedFieldSectionBytes});
    if (!head || (head->contentLength && *head->contentLength != 0)) {
        return std::unexpected(Error::kMessageError);
    }
    std::array<char, 24> prefix{};
    const auto idSize = http3VarIntEncodedSize(pushId);
    const auto frameSize = encodeHttp3FrameHeader(prefix, 5, idSize + encoded->fieldSection.size());
    const auto id = encodeHttp3VarInt(std::span(prefix).subspan(*frameSize), pushId);
    std::pmr::vector<char> output(resource_);
    output.insert(output.end(), prefix.begin(), prefix.begin() + *frameSize + *id);
    output.insert(output.end(), encoded->fieldSection.begin(), encoded->fieldSection.end());
    if (!owner.acceptPromise(pushId, *head)) {
        return std::unexpected(owner.failed.code);
    }
    return output;
}

std::expected<std::pmr::vector<char>, Http3QpackConnectionError> Http3Connection::encodeFieldSection(
    std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone) {
        return std::unexpected(Http3QpackConnectionError::kInvalidStreamId);
    }
    if (!isHttp3RequestStreamId(streamId) &&
        !(impl_->role == Http3PeerRole::kServer && (streamId & 3) == 3 && streamId <= kHttp3VarIntMax)) {
        return std::unexpected(Http3QpackConnectionError::kInvalidStreamId);
    }
    return impl_->transmitQpack->encode(streamId, fields, impl_->outbound_field_limits({}));
}
std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> Http3Connection::encodeClientRequestHead(
    std::uint64_t streamId, Http3ClientRequestHeadView view, Http3FieldSectionLimits limits) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone ||
        impl_->role != Http3PeerRole::kClient || !isHttp3RequestStreamId(streamId)) {
        return std::unexpected(Http3ClientRequestHeadFailure{Http3ClientRequestHeadError::kFieldSectionError, Http3FieldSectionError::kInvalidPrefix});
    }
    view.peerEnableConnectProtocol = impl_->control.peerSettings() && impl_->control.peerSettings()->enableConnectProtocol;
    return encodeHttp3ClientRequestHead(*impl_->transmitQpack, streamId, view, impl_->outbound_field_limits(limits), resource_);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> Http3Connection::encodeConnectResponseHead(
    std::uint64_t streamId, const HttpResponse& response, Http3FieldSectionLimits limits) {
    if (!isHttp3RequestStreamId(streamId) || !response.status().isSuccessful() || !response.bodyBytes().empty() || response.fileBody().has_value()) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kInvalidField});
    }
    return encodeResponseHead(streamId, response, planBufferedHttpResponseWrite(HttpKnownMethod::kConnect, response), limits);
}

std::expected<Http3ResponseHead, Http3ResponseHeadFailure> Http3Connection::encodeResponseHead(std::uint64_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan plan, Http3FieldSectionLimits limits) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone || impl_->role != Http3PeerRole::kServer ||
        (!isHttp3RequestStreamId(streamId) && ((streamId & 3) != 3 || streamId > kHttp3VarIntMax))) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, Http3FieldSectionError::kInvalidPrefix});
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encodeHttp3ResponseHead(*impl_->transmitQpack, streamId, response, plan, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure> Http3Connection::encodeStreamingResponseHead(std::uint64_t streamId, HttpResponse response, HttpKnownMethod method, http_response_stream_kind kind, http_response_trailer_intent trailers, Http3FieldSectionLimits limits) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone || impl_->role != Http3PeerRole::kServer ||
        (!isHttp3RequestStreamId(streamId) && ((streamId & 3) != 3 || streamId > kHttp3VarIntMax))) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, Http3FieldSectionError::kInvalidPrefix});
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encodeHttp3StreamingResponseHead(*impl_->transmitQpack, streamId, std::move(response), method, kind, trailers, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::expected<Http3ResponseHead, Http3ResponseHeadFailure> Http3Connection::encodeInterimResponseHead(std::uint64_t streamId, const HttpInterimResponseHead& response, Http3FieldSectionLimits limits) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone || impl_->role != Http3PeerRole::kServer ||
        (!isHttp3RequestStreamId(streamId) && ((streamId & 3) != 3 || streamId > kHttp3VarIntMax))) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, Http3FieldSectionError::kInvalidPrefix});
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encodeHttp3InterimResponseHead(*impl_->transmitQpack, streamId, response, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> Http3Connection::encodeResponseTrailers(std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields, Http3FieldSectionLimits limits) {
    if (!impl_ || impl_->feeding || impl_->failed.scope != Http3ConnectionErrorScope::kNone || impl_->role != Http3PeerRole::kServer ||
        (!isHttp3RequestStreamId(streamId) && ((streamId & 3) != 3 || streamId > kHttp3VarIntMax))) {
        return std::unexpected(Http3ResponseHeadFailure{Http3ResponseHeadError::kFieldSectionError, Http3FieldSectionError::kInvalidPrefix});
    }
    const auto local_limits = impl_->local_field_limits(limits);
    return impl_->response_encoding_result(
        encodeHttp3ResponseTrailers(*impl_->transmitQpack, streamId, fields, impl_->outbound_field_limits(local_limits), resource_), local_limits);
}
std::span<const char> Http3Connection::pendingQpackEncoderOutput() const& noexcept {
    return impl_ ? impl_->transmitQpack->pendingEncoderOutput() : std::span<const char>{};
}
bool Http3Connection::consumeQpackEncoderOutput(std::size_t bytes) noexcept {
    return impl_ && !impl_->feeding && impl_->transmitQpack->consumeEncoderOutput(bytes);
}

std::span<const char> Http3Connection::pendingQpackDecoderOutput() const& noexcept {
    return impl_ ? impl_->receiveQpack.pendingDecoderOutput() : std::span<const char>{};
}
bool Http3Connection::consumeQpackDecoderOutput(std::size_t bytes) noexcept {
    return impl_ && !impl_->feeding && impl_->receiveQpack.consumeDecoderOutput(bytes);
}

std::size_t Http3Connection::activeRequestCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->requests.size() + impl_->clientRequests.size() + impl_->pushStreams.size();
}

Http3Settings Http3Connection::localSettings() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    const auto& config = impl_->limits;
    return {.qpackMaxTableCapacity = config.qpackMaxTableCapacity,
        .maxFieldSectionSize = config.maxFieldSectionSize,
        .qpackBlockedStreams = config.qpackBlockedStreams,
        .enableConnectProtocol = config.enableConnectProtocol,
        .h3Datagram = config.enableDatagrams};
}

const std::optional<Http3Settings>& Http3Connection::peerSettings() const noexcept {
    static const std::optional<Http3Settings> empty;
    return impl_ == nullptr ? empty : impl_->control.peerSettings();
}

bool Http3Connection::peerReportsUnprocessed(std::uint64_t streamId,
    std::optional<std::uint64_t> peerResetErrorCode) const noexcept {
    if (impl_ == nullptr || impl_->failed.scope != Http3ConnectionErrorScope::kNone) {
        return false;
    }
    const auto found = impl_->clientRequests.find(streamId);
    if (found == impl_->clientRequests.end() || found->second.responseObserved) {
        return false;
    }
    const auto goaway = impl_->control.goawayId();
    return (goaway && streamId >= *goaway) ||
           peerResetErrorCode == static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestRejected);
}

std::optional<std::uint64_t> Http3Connection::peerGoawayId() const noexcept {
    return impl_ == nullptr ? std::nullopt : impl_->control.goawayId();
}

}  // namespace ruvia
