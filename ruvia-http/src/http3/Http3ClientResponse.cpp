#include "ruvia/http/Http3ClientResponse.h"

#include <limits>
#include <stdexcept>
#include <variant>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

#include "http3/http3_trailer_collector.h"

namespace ruvia {
namespace {

Http3ClientResponseResult streamError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ClientResponseStatus::kStreamError, Http3ConnectionErrorScope::kStream, code};
}

Http3ClientResponseResult connectionError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ClientResponseStatus::kConnectionError, Http3ConnectionErrorScope::kConnection, code};
}

bool valid_response_trailer_policy(Http3FieldSectionFieldView field) noexcept {
    return detail::response_trailer_content_valid(field.name, field.value);
}

}  // namespace

struct Http3ClientResponse::Impl final {
    using trailer_collector = detail::http3_trailer_collector<valid_response_trailer_policy>;

    Impl(std::uint64_t stream, HttpKnownMethod method, std::pmr::memory_resource* resource,
        Http3ClientResponseLimits configured, Http3QpackDecoder* sharedDecoder)
        : streamId(stream),
          requestMethod(method),
          memory(resource),
          frames(Http3StreamKind::kResponse, resource,
              {.maxFieldSectionSize = configured.maxEncodedFieldSectionBytes,
                  .maxSettingsPayloadBytes = 64 * 1024,
                  .allowPush = configured.maxPushId.has_value() && !configured.pushStream}),
          limits(configured),
          decoder(sharedDecoder) {}

    std::uint64_t streamId;
    HttpKnownMethod requestMethod;
    std::pmr::memory_resource* memory;
    Http3StreamFrames frames;
    Http3ClientResponseLimits limits;
    Http3QpackDecoder* decoder;
    Http3ClientResponseCallback callback{};
    void* callbackContext{};
    Http3ClientResponseResult result{};
    Http3MessageBody body{std::nullopt, true};
    std::optional<HttpResponseBodyPlan> bodyPlan;
    std::uint64_t tunnelDataLength{0};
    bool finalHeaders{false};
    bool trailers{false};
    bool tunnel{false};
    bool terminal{false};
    bool feeding{false};

    static void onFrame(void* opaque, Http3StreamFrameEvent frame) {
        auto& self = *static_cast<Impl*>(opaque);
        if (self.terminal || self.result.scope != Http3ConnectionErrorScope::kNone) {
            return;
        }
        if (frame.kind == Http3StreamFrameEventKind::kPushPromise) {
            const auto id = decodeHttp3VarInt(frame.payload);
            if ((id.index() != 0)) {
                self.result = connectionError(Http3ConnectionErrorCode::kFrameError);
                return;
            }
            if (!self.limits.maxPushId || std::get<0>(id).value > *self.limits.maxPushId || self.limits.pushStream) {
                self.result = connectionError(Http3ConnectionErrorCode::kIdError);
                return;
            }
            const auto section = frame.payload.subspan(std::get<0>(id).encodedBytes);
            const Http3MessageHeadLimits headLimits{self.limits.maxFieldSectionSize,
                self.limits.maxFields, self.limits.maxEncodedFieldSectionBytes};
            auto head = [&]() -> std::variant<Http3MessageHead, Http3MessageHeadError> {
                if (!self.decoder) {
                    return decodeHttp3MessageHead(section, Http3MessageHeadKind::kRequest, self.memory, headLimits);
                }
                auto decoded = decodeHttp3MessageHead(*self.decoder, self.streamId, section,
                    Http3MessageHeadKind::kRequest, self.memory, headLimits);
                if ((decoded.index() != 0)) {
                    return std::get<1>(decoded);
                }
                if (auto* result = std::get_if<Http3MessageHead>(&std::get<0>(decoded))) {
                    return std::move(*result);
                }
                self.frames.pause();
                return Http3MessageHeadError::kMessageError;
            }();
            if (self.frames.paused()) {
                return;
            }
            if ((head.index() != 0) || (std::get<0>(head).method != "GET" && std::get<0>(head).method != "HEAD") ||
                (std::get<0>(head).contentLength && *std::get<0>(head).contentLength != 0)) {
                self.result = connectionError((head.index() != 0) && std::get<1>(head) == Http3MessageHeadError::kQpackDecompressionFailed
                                                  ? Http3ConnectionErrorCode::kQpackDecompressionFailed
                                                  : Http3ConnectionErrorCode::kMessageError);
                return;
            }
            const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kPushPromise,
                .streamId = self.streamId,
                .head = &std::get<0>(head),
                .pushId = std::get<0>(id).value};
            self.callback(self.callbackContext, event);
            return;
        }
        if (frame.kind == Http3StreamFrameEventKind::kData) {
            if (!self.finalHeaders || self.trailers) {
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            if (self.tunnel) {
                if (frame.payload.size() > std::numeric_limits<std::uint64_t>::max() - self.tunnelDataLength) {
                    self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                    return;
                }
                self.tunnelDataLength += frame.payload.size();
                if (!frame.payload.empty()) {
                    const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kTunnelData,
                        .streamId = self.streamId,
                        .body = frame.payload};
                    self.callback(self.callbackContext, event);
                }
                return;
            }
            const auto counted = self.body.feed(frame.payload.size(), false);
            if (counted != Http3MessageBodyResult::kAccepted) {
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            if (!frame.payload.empty()) {
                const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kBody,
                    .streamId = self.streamId,
                    .body = frame.payload};
                self.callback(self.callbackContext, event);
            }
            return;
        }
        if (frame.kind != Http3StreamFrameEventKind::kHeaders || !frame.endFrame) {
            return;
        }

