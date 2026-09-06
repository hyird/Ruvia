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
#include <variant>

#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpExpectations.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

namespace detail {
class Http2Connection;
class Http2ConnectionOwnerEndpoint;
class Http2RequestHeadSubmitResult;
}  // namespace detail

enum class Http2Role : std::uint8_t { kServer,
    kClient };
enum class Http2FeedResult : std::uint8_t {
    kEventsPending,
    kAccepted,
    kNeedInput,
    kProtocolFailure
};
enum class Http2EndStream : std::uint8_t { kKeepOpen,
    kEndStream };
enum class Http2OutputConsumeStatus : std::uint8_t { kPending,
    kDrained,
    kOutOfRange };
enum class Http2SubmitStatus : std::uint8_t {
    kAccepted,
    kClosed,
    kInvalidState,
    kInvalidMessage,
    kPeerCapabilityUnavailable
};
enum class Http2DataSubmitStatus : std::uint8_t {
    kAccepted,
    kQueued,
    kBackpressured,
    kExpectationPending,
    kClosed,
    kInvalidState,
    kContentLengthExceeded,
    kContentLengthIncomplete
};
enum class Http2RequestContentReleaseStatus : std::uint8_t { kReleased,
    kNotPending,
    kClosed };
enum class Http2ServerRequestReleaseStatus : std::uint8_t { kReleased,
    kClosed,
    kInvalidLease };
enum class Http2StreamCloseSource : std::uint8_t { kLocal,
    kPeer,
    kPeerGoaway };

struct Http2ConnectionOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

class Http2RequestContent;

class Http2RequestWithoutContent final {
private:
    friend class Http2RequestContent;
    constexpr Http2RequestWithoutContent() noexcept = default;
};

class Http2KnownLengthRequestContent final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class Http2RequestContent;
    explicit constexpr Http2KnownLengthRequestContent(std::uint64_t length) noexcept
        : length_(length) {}
    std::uint64_t length_;
};

class Http2StreamingRequestContent final {
private:
    friend class Http2RequestContent;
    constexpr Http2StreamingRequestContent() noexcept = default;
};

