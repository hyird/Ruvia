#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"
#include "ruvia/http/detail/http2/stream/Http2StreamCloseSource.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

class Http2Connection;

// The latest GOAWAY received from the peer. Last-Stream-ID is a connection-level
// processing boundary, not the identifier of an event stream.
class Http2PeerGoaway final {
public:
    constexpr Http2PeerGoaway(std::uint32_t lastStreamId, Http2ErrorCode error) noexcept
        : lastStreamId_(lastStreamId),
          error_(error) {}

    [[nodiscard]] constexpr std::uint32_t lastStreamId() const noexcept {
        return lastStreamId_;
    }

    [[nodiscard]] constexpr Http2ErrorCode error() const noexcept {
        return error_;
    }

private:
    std::uint32_t lastStreamId_{0};
    Http2ErrorCode error_{Http2ErrorCode::kNoError};
};

enum class Http2EventKind : std::uint8_t { kInformationalHead, kMessageHead, kMessageBodyChunk, kMessageEnd, kTunnelData, kTunnelEnd, kStreamClosed, kRequestUnprocessed, kGoaway };

class Http2InformationalHeadEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    [[nodiscard]] const HttpClientResponseHead& head() const& noexcept {
        return head_;
    }
    [[nodiscard]] const HttpClientResponseHead& head() const&& = delete;

    [[nodiscard]] constexpr std::optional<HttpClientRequestContentSignal> requestContentSignal() const noexcept {
        return requestContentSignal_;
    }

private:
    friend class Http2Event;
    Http2InformationalHeadEvent(std::uint32_t streamId, HttpClientResponseHead head, std::optional<HttpClientRequestContentSignal> requestContentSignal) noexcept
        : streamId_(streamId),
          head_(std::move(head)),
          requestContentSignal_(requestContentSignal) {}

    std::uint32_t streamId_{0};
    HttpClientResponseHead head_;
    std::optional<HttpClientRequestContentSignal> requestContentSignal_;
};

class Http2MessageHeadEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    [[nodiscard]] constexpr std::optional<HttpClientRequestContentSignal> requestContentSignal() const noexcept {
        return requestContentSignal_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2MessageHeadEvent(std::uint32_t streamId, std::optional<HttpClientRequestContentSignal> requestContentSignal) noexcept
        : streamId_(streamId),
          requestContentSignal_(requestContentSignal) {}
    std::uint32_t streamId_{0};
    std::optional<HttpClientRequestContentSignal> requestContentSignal_;
};

class Http2MessageBodyChunkEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    // This view remains valid until the next feed() call that consumes input.
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class Http2Event;
    constexpr Http2MessageBodyChunkEvent(std::uint32_t streamId, std::string_view bytes) noexcept
        : streamId_(streamId),
          bytes_(bytes) {}
    std::uint32_t streamId_{0};
    std::string_view bytes_{};
};

class Http2MessageEndEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2MessageEndEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_{0};
};

class Http2TunnelDataEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    // This view remains valid until the next feed() call that consumes input.
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class Http2Event;
    constexpr Http2TunnelDataEvent(std::uint32_t streamId, std::string_view bytes) noexcept
        : streamId_(streamId),
          bytes_(bytes) {}
    std::uint32_t streamId_{0};
    std::string_view bytes_{};
};

class Http2TunnelEndEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2TunnelEndEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_{0};
};

class Http2StreamClosedEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    [[nodiscard]] constexpr Http2StreamCloseSource source() const noexcept {
        return source_;
    }

    // RFC 9113 section 6.4 makes this the reason carried by RST_STREAM.
    [[nodiscard]] constexpr Http2ErrorCode error() const noexcept {
        return error_;
    }

private:
    friend class Http2Event;
    constexpr Http2StreamClosedEvent(std::uint32_t streamId, Http2StreamCloseSource source, Http2ErrorCode error) noexcept
        : streamId_(streamId),
          source_(source),
          error_(error) {}
    std::uint32_t streamId_{0};
    Http2StreamCloseSource source_;
    Http2ErrorCode error_{Http2ErrorCode::kNoError};
};

// A client request above the peer GOAWAY boundary was not processed and is safe
// to retry. The GOAWAY error remains connection metadata on Http2GoawayEvent and
// Http2Connection::peerGoaway(); it is not duplicated into each request event.
class Http2RequestUnprocessedEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t streamId() const noexcept {
        return streamId_;
    }

private:
    friend class Http2Event;
    explicit constexpr Http2RequestUnprocessedEvent(std::uint32_t streamId) noexcept
        : streamId_(streamId) {}
    std::uint32_t streamId_{0};
};

class Http2GoawayEvent final {
public:
    [[nodiscard]] constexpr std::uint32_t lastStreamId() const noexcept {
        return peerGoaway_.lastStreamId();
    }

    [[nodiscard]] constexpr Http2ErrorCode error() const noexcept {
        return peerGoaway_.error();
    }

    [[nodiscard]] constexpr const Http2PeerGoaway& peerGoaway() const& noexcept {
        return peerGoaway_;
    }
    [[nodiscard]] constexpr const Http2PeerGoaway& peerGoaway() const&& = delete;

private:
    friend class Http2Event;
    explicit constexpr Http2GoawayEvent(Http2PeerGoaway peerGoaway) noexcept
        : peerGoaway_(peerGoaway) {}
    Http2PeerGoaway peerGoaway_;
};

