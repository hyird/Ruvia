#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpExpectations.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

enum class Http2Role : std::uint8_t { kServer, kClient };
enum class Http2FeedResult : std::uint8_t { kConnectionNotStarted, kEventsPending, kAccepted, kNeedInput, kProtocolFailure };
enum class Http2EndStream : std::uint8_t { kKeepOpen, kEndStream };
enum class Http2OutputConsumeStatus : std::uint8_t { kPending, kDrained, kOutOfRange };
enum class Http2SubmitStatus : std::uint8_t { kAccepted, kClosed, kInvalidState, kInvalidMessage, kPeerCapabilityUnavailable };
enum class Http2DataSubmitStatus : std::uint8_t { kAccepted, kQueued, kBackpressured, kExpectationPending, kClosed, kInvalidState, kContentLengthExceeded, kContentLengthIncomplete };
enum class Http2RequestContentReleaseStatus : std::uint8_t { kReleased, kNotPending, kClosed };
enum class Http2ServerRequestReleaseStatus : std::uint8_t { kReleased, kClosed, kInvalidRole };
enum class Http2StreamCloseSource : std::uint8_t { kLocal, kPeer, kPeerGoaway };

class Http2RequestContent final {
public:
    enum class Kind : std::uint8_t { kNone, kKnownLength, kStreaming };
    [[nodiscard]] static constexpr Http2RequestContent none() noexcept { return Http2RequestContent(Kind::kNone, 0); }
    [[nodiscard]] static constexpr Http2RequestContent knownLength(std::uint64_t length) noexcept { return Http2RequestContent(Kind::kKnownLength, length); }
    [[nodiscard]] static constexpr Http2RequestContent streaming() noexcept { return Http2RequestContent(Kind::kStreaming, 0); }
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept { return length_; }
private:
    constexpr Http2RequestContent(Kind kind, std::uint64_t length) noexcept : kind_(kind), length_(length) {}
    Kind kind_;
    std::uint64_t length_;
};

enum class Http2RequestHeadSubmitError : std::uint8_t { kInvalidState, kConnectionNotStarted, kConnectionUnavailable, kPeerStreamLimitReached, kLocalStreamCapacityReached, kPeerCapabilityUnavailable, kInvalidMessage };

class Http2RequestHeadSubmitResult final {
public:
    [[nodiscard]] constexpr std::optional<std::uint32_t> streamId() const noexcept { return streamId_; }
    [[nodiscard]] constexpr std::optional<Http2RequestHeadSubmitError> error() const noexcept { return error_; }
private:
    friend class Http2Connection;
    constexpr Http2RequestHeadSubmitResult(std::optional<std::uint32_t> streamId, std::optional<Http2RequestHeadSubmitError> error) noexcept : streamId_(streamId), error_(error) {}
    std::optional<std::uint32_t> streamId_;
    std::optional<Http2RequestHeadSubmitError> error_;
};

enum class Http2EventKind : std::uint8_t { kInformationalHead, kMessageHead, kMessageBodyChunk, kMessageEnd, kTunnelData, kTunnelEnd, kStreamClosed, kRequestUnprocessed, kGoaway };

// One move-only protocol event. A server message-head owns an HttpRequest view;
// its stream is pinned by the driver until releaseServerRequest() releases that
// borrow.
// A client message-head owns the final response head. Body/tunnel bytes borrow
// the accepted feed buffer and remain valid until the next feed(). Client
// streams are pinned automatically and released after their terminal event.
class Http2Event final {
public:
    Http2Event(const Http2Event&) = delete;
    Http2Event& operator=(const Http2Event&) = delete;
    Http2Event(Http2Event&&) noexcept = default;
    Http2Event& operator=(Http2Event&&) = delete;