class Http2RequestContent final {
public:
    [[nodiscard]] static constexpr Http2RequestContent none() noexcept {
        return Http2RequestContent(Http2RequestWithoutContent());
    }
    [[nodiscard]] static constexpr Http2RequestContent knownLength(std::uint64_t length) noexcept {
        return Http2RequestContent(Http2KnownLengthRequestContent(length));
    }
    [[nodiscard]] static constexpr Http2RequestContent streaming() noexcept {
        return Http2RequestContent(Http2StreamingRequestContent());
    }
    [[nodiscard]] constexpr const Http2RequestWithoutContent* withoutContent() const& noexcept {
        return std::get_if<Http2RequestWithoutContent>(&value_);
    }
    const Http2RequestWithoutContent* withoutContent() const&& = delete;
    [[nodiscard]] constexpr const Http2KnownLengthRequestContent* knownLengthContent()
        const& noexcept {
        return std::get_if<Http2KnownLengthRequestContent>(&value_);
    }
    const Http2KnownLengthRequestContent* knownLengthContent() const&& = delete;
    [[nodiscard]] constexpr const Http2StreamingRequestContent* streamingContent() const& noexcept {
        return std::get_if<Http2StreamingRequestContent>(&value_);
    }
    const Http2StreamingRequestContent* streamingContent() const&& = delete;

private:
    using Value = std::variant<Http2RequestWithoutContent, Http2KnownLengthRequestContent,
        Http2StreamingRequestContent>;
    explicit constexpr Http2RequestContent(Http2RequestWithoutContent value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestContent(Http2KnownLengthRequestContent value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestContent(Http2StreamingRequestContent value) noexcept
        : value_(value) {}
    Value value_;
};

struct Http2RegularRequestHeadView final {
    BorrowedText method{"GET"};
    BorrowedText scheme{"https"};
    std::optional<BorrowedText> authority{};
    BorrowedText target{"/"};
    std::span<const HttpHeaderView> headers{};
    Http2RequestContent content{Http2RequestContent::none()};
    HttpClientRequestExpectation expectation{HttpClientRequestExpectation::kNone};
};

struct Http2ConnectRequestHeadView final {
    BorrowedText authority{};
    std::span<const HttpHeaderView> headers{};
};

struct Http2ExtendedConnectRequestHeadView final {
    BorrowedText protocol{};
    BorrowedText scheme{"https"};
    BorrowedText authority{};
    BorrowedText target{"/"};
    std::span<const HttpHeaderView> headers{};
};

enum class Http2RequestHeadSubmitError : std::uint8_t {
    kInvalidState,
    kConnectionUnavailable,
    kPeerStreamLimitReached,
    kLocalStreamCapacityReached,
    kPeerCapabilityUnavailable,
    kInvalidMessage
};

class Http2SubmittedRequestHead final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2RequestHeadSubmitResult;
    explicit constexpr Http2SubmittedRequestHead(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_;
};

class Http2RequestHeadSubmitFailure final {
public:
    [[nodiscard]] constexpr Http2RequestHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2RequestHeadSubmitResult;
    explicit constexpr Http2RequestHeadSubmitFailure(Http2RequestHeadSubmitError error) noexcept
        : error_(error) {}
    Http2RequestHeadSubmitError error_;
};

class Http2RequestHeadSubmitResult final {
public:
    [[nodiscard]] constexpr const Http2SubmittedRequestHead* submitted() const& noexcept {
        return std::get_if<Http2SubmittedRequestHead>(&value_);
    }
    const Http2SubmittedRequestHead* submitted() const&& = delete;
    [[nodiscard]] constexpr const Http2RequestHeadSubmitFailure* failure() const& noexcept {
        return std::get_if<Http2RequestHeadSubmitFailure>(&value_);
    }
    const Http2RequestHeadSubmitFailure* failure() const&& = delete;

private:
    friend class Http2Connection;
    using Value = std::variant<Http2SubmittedRequestHead, Http2RequestHeadSubmitFailure>;
    explicit constexpr Http2RequestHeadSubmitResult(Http2SubmittedRequestHead value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestHeadSubmitResult(Http2RequestHeadSubmitFailure value) noexcept
        : value_(value) {}
    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeSubmitted(
        std::uint32_t streamId) noexcept {
        return Http2RequestHeadSubmitResult(Http2SubmittedRequestHead(streamId));
    }
    [[nodiscard]] static constexpr Http2RequestHeadSubmitResult makeFailure(
        Http2RequestHeadSubmitError error) noexcept {
        return Http2RequestHeadSubmitResult(Http2RequestHeadSubmitFailure(error));
    }
    Value value_;
};

class Http2ReceivedDataCredit final {
public:
    // A credit is linear. acknowledge() consumes it explicitly; destroying a
    // still-valid token returns the same exact byte credit without throwing.
    ~Http2ReceivedDataCredit();
    Http2ReceivedDataCredit(const Http2ReceivedDataCredit&) = delete;
    Http2ReceivedDataCredit& operator=(const Http2ReceivedDataCredit&) = delete;
    Http2ReceivedDataCredit(Http2ReceivedDataCredit&& other) noexcept;
    Http2ReceivedDataCredit& operator=(Http2ReceivedDataCredit&&) = delete;
    [[nodiscard]] bool valid() const noexcept {
        return endpoint_ != nullptr && streamId_ != 0 && bytes_ != 0;
    }

private:
    friend class Http2Connection;
    friend class Http2MessageBodyChunkEvent;
    friend class Http2TunnelDataEvent;
    Http2ReceivedDataCredit(detail::Http2ConnectionOwnerEndpoint* endpoint, std::uint32_t streamId,
        std::uint32_t bytes) noexcept;
    detail::Http2ConnectionOwnerEndpoint* endpoint_{nullptr};
    std::uint32_t streamId_{0};
    std::uint32_t bytes_{0};
};

enum class Http2ReceivedDataAcknowledgeStatus : std::uint8_t {
    kAcknowledged,
    kClosed,
    kInvalidCredit
};

class Http2InformationalHeadEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] const HttpClientResponseHead& head() const& noexcept {
        return head_;
    }
    const HttpClientResponseHead& head() const&& = delete;
    [[nodiscard]] std::optional<HttpClientRequestContentSignal> requestContentSignal()
        const noexcept {
        return signal_;
    }

private:
    friend class Http2Event;
    Http2InformationalHeadEvent(std::uint32_t streamId, HttpClientResponseHead head,
        std::optional<HttpClientRequestContentSignal> signal) noexcept
        : streamId_(streamId),
          head_(std::move(head)),
          signal_(signal) {}
    std::uint32_t streamId_;
    HttpClientResponseHead head_;
    std::optional<HttpClientRequestContentSignal> signal_;
};

// The request-head event is also the stream-storage lease for every view in
// request(). Once processing finishes, release(std::move(event)) consumes the
// event and invalidates those views as one operation.
class Http2RequestHeadEvent final {
public:
    ~Http2RequestHeadEvent();
    Http2RequestHeadEvent(const Http2RequestHeadEvent&) = delete;
    Http2RequestHeadEvent& operator=(const Http2RequestHeadEvent&) = delete;
    Http2RequestHeadEvent(Http2RequestHeadEvent&& other) noexcept;
    Http2RequestHeadEvent& operator=(Http2RequestHeadEvent&&) = delete;
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] const HttpRequest& request() const& noexcept {
        return request_;
    }
    const HttpRequest& request() const&& = delete;
    [[nodiscard]] HttpServerExpectationPlan expectationPlan(
        HttpUnsupportedExpectationPolicy policy) const noexcept {
        return expectations_.serverPlan(content_, policy);
    }

private:
    friend class Http2Connection;
    friend class Http2Event;
    Http2RequestHeadEvent(detail::Http2ConnectionOwnerEndpoint* endpoint, std::uint32_t streamId,
        HttpRequest request, HttpRequestExpectations expectations,
        HttpRequestContentIndication content) noexcept;
    std::uint32_t streamId_;
    HttpRequest request_;
    HttpRequestExpectations expectations_;
    HttpRequestContentIndication content_;
    detail::Http2ConnectionOwnerEndpoint* endpoint_{nullptr};
};