// A zero-allocation discriminated event. There is deliberately no kNone
// alternative: nextEvent() uses std::optional to represent an empty queue, so
// every materialized Http2Event has exactly one valid payload.
class Http2Event final {
public:
    Http2Event(const Http2Event&) = delete;
    Http2Event& operator=(const Http2Event&) = delete;
    Http2Event(Http2Event&&) noexcept = default;
    Http2Event& operator=(Http2Event&&) = delete;

    [[nodiscard]] Http2EventKind kind() const noexcept {
        return static_cast<Http2EventKind>(value_.index());
    }

    [[nodiscard]] const Http2InformationalHeadEvent* informationalHead() const& noexcept {
        return std::get_if<Http2InformationalHeadEvent>(&value_);
    }
    [[nodiscard]] const Http2InformationalHeadEvent* informationalHead() const&& = delete;

    [[nodiscard]] const Http2MessageHeadEvent* messageHead() const& noexcept {
        return std::get_if<Http2MessageHeadEvent>(&value_);
    }
    [[nodiscard]] const Http2MessageHeadEvent* messageHead() const&& = delete;

    [[nodiscard]] const Http2MessageBodyChunkEvent* messageBodyChunk() const& noexcept {
        return std::get_if<Http2MessageBodyChunkEvent>(&value_);
    }
    [[nodiscard]] const Http2MessageBodyChunkEvent* messageBodyChunk() const&& = delete;

    [[nodiscard]] const Http2MessageEndEvent* messageEnd() const& noexcept {
        return std::get_if<Http2MessageEndEvent>(&value_);
    }
    [[nodiscard]] const Http2MessageEndEvent* messageEnd() const&& = delete;

    [[nodiscard]] const Http2TunnelDataEvent* tunnelData() const& noexcept {
        return std::get_if<Http2TunnelDataEvent>(&value_);
    }
    [[nodiscard]] const Http2TunnelDataEvent* tunnelData() const&& = delete;

    [[nodiscard]] const Http2TunnelEndEvent* tunnelEnd() const& noexcept {
        return std::get_if<Http2TunnelEndEvent>(&value_);
    }
    [[nodiscard]] const Http2TunnelEndEvent* tunnelEnd() const&& = delete;

    [[nodiscard]] const Http2StreamClosedEvent* streamClosed() const& noexcept {
        return std::get_if<Http2StreamClosedEvent>(&value_);
    }
    [[nodiscard]] const Http2StreamClosedEvent* streamClosed() const&& = delete;

    [[nodiscard]] const Http2RequestUnprocessedEvent* requestUnprocessed() const& noexcept {
        return std::get_if<Http2RequestUnprocessedEvent>(&value_);
    }
    [[nodiscard]] const Http2RequestUnprocessedEvent* requestUnprocessed() const&& = delete;

    [[nodiscard]] const Http2GoawayEvent* goaway() const& noexcept {
        return std::get_if<Http2GoawayEvent>(&value_);
    }
    [[nodiscard]] const Http2GoawayEvent* goaway() const&& = delete;

private:
    friend class Http2Connection;

    using Value = std::variant<Http2InformationalHeadEvent, Http2MessageHeadEvent, Http2MessageBodyChunkEvent, Http2MessageEndEvent, Http2TunnelDataEvent, Http2TunnelEndEvent, Http2StreamClosedEvent, Http2RequestUnprocessedEvent, Http2GoawayEvent>;

    static_assert(static_cast<std::size_t>(Http2EventKind::kGoaway) + 1 == std::variant_size_v<Value>);

    template <typename Event>
    explicit Http2Event(Event event) noexcept
        : value_(std::move(event)) {}

    [[nodiscard]] static Http2Event informationalHead(std::uint32_t streamId, HttpClientResponseHead head, std::optional<HttpClientRequestContentSignal> requestContentSignal) noexcept {
        return Http2Event(Http2InformationalHeadEvent(streamId, std::move(head), requestContentSignal));
    }

    [[nodiscard]] static Http2Event messageHead(std::uint32_t streamId, std::optional<HttpClientRequestContentSignal> requestContentSignal = std::nullopt) noexcept {
        return Http2Event(Http2MessageHeadEvent(streamId, requestContentSignal));
    }

    [[nodiscard]] static Http2Event messageBodyChunk(std::uint32_t streamId, std::string_view bytes) noexcept {
        return Http2Event(Http2MessageBodyChunkEvent(streamId, bytes));
    }

    [[nodiscard]] static Http2Event messageEnd(std::uint32_t streamId) noexcept {
        return Http2Event(Http2MessageEndEvent(streamId));
    }

    [[nodiscard]] static Http2Event tunnelData(std::uint32_t streamId, std::string_view bytes) noexcept {
        return Http2Event(Http2TunnelDataEvent(streamId, bytes));
    }

    [[nodiscard]] static Http2Event tunnelEnd(std::uint32_t streamId) noexcept {
        return Http2Event(Http2TunnelEndEvent(streamId));
    }

    [[nodiscard]] static Http2Event streamClosed(std::uint32_t streamId, Http2StreamCloseSource source, Http2ErrorCode error) noexcept {
        return Http2Event(Http2StreamClosedEvent(streamId, source, error));
    }

    [[nodiscard]] static Http2Event requestUnprocessed(std::uint32_t streamId) noexcept {
        return Http2Event(Http2RequestUnprocessedEvent(streamId));
    }

    [[nodiscard]] static Http2Event goaway(Http2PeerGoaway peerGoaway) noexcept {
        return Http2Event(Http2GoawayEvent(peerGoaway));
    }

    Value value_;
};

}  // namespace ruvia::detail
