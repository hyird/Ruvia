#pragma once

// HTTP/1.1 sans-I/O server connection core.
//
// A pure request-side state machine over the (already sans-I/O) h1 parser, shaped
// like Http2Connection: you feed it inbound bytes and it emits events -- a decoded
// request head, de-chunked/measured body slices, message end -- one message at a
// time, honoring keep-alive pipelining. It never touches a socket, a timer, or
// asio, so any runtime (the web session, edge, an external embedder) can drive it.
//
// Contract (same discipline as Http2Connection):
//   - Event `bytes` views point into the connection's input buffer; drain all events
//     after each feed() / nextMessage() before calling either again.
//   - The head() views are valid from kMessageHead until nextMessage().
//   - After kMessageEnd the core STOPS consuming (pipelined bytes stay buffered and
//     are visible via unconsumedInput()) until the owner -- having written its
//     response -- calls nextMessage(), which reclaims the finished message and
//     immediately continues parsing any pipelined input.
//   - Response writing is the owner's job: an h1 response is plain bytes, and
//     everything response-side (serialization, compression, framing) already has
//     pure helpers; this core deliberately has no output buffer.
//   - Owner policy stays outside: 100-continue (head().flags), upgrades
//     (h2c/WebSocket -- inspect the head + unconsumedInput and hand off), and
//     cleartext h2-preface sniffing happen on the owner's side.
//
// A parse or framing violation puts the core in an error state (feed returns
// kError); error() then names the HttpParseError for the owner's error response.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "HttpParserInternal.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpParseTypes.h"

namespace ruvia::detail {

// Protocol-only configuration (NOT server policy; timeouts/limits policy is the
// owner's). maxBodyBytes bounds the DECODED body (0 = unlimited).
struct Http1CoreConfig final {
    std::size_t maxHeaderBytes{kMaxHttpHeaderBytes};
    std::size_t maxBodyBytes{0};
};

enum class Http1FeedStatus : std::uint8_t {
    kOk,        // consumed bytes; events may be pending
    kNeedMore,  // buffered a partial head/body; feed more bytes
    kError,     // protocol error; see error() -- the connection is unusable
};

struct Http1FeedResult final {
    std::size_t consumed{0};
    Http1FeedStatus status{Http1FeedStatus::kNeedMore};
};

struct Http1Event final {
    enum class Kind : std::uint8_t {
        kNone,
        kMessageHead,       // head() carries the decoded request
        kMessageBodyChunk,  // de-framed body bytes (view valid until next feed)
        kMessageEnd,        // message complete; core paused until nextMessage()
    };
    Kind kind{Kind::kNone};
    std::string_view bytes{};  // for kMessageBodyChunk
};

class Http1Connection final {
public:
    explicit Http1Connection(std::pmr::memory_resource* resource, Http1CoreConfig config = {});

    [[nodiscard]] Http1FeedResult feed(std::string_view in);
    [[nodiscard]] Http1Event nextEvent();

    // The current message's decoded head (valid from kMessageHead until nextMessage).
    [[nodiscard]] const HttpServerParseResult& head() const noexcept { return parsed_; }
    // Valid after a kError feed result.
    [[nodiscard]] HttpParseError error() const noexcept { return error_; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::kError; }
    // True between kMessageEnd and nextMessage().
    [[nodiscard]] bool messageComplete() const noexcept { return state_ == State::kMessageDone; }
    // RFC 9112 connection persistence of the current head (Connection header + version).
    [[nodiscard]] bool keepAlive() const noexcept;
    // Buffered bytes beyond what the current message consumed: the pipelined next
    // request, or an upgraded protocol's initial bytes for hand-off. Valid until the
    // next feed()/nextMessage().
    [[nodiscard]] std::string_view unconsumedInput() const noexcept;

    // The owner finished responding to the current message: reclaim its bytes and
    // continue with pipelined input (the next head's events may be emitted -- drain).
    void nextMessage();

private:
    enum class State : std::uint8_t {
        kHead,
        kBodyContentLength,
        kChunkSize,
        kChunkData,
        kChunkDataCrlf,
        kChunkTrailers,
        kMessageDone,
        kError,
    };

    void advance();
    void fail(HttpParseError error);
    void finishMessage();
    [[nodiscard]] bool accountBody(std::size_t bytes);

    Http1CoreConfig config_;
    HttpServerParser parser_;
    HttpServerParseResult parsed_;
    std::pmr::string input_;
    std::pmr::vector<Http1Event> events_;
    std::size_t eventOffset_{0};
    std::size_t headerSearchOffset_{0};
    std::size_t cursor_{0};         // consumed-framing cursor within input_
    std::size_t bodyRemaining_{0};  // content-length remainder / current chunk remainder
    std::size_t decodedBodyBytes_{0};
    HttpParseError error_{HttpParseError::kNone};
    State state_{State::kHead};
};

}  // namespace ruvia::detail