        if (self.finalHeaders) {
            if (self.trailers || self.tunnel) {
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            trailer_collector collector(self.memory);
            const Http3FieldSectionLimits fieldLimits{self.limits.maxEncodedFieldSectionBytes,
                self.limits.maxFieldSectionSize, self.limits.maxFields};
            if (self.decoder) {
                const auto decoded = self.decoder->decode(self.streamId, frame.payload, trailer_collector::collect, &collector);
                if ((decoded.index() == 0) && std::get<0>(decoded).status == Http3QpackDecodeStatus::kBlocked) {
                    self.frames.pause();
                    return;
                }
                if ((decoded.index() != 0) || !collector.valid_) {
                    self.result = (decoded.index() != 0) ? connectionError(std::get<1>(decoded) == Http3QpackConnectionError::kLimit
                                                                               ? Http3ConnectionErrorCode::kExcessiveLoad
                                                                               : Http3ConnectionErrorCode::kQpackDecompressionFailed)
                                                         : streamError(Http3ConnectionErrorCode::kMessageError);
                    return;
                }
            } else {
                const auto decoded = decodeHttp3FieldSection(frame.payload, trailer_collector::collect, &collector, fieldLimits, self.memory);
                if ((decoded.index() != 0) || !collector.valid_) {
                    self.result = collector.valid_ ? connectionError(std::get<1>(decoded) == Http3FieldSectionError::kFieldListTooLarge ||
                                                                             std::get<1>(decoded) == Http3FieldSectionError::kFieldSectionTooLarge || std::get<1>(decoded) == Http3FieldSectionError::kTooManyFields
                                                                         ? Http3ConnectionErrorCode::kExcessiveLoad
                                                                         : Http3ConnectionErrorCode::kQpackDecompressionFailed)
                                                   : streamError(Http3ConnectionErrorCode::kMessageError);
                    return;
                }
            }
            self.trailers = true;
            for (const auto& field : collector.fields_) {
                const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kTrailerField,
                    .streamId = self.streamId,
                    .trailer = {field.name_, field.value_, field.never_indexed_}};
                self.callback(self.callbackContext, event);
            }
            return;
        }

