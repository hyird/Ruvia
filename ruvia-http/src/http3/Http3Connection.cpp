#include "ruvia/http/Http3Connection.h"

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/http/Http3ControlStream.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/Http3QpackStreams.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpHeader.h"

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

bool isValidTrailer(Http3FieldSectionFieldView field) {
    const bool uppercaseName = std::any_of(field.name.begin(), field.name.end(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z';
    });
    return !field.name.empty() && !uppercaseName && field.name.front() != ':' &&
           isValidHttpHeaderName(field.name) && isValidHttpHeaderValue(field.value) &&
           field.name != "content-length" && field.name != "transfer-encoding" && field.name != "host" &&
           field.name != "trailer" && field.name != "connection" && field.name != "keep-alive" &&
           field.name != "proxy-connection" && field.name != "upgrade";
}

}  // namespace

struct Http3Connection::Impl final {
    struct Request final {
        Request(std::pmr::memory_resource* resource, const Http3ConnectionLimits& limits)
            : frames(Http3StreamKind::kRequest, resource,
                  {.maxFieldSectionSize = limits.maxEncodedFieldSectionBytes}),
              resource(resource),
              limits(limits) {}

        Http3StreamFrames frames;
        std::pmr::memory_resource* resource;
        Http3ConnectionLimits limits;
        std::optional<Http3MessageBody> body;
        bool extendedConnect{false};
        bool terminal{false};
        Http3ConnectionResult callbackResult{};
        Http3ConnectionCallback callback{nullptr};
        void* callbackContext{nullptr};
        std::uint64_t streamId{0};
    };

    struct ClientRequest final {
        ClientRequest(std::uint64_t streamId, HttpKnownMethod method, std::pmr::memory_resource* resource,
            const Http3ConnectionLimits& limits)
            : response(streamId, method, resource,
                  {.maxFieldSectionSize = limits.maxFieldSectionSize,
                      .maxFields = limits.maxFields,
                      .maxEncodedFieldSectionBytes = limits.maxEncodedFieldSectionBytes}) {}
        Http3ClientResponse response;
        bool responseObserved{false};
        Http3ConnectionCallback callback{nullptr};
        void* callbackContext{nullptr};
    };

    struct TrailerField final {
        TrailerField(Http3FieldSectionFieldView field, std::pmr::memory_resource* resource)
            : name(field.name, resource),
              value(field.value, resource),
              neverIndexed(field.neverIndexed) {}
        std::pmr::string name;
        std::pmr::string value;
        bool neverIndexed;
    };

    struct TrailerCollector final {
        explicit TrailerCollector(std::pmr::memory_resource* resource)
            : fields(resource) {}
        std::pmr::vector<TrailerField> fields;
        bool valid{true};
    };

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

    static bool collectTrailer(void* opaque, Http3FieldSectionFieldView field) {
        auto& collector = *static_cast<TrailerCollector*>(opaque);
        if (!isValidTrailer(field)) {
            collector.valid = false;
            return false;
        }
        collector.fields.emplace_back(field, collector.fields.get_allocator().resource());
        return true;
    }

    Impl(Http3PeerRole role, std::pmr::memory_resource* resource, Http3ConnectionLimits limits)
        : role(role),
          resource(resource),
          limits(limits),
          peerStreams(role, resource, {.maxActiveStreams = limits.maxActiveStreams}),
          requests(resource),
          clientRequests(resource),
          control(role == Http3PeerRole::kServer ? Http3ControlRole::kServer : Http3ControlRole::kClient,
              resource,
              {.maxFieldSectionSize = limits.maxEncodedFieldSectionBytes}),
          encoder(),
          decoder() {}

    static void onRequestFrame(void* opaque, Http3StreamFrameEvent frame) {
        auto& request = *static_cast<Request*>(opaque);
        if (request.terminal || request.callbackResult.scope != Http3ConnectionErrorScope::kNone) {
            return;
        }
        if (frame.kind == Http3StreamFrameEventKind::kData) {
            if (!frame.payload.empty()) {
                if (request.extendedConnect) {
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
            auto decoded = decodeHttp3MessageHead(frame.payload, Http3MessageHeadKind::kRequest,
                request.resource, headLimits);
            if (!decoded) {
                request.callbackResult = requestError(decoded.error() == Http3MessageHeadError::kQpackDecompressionFailed
                                                          ? Http3ConnectionErrorCode::kQpackDecompressionFailed
                                                      : decoded.error() == Http3MessageHeadError::kFieldSectionTooLarge
                                                          ? Http3ConnectionErrorCode::kExcessiveLoad
                                                          : Http3ConnectionErrorCode::kMessageError);
                request.terminal = true;
                return;
            }
            request.extendedConnect = decoded->method == "CONNECT" && !decoded->protocol.empty();
            if (!request.extendedConnect) {
                request.body.emplace(decoded->contentLength, true);
            }
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kRequestHead,
                .streamId = request.streamId,
                .head = &*decoded};
            request.callback(request.callbackContext, event);
            return;
        }

        if (request.extendedConnect) {
            request.callbackResult = streamError(Http3ConnectionErrorCode::kMessageError);
            request.terminal = true;
            return;
        }
        const Http3FieldSectionLimits fieldLimits{request.limits.maxEncodedFieldSectionBytes,
            request.limits.maxFieldSectionSize, request.limits.maxFields};
        TrailerCollector collector(request.resource);
        const auto decoded = decodeHttp3FieldSection(frame.payload, collectTrailer, &collector,
            fieldLimits, request.resource);
        if (!decoded || !collector.valid) {
            request.callbackResult = requestError(!collector.valid
                                                      ? Http3ConnectionErrorCode::kMessageError
                                                  : decoded.error() == Http3FieldSectionError::kFieldListTooLarge ||
                                                          decoded.error() == Http3FieldSectionError::kFieldSectionTooLarge ||
                                                          decoded.error() == Http3FieldSectionError::kTooManyFields
                                                      ? Http3ConnectionErrorCode::kExcessiveLoad
                                                      : Http3ConnectionErrorCode::kQpackDecompressionFailed);
            request.terminal = true;
            return;
        }
        for (const auto& field : collector.fields) {
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kTrailerField,
                .streamId = request.streamId,
                .trailer = {field.name, field.value, field.neverIndexed}};
            request.callback(request.callbackContext, event);
        }
    }