class Http2ResponseHeadEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] const HttpClientResponseHead& head() const& noexcept {
        return head_;
    }
    const HttpClientResponseHead& head() const&& = delete;
    [[nodiscard]] std::optional<HttpClientRequestContentSignal> requestContentSignal()
        const noexcept {
        return signal_;
    }

private:
    friend class Http2Event;
    Http2ResponseHeadEvent(std::uint32_t streamId, HttpClientResponseHead head,
        std::optional<HttpClientRequestContentSignal> signal) noexcept
        : streamId_(streamId),
          head_(std::move(head)),
          signal_(signal) {}
    std::uint32_t streamId_;
    HttpClientResponseHead head_;
    std::optional<HttpClientRequestContentSignal> signal_;
};

class Http2MessageBodyChunkEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] Http2ReceivedDataCredit takeCredit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class Http2Event;
    Http2MessageBodyChunkEvent(detail::Http2ConnectionOwnerEndpoint* endpoint,
        std::uint32_t streamId, std::string_view bytes, std::uint32_t credit) noexcept
        : streamId_(streamId),
          bytes_(bytes),
          credit_(endpoint, streamId, credit) {}
    std::uint32_t streamId_;
    std::string_view bytes_;
    Http2ReceivedDataCredit credit_;
};

class Http2MessageEndEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2MessageEndEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_;
};