        const Http3MessageHeadLimits headLimits{self.limits.maxFieldSectionSize,
            self.limits.maxFields, self.limits.maxEncodedFieldSectionBytes};
        auto decoded = [&]() -> std::variant<Http3MessageHead, Http3MessageHeadError> {
            if (!self.decoder) {
                return decodeHttp3MessageHead(frame.payload, Http3MessageHeadKind::kResponse, self.memory, headLimits);
            }
            auto result = decodeHttp3MessageHead(*self.decoder, self.streamId, frame.payload,
                Http3MessageHeadKind::kResponse, self.memory, headLimits);
            if ((result.index() != 0)) {
                return std::get<1>(result);
            }
            if (auto* head = std::get_if<Http3MessageHead>(&std::get<0>(result))) {
                return std::move(*head);
            }
            self.frames.pause();
            return Http3MessageHeadError::kMessageError;
        }();
        if (self.frames.paused()) {
            return;
        }
        if ((decoded.index() != 0)) {
            self.result = std::get<1>(decoded) == Http3MessageHeadError::kQpackDecompressionFailed
                              ? connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed)
                          : std::get<1>(decoded) == Http3MessageHeadError::kFieldSectionTooLarge
                              ? connectionError(Http3ConnectionErrorCode::kExcessiveLoad)
                              : streamError(Http3ConnectionErrorCode::kMessageError);
            return;
        }
        if (std::get<0>(decoded).status < 200) {
            if (std::get<0>(decoded).status == 101) {
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kInformationalHead,
                .streamId = self.streamId,
                .head = &std::get<0>(decoded),
                .requestContentSignal = std::get<0>(decoded).status == 100 ? std::optional{HttpClientRequestContentSignal::kContinue} : std::nullopt};
            self.callback(self.callbackContext, event);
            return;
        }
        self.finalHeaders = true;
        self.tunnel = self.requestMethod == HttpKnownMethod::kConnect && std::get<0>(decoded).status >= 200 && std::get<0>(decoded).status < 300;
        self.bodyPlan = planHttpResponseBody(self.requestMethod, HttpStatusCode::fromValue(std::get<0>(decoded).status));
        const bool payloadAllowed = !self.tunnel && self.bodyPlan->statusAllowsBody() && !self.bodyPlan->bodySuppressed();
        // For HEAD and bodyless statuses Content-Length is representation metadata,
        // not a DATA accounting expectation.
        self.body = Http3MessageBody(payloadAllowed ? std::get<0>(decoded).contentLength : std::nullopt, payloadAllowed);
        const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kFinalHead,
            .streamId = self.streamId,
            .head = &std::get<0>(decoded),
            .responseBodyPlan = self.bodyPlan,
            .requestContentSignal = HttpClientRequestContentSignal::kExchangeComplete};
        self.callback(self.callbackContext, event);
    }
};

Http3ClientResponse::Http3ClientResponse(std::uint64_t streamId, HttpKnownMethod requestMethod,
    std::pmr::memory_resource* resource, Http3ClientResponseLimits limits, Http3QpackDecoder* decoder)
    : resource_(resource),
      impl_(nullptr) {
    if (!resource || limits.maxFields == 0 || limits.maxEncodedFieldSectionBytes == 0 || limits.maxFieldSectionSize == 0) {
        throw std::invalid_argument("HTTP/3 client response requires a resource and positive limits");
    }
    if ((!limits.pushStream && !isHttp3RequestStreamId(streamId)) ||
        (limits.pushStream && http3StreamIdType(streamId) != Http3StreamIdType::kServerUnidirectional)) {
        throw std::invalid_argument("HTTP/3 response stream must be a valid locally initiated bidirectional stream");
    }
    std::pmr::polymorphic_allocator<Impl> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, streamId, requestMethod, resource, limits, decoder);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        impl_ = nullptr;
        throw;
    }
}

Http3ClientResponse::~Http3ClientResponse() {
    if (impl_) {
        std::pmr::polymorphic_allocator<Impl> allocator(resource_);
        std::allocator_traits<decltype(allocator)>::destroy(allocator, impl_);
        allocator.deallocate(impl_, 1);
    }
}

Http3ClientResponse::Http3ClientResponse(Http3ClientResponse&& other) noexcept
    : resource_(other.resource_),
      impl_(other.impl_) {
    other.impl_ = nullptr;
}

