#include "ruvia/http/Http3ClientResponse.h"

#include <limits>
#include <stdexcept>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia {
namespace {

Http3ClientResponseResult streamError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ClientResponseStatus::kStreamError, Http3ConnectionErrorScope::kStream, code};
}

Http3ClientResponseResult connectionError(Http3ConnectionErrorCode code) noexcept {
    return {Http3ClientResponseStatus::kConnectionError, Http3ConnectionErrorScope::kConnection, code};
}

bool validTrailer(Http3FieldSectionFieldView field) noexcept {
    if (field.name.empty() || field.name.front() == ':') {
        return false;
    }
    for (const unsigned char ch : field.name) {
        if (ch >= 'A' && ch <= 'Z') {
            return false;
        }
    }
    return detail::responseTrailerFieldValid(field.name, field.value);
}

}  // namespace

struct Http3ClientResponse::Impl final {
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

    Impl(std::uint64_t stream, HttpKnownMethod method, std::pmr::memory_resource* resource,
        Http3ClientResponseLimits configured)
        : streamId(stream),
          requestMethod(method),
          memory(resource),
          frames(Http3StreamKind::kResponse, resource,
              {.maxFieldSectionSize = configured.maxEncodedFieldSectionBytes,
                  .maxSettingsPayloadBytes = 64 * 1024}),
          limits(configured) {}

    std::uint64_t streamId;
    HttpKnownMethod requestMethod;
    std::pmr::memory_resource* memory;
    Http3StreamFrames frames;
    Http3ClientResponseLimits limits;
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

    static bool collectTrailer(void* opaque, Http3FieldSectionFieldView field) {
        auto& collector = *static_cast<TrailerCollector*>(opaque);
        if (!validTrailer(field)) {
            collector.valid = false;
            return false;
        }
        collector.fields.emplace_back(field, collector.fields.get_allocator().resource());
        return true;
    }

    static void onFrame(void* opaque, Http3StreamFrameEvent frame) {
        auto& self = *static_cast<Impl*>(opaque);
        if (self.terminal || self.result.scope != Http3ConnectionErrorScope::kNone) {
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
            TrailerCollector collector(self.memory);
            const Http3FieldSectionLimits fieldLimits{self.limits.maxEncodedFieldSectionBytes,
                self.limits.maxFieldSectionSize, self.limits.maxFields};
            const auto decoded = decodeHttp3FieldSection(frame.payload, collectTrailer, &collector, fieldLimits, self.memory);
            if (!decoded || !collector.valid) {
                self.result = collector.valid
                                  ? connectionError(decoded.error() == Http3FieldSectionError::kFieldListTooLarge ||
                                                            decoded.error() == Http3FieldSectionError::kFieldSectionTooLarge ||
                                                            decoded.error() == Http3FieldSectionError::kTooManyFields
                                                        ? Http3ConnectionErrorCode::kExcessiveLoad
                                                        : Http3ConnectionErrorCode::kQpackDecompressionFailed)
                                  : streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            self.trailers = true;
            for (const auto& field : collector.fields) {
                const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kTrailerField,
                    .streamId = self.streamId,
                    .trailer = {field.name, field.value, field.neverIndexed}};
                self.callback(self.callbackContext, event);
            }
            return;
        }

        const Http3MessageHeadLimits headLimits{self.limits.maxFieldSectionSize,
            self.limits.maxFields, self.limits.maxEncodedFieldSectionBytes};
        auto decoded = decodeHttp3MessageHead(frame.payload, Http3MessageHeadKind::kResponse, self.memory, headLimits);
        if (!decoded) {
            self.result = decoded.error() == Http3MessageHeadError::kQpackDecompressionFailed
                              ? connectionError(Http3ConnectionErrorCode::kQpackDecompressionFailed)
                          : decoded.error() == Http3MessageHeadError::kFieldSectionTooLarge
                              ? connectionError(Http3ConnectionErrorCode::kExcessiveLoad)
                              : streamError(Http3ConnectionErrorCode::kMessageError);
            return;
        }
        if (decoded->status < 200) {
            if (decoded->status == 101) {
                self.result = streamError(Http3ConnectionErrorCode::kMessageError);
                return;
            }
            const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kInformationalHead,
                .streamId = self.streamId,
                .head = &*decoded};
            self.callback(self.callbackContext, event);
            return;
        }
        self.finalHeaders = true;
        self.tunnel = self.requestMethod == HttpKnownMethod::kConnect && decoded->status >= 200 && decoded->status < 300;
        self.bodyPlan = planHttpResponseBody(self.requestMethod, HttpStatusCode::fromValue(decoded->status));
        const bool payloadAllowed = !self.tunnel && self.bodyPlan->statusAllowsBody() && !self.bodyPlan->bodySuppressed();
        // For HEAD and bodyless statuses Content-Length is representation metadata,
        // not a DATA accounting expectation.
        self.body = Http3MessageBody(payloadAllowed ? decoded->contentLength : std::nullopt, payloadAllowed);
        const Http3ClientResponseEvent event{.kind = Http3ClientResponseEventKind::kFinalHead,
            .streamId = self.streamId,
            .head = &*decoded,
            .responseBodyPlan = self.bodyPlan};
        self.callback(self.callbackContext, event);
    }
};

Http3ClientResponse::Http3ClientResponse(std::uint64_t streamId, HttpKnownMethod requestMethod,
    std::pmr::memory_resource* resource, Http3ClientResponseLimits limits)
    : resource_(resource),
      impl_(nullptr) {
    if (!resource || limits.maxFields == 0 || limits.maxEncodedFieldSectionBytes == 0 || limits.maxFieldSectionSize == 0) {
        throw std::invalid_argument("HTTP/3 client response requires a resource and positive limits");
    }
    if (!isHttp3RequestStreamId(streamId)) {
        throw std::invalid_argument("HTTP/3 response stream must be a valid locally initiated bidirectional stream");
    }
    std::pmr::polymorphic_allocator<Impl> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, streamId, requestMethod, resource, limits);
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
        // A framing failure later in this input can have connection scope even
        // when an earlier message callback found a stream-scoped error.
        if (frameStatus != Http3StreamFrameStatus::kNeedMoreData && frameStatus != Http3StreamFrameStatus::kMessageEnd) {
            self.terminal = true;
            // A response PUSH_PROMISE is connection-fatal because this client
            // has not authorized any push IDs; other framing failures use the
            // shared protocol status mapping.
            const auto code = frameStatus == Http3StreamFrameStatus::kPushPromise
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

std::uint64_t Http3ClientResponse::streamId() const noexcept {
    return impl_ ? impl_->streamId : 0;
}

}  // namespace ruvia