    static void finish(Request& request) {
        if (request.terminal) {
            return;
        }
        if (!request.extendedConnect) {
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
        if (responseEvent.kind == Http3ClientResponseEventKind::kInformationalHead ||
            responseEvent.kind == Http3ClientResponseEventKind::kFinalHead) {
            request.responseObserved = true;
        }
        const auto kind = [&] {
            switch (responseEvent.kind) {
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
            .responseBodyPlan = responseEvent.responseBodyPlan};
        request.callback(request.callbackContext, event);
    }

    Http3PeerRole role;
    std::pmr::memory_resource* resource;
    Http3ConnectionLimits limits;
    Http3PeerStreams peerStreams;
    std::pmr::unordered_map<std::uint64_t, Request> requests;
    std::pmr::unordered_map<std::uint64_t, ClientRequest> clientRequests;
    bool feeding{false};
    Http3ControlStream control;
    Http3QpackEncoderStreamValidator encoder;
    Http3QpackDecoderStreamValidator decoder;
    Http3ConnectionResult failed{};
};

Http3Connection::Http3Connection(Http3PeerRole localRole, std::pmr::memory_resource* resource,
    Http3ConnectionLimits limits)
    : resource_(resource),
      impl_(nullptr) {
    if (resource == nullptr || limits.maxActiveStreams == 0) {
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
    if (const auto goaway = impl.control.goawayId(); goaway && streamId >= *goaway) {
        return streamError(Http3ConnectionErrorCode::kRequestRejected);
    }
    if (impl.clientRequests.contains(streamId)) {
        return streamError(Http3ConnectionErrorCode::kStreamCreationError);
    }
    if (impl.clientRequests.size() >= impl.limits.maxActiveStreams) {
        return streamError(Http3ConnectionErrorCode::kExcessiveLoad);
    }
    impl.clientRequests.try_emplace(streamId, streamId, method, impl.resource, impl.limits);
    return {};
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
        if (isHttp3UnidirectionalStreamId(streamId)) {
            const auto peer = impl.peerStreams.feed(streamId, bytes, fin, reset);
            if (!peer) {
                impl.failed = connectionError(mapPeerError(peer.error()));
                return impl.failed;
            }
            if (peer->kind == Http3PeerStreamKind::kPush && impl.role == Http3PeerRole::kClient) {
                impl.failed = connectionError(Http3ConnectionErrorCode::kIdError);
                return impl.failed;
            }
            if (peer->closed) {
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (peer->kind == Http3PeerStreamKind::kUnclassified || peer->kind == Http3PeerStreamKind::kUnknown ||
                peer->kind == Http3PeerStreamKind::kPush) {
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (peer->kind == Http3PeerStreamKind::kControl) {
                const auto status = impl.control.feed(peer->remaining, fin || reset);
                if (status != Http3ControlStreamStatus::kNeedMoreData) {
                    impl.failed = connectionError(mapControlError(status));
                    return impl.failed;
                }
            } else if (peer->kind == Http3PeerStreamKind::kQpackEncoder) {
                if (!impl.encoder.consume(peer->remaining, fin || reset)) {
                    impl.failed = connectionError(Http3ConnectionErrorCode::kQpackEncoderStreamError);
                    return impl.failed;
                }
            } else if (peer->kind == Http3PeerStreamKind::kQpackDecoder) {
                if (!impl.decoder.consume(peer->remaining, fin || reset)) {
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
            const auto response = request.response.feed(bytes, fin, reset, Impl::onClientResponse, &request);
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
                // The QUIC adapter reports termination once; an already-absent stream
                // has no request state to notify and needs no connection-lifetime tombstone.
                return {Http3ConnectionStatus::kNeedMoreData};
            }
            if (impl.requests.size() >= impl.limits.maxActiveStreams) {
                return streamError(Http3ConnectionErrorCode::kExcessiveLoad);
            }
            found = impl.requests.try_emplace(streamId, impl.resource, impl.limits).first;
            found->second.streamId = streamId;
        }
        auto& request = found->second;
        if (reset) {
            impl.requests.erase(found);
            const Http3ConnectionEvent event{.kind = Http3ConnectionEventKind::kReset, .streamId = streamId};
            callback(context, event);
            return {Http3ConnectionStatus::kNeedMoreData};
        }
        request.callback = callback;
        request.callbackContext = context;
        request.callbackResult = {};
        const auto status = request.frames.feed(bytes, fin, Impl::onRequestFrame, &request);
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

std::size_t Http3Connection::activeRequestCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->requests.size() + impl_->clientRequests.size();
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