    [[nodiscard]] constexpr Http2EventKind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept { return streamId_; }
    [[nodiscard]] constexpr std::string_view bytes() const noexcept { return bytes_; }
    [[nodiscard]] constexpr std::optional<HttpClientRequestContentSignal> requestContentSignal() const noexcept { return requestContentSignal_; }
    // Present only on a server request-head event. This is the same typed
    // decision used by Http1RequestBodyPlan::expectationPlan().
    [[nodiscard]] constexpr std::optional<HttpServerExpectationPlan> serverExpectationPlan(HttpUnsupportedExpectationPolicy unsupportedPolicy) const noexcept {
        if (!requestExpectations_ || !requestContentIndication_) return std::nullopt;
        return requestExpectations_->serverPlan(*requestContentIndication_, unsupportedPolicy);
    }
    [[nodiscard]] constexpr std::optional<Http2ErrorCode> error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<Http2StreamCloseSource> closeSource() const noexcept { return closeSource_; }
    [[nodiscard]] const HttpRequest* request() const& noexcept { return request_ ? &*request_ : nullptr; }
    const HttpRequest* request() const&& = delete;
    [[nodiscard]] const HttpClientResponseHead* response() const& noexcept { return response_ ? &*response_ : nullptr; }
    const HttpClientResponseHead* response() const&& = delete;

private:
    friend class Http2Connection;
    Http2Event(Http2EventKind kind, std::uint32_t streamId, std::string_view bytes = {}, std::optional<HttpClientRequestContentSignal> signal = std::nullopt,
        std::optional<Http2ErrorCode> error = std::nullopt, std::optional<Http2StreamCloseSource> closeSource = std::nullopt,
        std::optional<HttpRequest> request = std::nullopt, std::optional<HttpClientResponseHead> response = std::nullopt,
        std::optional<HttpRequestExpectations> requestExpectations = std::nullopt, std::optional<HttpRequestContentIndication> requestContentIndication = std::nullopt)
        : kind_(kind), streamId_(streamId), bytes_(bytes), requestContentSignal_(signal), error_(error), closeSource_(closeSource), request_(std::move(request)), response_(std::move(response)), requestExpectations_(requestExpectations), requestContentIndication_(requestContentIndication) {}

    Http2EventKind kind_;
    std::uint32_t streamId_{0};
    std::string_view bytes_;
    std::optional<HttpClientRequestContentSignal> requestContentSignal_;
    std::optional<Http2ErrorCode> error_;
    std::optional<Http2StreamCloseSource> closeSource_;
    std::optional<HttpRequest> request_;
    std::optional<HttpClientResponseHead> response_;
    std::optional<HttpRequestExpectations> requestExpectations_;
    std::optional<HttpRequestContentIndication> requestContentIndication_;
};

// Stable HTTP/2 sans-I/O driver. Feed transport bytes, drain typed events, and
// flush pendingOutput(); the class owns all HPACK, stream and flow-control state.
class Http2Connection final {
public:
    explicit Http2Connection(std::pmr::memory_resource* resource = nullptr, Http2Role role = Http2Role::kServer);
    ~Http2Connection();
    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;
    Http2Connection(Http2Connection&&) noexcept;
    Http2Connection& operator=(Http2Connection&&) noexcept;

    [[nodiscard]] Http2Role role() const noexcept;
    void beginConnection();
    [[nodiscard]] Http2FeedResult feed(std::string_view input);
    [[nodiscard]] std::optional<Http2Event> nextEvent();
    [[nodiscard]] std::string_view pendingOutput() const& noexcept;
    std::string_view pendingOutput() const&& = delete;
    [[nodiscard]] Http2OutputConsumeStatus consumeOutput(std::size_t bytes) noexcept;
    void takeOutput(std::pmr::string& output);
    [[nodiscard]] bool wantsWrite() const noexcept;

    [[nodiscard]] Http2RequestHeadSubmitResult submitRegularRequestHead(std::string_view method, std::string_view scheme, std::optional<std::string_view> authority, std::string_view path, std::span<const HttpHeaderView> headers, Http2RequestContent content, HttpClientRequestExpectation expectation = HttpClientRequestExpectation::kNone);
    [[nodiscard]] Http2DataSubmitStatus submitData(std::uint32_t streamId, std::string_view bytes, Http2EndStream endStream);
    [[nodiscard]] Http2RequestContentReleaseStatus releaseRequestContent(std::uint32_t streamId) noexcept;
    [[nodiscard]] Http2SubmitStatus submitInterimResponseHead(std::uint32_t streamId, const HttpInterimResponseHead& response);
    [[nodiscard]] Http2SubmitStatus submitBufferedResponse(std::uint32_t streamId, const HttpResponse& response);
    [[nodiscard]] Http2SubmitStatus submitReset(std::uint32_t streamId, Http2ErrorCode error);
    void releaseReceivedData(std::uint32_t streamId);
    [[nodiscard]] bool hasQueuedData(std::uint32_t streamId) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> takeDrainedDataStreams() & noexcept;
    std::span<const std::uint32_t> takeDrainedDataStreams() && = delete;
    // Server request events pin their stream automatically. Release the lease
    // only after every request-derived view has expired.
    [[nodiscard]] Http2ServerRequestReleaseStatus releaseServerRequest(std::uint32_t streamId);
    void beginDrain();
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] std::optional<Http2ErrorCode> connectionError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