class Http2TunnelDataEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] Http2ReceivedDataCredit takeCredit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class Http2Event;
    Http2TunnelDataEvent(detail::Http2ConnectionOwnerEndpoint* endpoint, std::uint32_t streamId,
        std::string_view bytes, std::uint32_t credit) noexcept
        : streamId_(streamId),
          bytes_(bytes),
          credit_(endpoint, streamId, credit) {}
    std::uint32_t streamId_;
    std::string_view bytes_;
    Http2ReceivedDataCredit credit_;
};

class Http2TunnelEndEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2TunnelEndEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_;
};

class Http2StreamClosedEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }
    [[nodiscard]] Http2StreamCloseSource source() const noexcept {
        return source_;
    }
    [[nodiscard]] Http2ErrorCode error() const noexcept {
        return error_;
    }

private:
    friend class Http2Event;
    constexpr Http2StreamClosedEvent(
        std::uint32_t streamId, Http2StreamCloseSource source, Http2ErrorCode error) noexcept
        : streamId_(streamId),
          source_(source),
          error_(error) {}
    std::uint32_t streamId_;
    Http2StreamCloseSource source_;
    Http2ErrorCode error_;
};

class Http2RequestUnprocessedEvent final {
public:
    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2RequestUnprocessedEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_;
};

class Http2GoawayEvent final {
public:
    [[nodiscard]] std::uint32_t lastStreamId() const noexcept {
        return lastStreamId_;
    }
    [[nodiscard]] Http2ErrorCode error() const noexcept {
        return error_;
    }

private:
    friend class Http2Event;
    constexpr Http2GoawayEvent(std::uint32_t lastStreamId, Http2ErrorCode error) noexcept
        : lastStreamId_(lastStreamId),
          error_(error) {}
    std::uint32_t lastStreamId_;
    Http2ErrorCode error_;
};

