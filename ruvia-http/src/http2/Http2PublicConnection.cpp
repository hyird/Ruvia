#include "ruvia/http/Http2Connection.h"

#include <stdexcept>

#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::Http2RequestContent toInternal(Http2RequestContent content) noexcept {
    switch (content.kind()) {
        case Http2RequestContent::Kind::kNone: return detail::Http2RequestContent::none();
        case Http2RequestContent::Kind::kKnownLength: return detail::Http2RequestContent::knownLength(content.length());
        case Http2RequestContent::Kind::kStreaming: return detail::Http2RequestContent::streaming();
    }
    std::terminate();
}

[[nodiscard]] HttpClientResponseHead copyResponseHead(const HttpClientResponseHead& source, std::pmr::memory_resource* resource) {
    auto result = detail::HttpClientResponseHeadAccess::make(source.status(), source.protocolVersion(), resource);
    auto& headers = detail::HttpClientResponseHeadAccess::headers(result);
    headers.reserve(source.headers().size());
    for (const auto& field : source.headers()) {
        headers.push_back(detail::HttpClientResponseHeaderAccess::make(field.name(), field.value(), resource));
    }
    return result;
}

[[nodiscard]] HttpClientResponseHead responseHeadFromStream(const detail::Http2StreamState& stream, std::pmr::memory_resource* resource) {
    const auto* status = stream.responseStatus();
    if (status == nullptr) throw std::logic_error("HTTP/2 final response event has no status");
    auto result = detail::HttpClientResponseHeadAccess::make(*status, HttpProtocolVersion::kHttp2, resource);
    auto& headers = detail::HttpClientResponseHeadAccess::headers(result);
    const auto count = stream.remoteInitialHeaderCount().value_or(stream.remoteHeaderCount());
    headers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto field = stream.remoteHeaderAt(index);
        headers.push_back(detail::HttpClientResponseHeaderAccess::make(field.name, field.value, resource));
    }
    return result;
}

}  // namespace

class Http2Connection::Impl final {
public:
    Impl(std::pmr::memory_resource* requested, Http2Role publicRole)
        : resource(detail::httpPmrResourceOrDefault(requested)),
          role(publicRole),
          connection(resource, static_cast<detail::Http2Role>(publicRole)) {}

    std::pmr::memory_resource* resource;
    Http2Role role;
    detail::Http2Connection connection;
};

Http2Connection::Http2Connection(std::pmr::memory_resource* resource, Http2Role role)
    : impl_(std::make_unique<Impl>(resource, role)) {}
Http2Connection::~Http2Connection() = default;
Http2Connection::Http2Connection(Http2Connection&&) noexcept = default;
Http2Connection& Http2Connection::operator=(Http2Connection&&) noexcept = default;

Http2Role Http2Connection::role() const noexcept { return impl_->role; }
void Http2Connection::beginConnection() { impl_->connection.beginConnection(); }
Http2FeedResult Http2Connection::feed(std::string_view input) { return static_cast<Http2FeedResult>(impl_->connection.feed(input)); }