Http3ClientResponse& Http3ClientResponse::operator=(Http3ClientResponse&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            std::pmr::polymorphic_allocator<Impl> allocator(resource_);
            std::allocator_traits<decltype(allocator)>::destroy(allocator, impl_);
            allocator.deallocate(impl_, 1);
        }
        resource_ = other.resource_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Http3ClientResponseResult Http3ClientResponse::feed(std::span<const char> bytes, bool fin, bool reset,
    Http3ClientResponseCallback callback, void* context) {
    if (!impl_) {
        return streamError(Http3ConnectionErrorCode::kMessageError);
    }
    auto& self = *impl_;
    if (self.feeding) {
        throw std::logic_error("recursive HTTP/3 client response feed");
    }
    if (self.terminal) {
        return self.result;
    }
    if (!callback) {
        return streamError(Http3ConnectionErrorCode::kMessageError);
    }
    self.feeding = true;
    struct Guard {
        bool& active;
        ~Guard() {
            active = false;
        }
    } guard{self.feeding};
    self.callback = callback;
    self.callbackContext = context;
    self.result = {};
    try {
        if (reset) {
            self.terminal = true;
            self.result = {Http3ClientResponseStatus::kReset, Http3ConnectionErrorScope::kNone,
                Http3ConnectionErrorCode::kNoError};
            const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kReset, .streamId = self.streamId};
            callback(context, event);
            return self.result;
        }
        const auto frameStatus = self.frames.feed(bytes, fin, Impl::onFrame, &self);
        self.result.consumedBytes = self.frames.consumedBytes();
        if (frameStatus == Http3StreamFrameStatus::kPaused) {
            self.result.status = Http3ClientResponseStatus::kQpackBlocked;
            return self.result;
        }
        // A framing failure later in this input can have connection scope even
        // when an earlier message callback found a stream-scoped error.
        if (frameStatus != Http3StreamFrameStatus::kNeedMoreData && frameStatus != Http3StreamFrameStatus::kMessageEnd) {
            self.terminal = true;
            // A response PUSH_PROMISE is connection-fatal because this client
            // has not authorized any push IDs; other framing failures use the
            // shared protocol status mapping.
            const auto code = frameStatus == Http3StreamFrameStatus::kPushPromise && !self.limits.pushStream
                                  ? Http3ConnectionErrorCode::kIdError
                                  : http3ConnectionErrorCodeForStreamFrameStatus(frameStatus)
                                        .value_or(Http3ConnectionErrorCode::kFrameError);
            self.result = connectionError(code);
            return self.result;
        }
        if (self.result.scope != Http3ConnectionErrorScope::kNone) {
            self.terminal = true;
            return self.result;
        }
        if (frameStatus == Http3StreamFrameStatus::kMessageEnd) {
            if (!self.finalHeaders) {
                self.terminal = true;
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return self.result;
            }
            if (!self.tunnel) {
                const auto end = self.body.feed(0, true);
                if (end != Http3MessageBodyResult::kComplete) {
                    self.terminal = true;
                    self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                    return self.result;
                }
            }
            self.terminal = true;
            self.result.status = Http3ClientResponseStatus::kMessageEnd;
            const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kMessageEnd,
                .streamId = self.streamId,
                .responseBodyPlan = self.bodyPlan};
            callback(context, event);
        }
        return self.result;
    } catch (...) {
        self.terminal = true;
        self.result = streamError(Http3ConnectionErrorCode::kMessageError);
        throw;
    }
}

bool Http3ClientResponse::authorizePush(std::uint64_t maximum) noexcept {
    if (!impl_ || impl_->feeding || impl_->limits.pushStream || maximum > kHttp3VarIntMax ||
        (impl_->limits.maxPushId && maximum < *impl_->limits.maxPushId)) {
        return false;
    }
    impl_->limits.maxPushId = maximum;
    impl_->frames.allowPush();
    return true;
}

std::uint64_t Http3ClientResponse::streamId() const noexcept {
    return impl_ ? impl_->streamId : 0;
}

}  // namespace ruvia