class Http2Event final {
public:
    Http2Event(const Http2Event&) = delete;
    Http2Event& operator=(const Http2Event&) = delete;
    Http2Event(Http2Event&&) noexcept = default;
    Http2Event& operator=(Http2Event&&) = delete;
    [[nodiscard]] const Http2InformationalHeadEvent* informationalHead() const& noexcept {
        return std::get_if<Http2InformationalHeadEvent>(&value_);
    }
    const Http2InformationalHeadEvent* informationalHead() const&& = delete;
    [[nodiscard]] Http2RequestHeadEvent* requestHead() & noexcept {
        return std::get_if<Http2RequestHeadEvent>(&value_);
    }
    [[nodiscard]] const Http2RequestHeadEvent* requestHead() const& noexcept {
        return std::get_if<Http2RequestHeadEvent>(&value_);
    }
    const Http2RequestHeadEvent* requestHead() const&& = delete;
    [[nodiscard]] const Http2ResponseHeadEvent* responseHead() const& noexcept {
        return std::get_if<Http2ResponseHeadEvent>(&value_);
    }
    const Http2ResponseHeadEvent* responseHead() const&& = delete;
    [[nodiscard]] Http2MessageBodyChunkEvent* messageBodyChunk() & noexcept {
        return std::get_if<Http2MessageBodyChunkEvent>(&value_);
    }
    [[nodiscard]] const Http2MessageBodyChunkEvent* messageBodyChunk() const& noexcept {
        return std::get_if<Http2MessageBodyChunkEvent>(&value_);
    }
    const Http2MessageBodyChunkEvent* messageBodyChunk() const&& = delete;
    [[nodiscard]] const Http2MessageEndEvent* messageEnd() const& noexcept {
        return std::get_if<Http2MessageEndEvent>(&value_);
    }
    const Http2MessageEndEvent* messageEnd() const&& = delete;
    [[nodiscard]] Http2TunnelDataEvent* tunnelData() & noexcept {
        return std::get_if<Http2TunnelDataEvent>(&value_);
    }
    [[nodiscard]] const Http2TunnelDataEvent* tunnelData() const& noexcept {
        return std::get_if<Http2TunnelDataEvent>(&value_);
    }
    const Http2TunnelDataEvent* tunnelData() const&& = delete;
    [[nodiscard]] const Http2TunnelEndEvent* tunnelEnd() const& noexcept {
        return std::get_if<Http2TunnelEndEvent>(&value_);
    }
    const Http2TunnelEndEvent* tunnelEnd() const&& = delete;
    [[nodiscard]] const Http2StreamClosedEvent* streamClosed() const& noexcept {
        return std::get_if<Http2StreamClosedEvent>(&value_);
    }
    const Http2StreamClosedEvent* streamClosed() const&& = delete;
    [[nodiscard]] const Http2RequestUnprocessedEvent* requestUnprocessed() const& noexcept {
        return std::get_if<Http2RequestUnprocessedEvent>(&value_);
    }
    const Http2RequestUnprocessedEvent* requestUnprocessed() const&& = delete;
    [[nodiscard]] const Http2GoawayEvent* goaway() const& noexcept {
        return std::get_if<Http2GoawayEvent>(&value_);
    }
    const Http2GoawayEvent* goaway() const&& = delete;

private:
    friend class Http2Connection;
    using Value = std::variant<Http2InformationalHeadEvent, Http2RequestHeadEvent,
        Http2ResponseHeadEvent, Http2MessageBodyChunkEvent, Http2MessageEndEvent,
        Http2TunnelDataEvent, Http2TunnelEndEvent, Http2StreamClosedEvent,
        Http2RequestUnprocessedEvent, Http2GoawayEvent>;
    template <typename Event>
    explicit Http2Event(Event event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static Http2Event informationalHead(std::uint32_t id, HttpClientResponseHead head,
        std::optional<HttpClientRequestContentSignal> signal) noexcept {
        return Http2Event(Http2InformationalHeadEvent(id, std::move(head), signal));
    }
    [[nodiscard]] static Http2Event requestHead(detail::Http2ConnectionOwnerEndpoint* endpoint,
        std::uint32_t id, HttpRequest request, HttpRequestExpectations expectations,
        HttpRequestContentIndication content) noexcept {
        return Http2Event(
            Http2RequestHeadEvent(endpoint, id, std::move(request), expectations, content));
    }
    [[nodiscard]] static Http2Event responseHead(std::uint32_t id, HttpClientResponseHead head,
        std::optional<HttpClientRequestContentSignal> signal) noexcept {
        return Http2Event(Http2ResponseHeadEvent(id, std::move(head), signal));
    }
    [[nodiscard]] static Http2Event messageBodyChunk(detail::Http2ConnectionOwnerEndpoint* endpoint,
        std::uint32_t id, std::string_view bytes, std::uint32_t credit) noexcept {
        return Http2Event(Http2MessageBodyChunkEvent(endpoint, id, bytes, credit));
    }
    [[nodiscard]] static Http2Event messageEnd(std::uint32_t id) noexcept {
        return Http2Event(Http2MessageEndEvent(id));
    }
    [[nodiscard]] static Http2Event tunnelData(detail::Http2ConnectionOwnerEndpoint* endpoint,
        std::uint32_t id, std::string_view bytes, std::uint32_t credit) noexcept {
        return Http2Event(Http2TunnelDataEvent(endpoint, id, bytes, credit));
    }
    [[nodiscard]] static Http2Event tunnelEnd(std::uint32_t id) noexcept {
        return Http2Event(Http2TunnelEndEvent(id));
    }
    [[nodiscard]] static Http2Event streamClosed(
        std::uint32_t id, Http2StreamCloseSource source, Http2ErrorCode error) noexcept {
        return Http2Event(Http2StreamClosedEvent(id, source, error));
    }
    [[nodiscard]] static Http2Event requestUnprocessed(std::uint32_t id) noexcept {
        return Http2Event(Http2RequestUnprocessedEvent(id));
    }
    [[nodiscard]] static Http2Event goaway(
        std::uint32_t lastStreamId, Http2ErrorCode error) noexcept {
        return Http2Event(Http2GoawayEvent(lastStreamId, error));
    }
    Value value_;
};

// Stable HTTP/2 sans-I/O driver. Feed transport bytes, drain typed events, and
// flush pendingOutput(); the class owns all HPACK, stream and flow-control state.
// The storage behind an accepted feed() input must remain alive and unchanged
// until every event produced by that call has been taken and every event-derived
// byte view has expired. Calling feed() again may invalidate those views; owning
// string temporaries are rejected at compile time.
class Http2Connection final {
public:
    [[nodiscard]] static Http2Connection server(Http2ConnectionOptions options = {});
    [[nodiscard]] static Http2Connection client(Http2ConnectionOptions options = {});
    ~Http2Connection();
    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;
    Http2Connection(Http2Connection&&) noexcept;
    Http2Connection& operator=(Http2Connection&&) noexcept;