std::optional<Http2Event> Http2Connection::nextEvent() {
    auto event = impl_->connection.nextEvent();
    if (!event) return std::nullopt;
    if (const auto* value = event->informationalHead()) {
        auto head = copyResponseHead(value->head(), impl_->resource);
        return Http2Event(Http2EventKind::kInformationalHead, value->streamId(), {}, value->requestContentSignal(), std::nullopt, std::nullopt, std::nullopt, std::optional<HttpClientResponseHead>(std::move(head)));
    }
    if (const auto* value = event->messageHead()) {
        if (impl_->role == Http2Role::kServer) {
            auto* stream = impl_->connection.stream(value->streamId());
            if (stream == nullptr) throw std::logic_error("HTTP/2 request event has no stream");
            impl_->connection.pinStream(value->streamId());
            auto request = detail::HttpRequestAccess::make();
            const auto build = detail::Http2RequestBuilder::build(*stream, request, impl_->resource, {});
            if (build.built() == nullptr) {
                impl_->connection.unpinStream(value->streamId());
                throw std::logic_error("validated HTTP/2 request cannot be materialized");
            }
            return Http2Event(Http2EventKind::kMessageHead, value->streamId(), {}, value->requestContentSignal(), std::nullopt, std::nullopt, std::optional<HttpRequest>(std::move(request)), std::nullopt, stream->requestExpectations(), stream->requestContentIndication());
        }
        auto* stream = impl_->connection.stream(value->streamId());
        if (stream == nullptr) throw std::logic_error("HTTP/2 response event has no stream");
        auto head = responseHeadFromStream(*stream, impl_->resource);
        return Http2Event(Http2EventKind::kMessageHead, value->streamId(), {}, value->requestContentSignal(), std::nullopt, std::nullopt, std::nullopt, std::optional<HttpClientResponseHead>(std::move(head)));
    }
    if (const auto* value = event->messageBodyChunk()) return Http2Event(Http2EventKind::kMessageBodyChunk, value->streamId(), value->bytes());
    if (const auto* value = event->messageEnd()) {
        auto result = Http2Event(Http2EventKind::kMessageEnd, value->streamId());
        if (impl_->role == Http2Role::kClient) impl_->connection.unpinStream(value->streamId());
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->tunnelData()) return Http2Event(Http2EventKind::kTunnelData, value->streamId(), value->bytes());
    if (const auto* value = event->tunnelEnd()) {
        auto result = Http2Event(Http2EventKind::kTunnelEnd, value->streamId());
        if (impl_->role == Http2Role::kClient) impl_->connection.unpinStream(value->streamId());
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->streamClosed()) {
        auto result = Http2Event(Http2EventKind::kStreamClosed, value->streamId(), {}, std::nullopt, static_cast<Http2ErrorCode>(value->error()), static_cast<Http2StreamCloseSource>(value->source()));
        if (impl_->role == Http2Role::kClient) impl_->connection.unpinStream(value->streamId());
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->requestUnprocessed()) {
        auto result = Http2Event(Http2EventKind::kRequestUnprocessed, value->streamId());
        if (impl_->role == Http2Role::kClient) impl_->connection.unpinStream(value->streamId());
        return std::optional<Http2Event>(std::move(result));
    }
    const auto* value = event->goaway();
    return Http2Event(Http2EventKind::kGoaway, value->lastStreamId(), {}, std::nullopt, static_cast<Http2ErrorCode>(value->error()));
}

std::string_view Http2Connection::pendingOutput() const& noexcept { return impl_->connection.pendingOutput(); }
Http2OutputConsumeStatus Http2Connection::consumeOutput(std::size_t bytes) noexcept { return static_cast<Http2OutputConsumeStatus>(impl_->connection.consumeOutput(bytes)); }
void Http2Connection::takeOutput(std::pmr::string& output) { impl_->connection.takeOutput(output); }
bool Http2Connection::wantsWrite() const noexcept { return impl_->connection.wantsWrite(); }

Http2RequestHeadSubmitResult Http2Connection::submitRegularRequestHead(std::string_view method, std::string_view scheme, std::optional<std::string_view> authority, std::string_view path, std::span<const HttpHeaderView> headers, Http2RequestContent content, HttpClientRequestExpectation expectation) {
    const auto result = impl_->connection.submitRegularRequestHead(method, scheme, authority, path, headers, toInternal(content), expectation);
    if (const auto* submitted = result.submitted()) {
        try {
            impl_->connection.pinStream(submitted->streamId());
        } catch (...) {
            (void)impl_->connection.submitReset(submitted->streamId(), detail::Http2ErrorCode::kCancel);
            throw;
        }
        return Http2RequestHeadSubmitResult(submitted->streamId(), std::nullopt);
    }
    return Http2RequestHeadSubmitResult(std::nullopt, static_cast<Http2RequestHeadSubmitError>(result.failure()->error()));
}