    [[nodiscard]] Http2Role role() const noexcept;
    [[nodiscard]] Http2FeedResult feed(std::string_view input);
    template <detail::HttpTemporaryOwningCharString Input>
    Http2FeedResult feed(Input&&) = delete;
    [[nodiscard]] std::optional<Http2Event> nextEvent();
    [[nodiscard]] std::string_view pendingOutput() const& noexcept;
    std::string_view pendingOutput() const&& = delete;
    [[nodiscard]] Http2OutputConsumeStatus consumeOutput(std::size_t bytes) noexcept;
    void takeOutput(std::pmr::string& output);
    [[nodiscard]] bool wantsWrite() const noexcept;

    [[nodiscard]] Http2RequestHeadSubmitResult submitRequestHead(
        const Http2RegularRequestHeadView& request);
    [[nodiscard]] Http2RequestHeadSubmitResult submitRequestHead(
        const Http2ConnectRequestHeadView& request);
    [[nodiscard]] Http2RequestHeadSubmitResult submitRequestHead(
        const Http2ExtendedConnectRequestHeadView& request);
    [[nodiscard]] Http2DataSubmitStatus submitData(
        std::uint32_t streamId, std::string_view bytes, Http2EndStream endStream);
    [[nodiscard]] Http2RequestContentReleaseStatus releaseRequestContent(
        std::uint32_t streamId) noexcept;
    [[nodiscard]] Http2SubmitStatus submitInterimResponseHead(
        std::uint32_t streamId, const HttpInterimResponseHead& response);
    [[nodiscard]] Http2SubmitStatus submitBufferedResponse(
        std::uint32_t streamId, const HttpResponse& response);
    // Submit a final generic response head without trailers and leave an eligible
    // response body open for subsequent submitData() calls. Content-Length is
    // not generated automatically; an explicit value, when present, constrains
    // the total submitted DATA bytes.
    [[nodiscard]] Http2SubmitStatus submitStreamingResponseHead(
        std::uint32_t streamId, HttpResponse response);
    [[nodiscard]] Http2SubmitStatus submitReset(std::uint32_t streamId, Http2ErrorCode error);
    [[nodiscard]] Http2ReceivedDataAcknowledgeStatus acknowledge(Http2ReceivedDataCredit&& credit);
    [[nodiscard]] bool hasQueuedData(std::uint32_t streamId) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> takeDrainedDataStreams() & noexcept;
    std::span<const std::uint32_t> takeDrainedDataStreams() && = delete;
    // Consume the server request event only after every request-derived view has
    // expired. An event cannot be released through another connection.
    [[nodiscard]] Http2ServerRequestReleaseStatus release(Http2RequestHeadEvent&& request);
    void beginDrain();
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] std::optional<Http2ErrorCode> connectionError() const noexcept;

private:
    [[nodiscard]] static Http2RequestHeadSubmitResult pinSubmittedRequest(
        detail::Http2Connection& connection, const detail::Http2RequestHeadSubmitResult& result);
    explicit Http2Connection(std::pmr::memory_resource* resource, Http2Role role);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