Http2DataSubmitStatus Http2Connection::submitData(std::uint32_t streamId, std::string_view bytes, Http2EndStream endStream) {
    return static_cast<Http2DataSubmitStatus>(impl_->connection.submitData(streamId, bytes, static_cast<detail::Http2EndStream>(endStream)));
}
Http2RequestContentReleaseStatus Http2Connection::releaseRequestContent(std::uint32_t streamId) noexcept { return static_cast<Http2RequestContentReleaseStatus>(impl_->connection.releaseRequestContent(streamId)); }
Http2SubmitStatus Http2Connection::submitInterimResponseHead(std::uint32_t streamId, const HttpInterimResponseHead& response) { return static_cast<Http2SubmitStatus>(impl_->connection.submitInterimResponseHead(streamId, response)); }

Http2SubmitStatus Http2Connection::submitBufferedResponse(std::uint32_t streamId, const HttpResponse& response) {
    auto* stream = impl_->connection.stream(streamId);
    if (stream == nullptr) return Http2SubmitStatus::kClosed;
    const auto& body = detail::responseBody(response);
    if (body.file().has_value()) return Http2SubmitStatus::kInvalidMessage;
    const auto plan = detail::httpBufferedResponseWritePlan(stream->requestKnownMethod(), response);
    const auto result = impl_->connection.submitResponseHead(streamId, response, plan);
    if (const auto* failure = result.failure()) {
        switch (failure->error()) {
            case detail::Http2ResponseHeadSubmitError::kClosed: return Http2SubmitStatus::kClosed;
            case detail::Http2ResponseHeadSubmitError::kInvalidState:
            case detail::Http2ResponseHeadSubmitError::kResponsePlanMismatch: return Http2SubmitStatus::kInvalidState;
            case detail::Http2ResponseHeadSubmitError::kInvalidMessage: return Http2SubmitStatus::kInvalidMessage;
        }
    }
    if (plan.sendBody()) {
        const auto data = impl_->connection.submitData(streamId, body.bytes(), detail::Http2EndStream::kEndStream);
        if (data != detail::Http2DataSubmitStatus::kAccepted && data != detail::Http2DataSubmitStatus::kQueued) return Http2SubmitStatus::kInvalidState;
    }
    return Http2SubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitReset(std::uint32_t streamId, Http2ErrorCode error) {
    const auto status = impl_->connection.submitReset(streamId, static_cast<detail::Http2ErrorCode>(error));
    if (status == detail::Http2SubmitStatus::kAccepted && impl_->role == Http2Role::kClient) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submitRegularRequestHead() here rather than waiting for nextEvent().
        impl_->connection.unpinStream(streamId);
    }
    return static_cast<Http2SubmitStatus>(status);
}
void Http2Connection::releaseReceivedData(std::uint32_t streamId) { impl_->connection.releaseReceivedData(streamId); }
bool Http2Connection::hasQueuedData(std::uint32_t streamId) const noexcept { return impl_->connection.hasQueuedData(streamId); }
std::span<const std::uint32_t> Http2Connection::takeDrainedDataStreams() & noexcept { return impl_->connection.takeDrainedDataStreams(); }
Http2ServerRequestReleaseStatus Http2Connection::releaseServerRequest(std::uint32_t streamId) {
    if (impl_->role != Http2Role::kServer) return Http2ServerRequestReleaseStatus::kInvalidRole;
    if (impl_->connection.stream(streamId) == nullptr) return Http2ServerRequestReleaseStatus::kClosed;
    impl_->connection.unpinStream(streamId);
    return Http2ServerRequestReleaseStatus::kReleased;
}
void Http2Connection::beginDrain() { impl_->connection.beginDrain(); }
bool Http2Connection::draining() const noexcept { return impl_->connection.draining(); }
std::optional<Http2ErrorCode> Http2Connection::connectionError() const noexcept {
    const auto error = impl_->connection.connectionError();
    return error ? std::optional<Http2ErrorCode>(static_cast<Http2ErrorCode>(*error)) : std::nullopt;
}

}  // namespace ruvia
