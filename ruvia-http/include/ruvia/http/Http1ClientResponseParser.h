#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpTransferCoding.h"
#include "ruvia/http/Http1ClientRequestWriter.h"

namespace ruvia {

namespace detail {

struct Http1ClientResponseParseResultAccess;
struct Http1ClientResponsePlanAccess;

// One request-content lifecycle for the response side of an HTTP/1 exchange.
// Expect and completion are not independent booleans: receiving Continue and
// completing content are ordered events, and the combined phase determines
// whether a later 100 is actionable and whether 101 is legal.
enum class Http1ClientRequestContentPhase : std::uint8_t {
    kContentComplete,
    kContentPending,
    kAwaitingContinue,
    kContinueReceived,
    kContentCompleteAwaitingContinue,
    kContinueReceivedContentComplete,
};

}  // namespace detail

// Connection lifecycle after a self-delimited response has been consumed.
// For an informational response, kReuse means the same exchange can await its
// final response; it does not make the connection poolable before that final.
// Close-delimited responses, tunnels, and upgrades are separate alternatives.

// Result of notifying the exchange that the external runtime finished writing
// every byte in the prepared request content plan. This event is required before
// a content-bearing request can accept 101 Switching Protocols.
enum class Http1ClientRequestContentCompletionStatus : std::uint8_t {
    kCompleted,
    kAlreadyComplete,
    kExchangeTerminal,
};

class Http1ClientInformationalResponse final {
public:
    [[nodiscard]] constexpr Http1ClosePolicy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    explicit constexpr Http1ClientInformationalResponse(Http1ClosePolicy persistence) noexcept
        : persistence_(persistence) {}

    Http1ClosePolicy persistence_;
};

class Http1ClientResponseWithoutContent final {
public:
    [[nodiscard]] constexpr Http1ClosePolicy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    explicit constexpr Http1ClientResponseWithoutContent(Http1ClosePolicy persistence) noexcept
        : persistence_(persistence) {}

    Http1ClosePolicy persistence_;
};

class Http1ClientKnownLengthResponse final {
public:
    [[nodiscard]] constexpr std::size_t contentLength() const noexcept {
        return contentLength_;
    }

    [[nodiscard]] constexpr bool requiresBodyConsumption() const noexcept {
        return contentLength_ != 0;
    }

    [[nodiscard]] constexpr Http1ClosePolicy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    constexpr Http1ClientKnownLengthResponse(
        std::size_t contentLength, Http1ClosePolicy persistence) noexcept
        : contentLength_(contentLength),
          persistence_(persistence) {}

    std::size_t contentLength_;
    Http1ClosePolicy persistence_;
};

class Http1ClientChunkedResponse final {
public:
    // Transfer codings preceding the terminal chunked framing. The runtime
    // removes chunk framing first and then drives this decoder list.
    [[nodiscard]] constexpr HttpTransferCodings transferCodings() const noexcept {
        return transferCodings_;
    }

    [[nodiscard]] constexpr Http1ClosePolicy persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    constexpr Http1ClientChunkedResponse(
        HttpTransferCodings transferCodings, Http1ClosePolicy persistence) noexcept
        : transferCodings_(transferCodings),
          persistence_(persistence) {}

    HttpTransferCodings transferCodings_;
    Http1ClosePolicy persistence_;
};

class Http1ClientCloseDelimitedResponse final {
public:
    // Any non-chunked transfer coding is decoded after EOF delimits the message.
    // This alternative always consumes through EOF and always closes; it exposes
    // no independent persistence field that could contradict those facts.
    [[nodiscard]] constexpr HttpTransferCodings transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    explicit constexpr Http1ClientCloseDelimitedResponse(
        HttpTransferCodings transferCodings) noexcept
        : transferCodings_(transferCodings) {}

    HttpTransferCodings transferCodings_;
};

// A 205 response has an ordinary HTTP/1 message-body framing phase, unlike
// HEAD/204/304, but RFC 9110 requires its decoded content to remain empty. The
// nested framing alternative tells the runtime how to reach the message end;
// the outer type prevents that framing from being mistaken for ordinary
// content that an application may consume.
class Http1ClientResponseWithZeroContent final {
public:
    [[nodiscard]] constexpr const Http1ClientKnownLengthResponse* knownLength() const& noexcept {
        return std::get_if<Http1ClientKnownLengthResponse>(&framing_);
    }
    const Http1ClientKnownLengthResponse* knownLength() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientChunkedResponse* chunked() const& noexcept {
        return std::get_if<Http1ClientChunkedResponse>(&framing_);
    }
    const Http1ClientChunkedResponse* chunked() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientCloseDelimitedResponse* closeDelimited()
        const& noexcept {
        return std::get_if<Http1ClientCloseDelimitedResponse>(&framing_);
    }
    const Http1ClientCloseDelimitedResponse* closeDelimited() const&& = delete;

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    using Framing = std::variant<Http1ClientKnownLengthResponse, Http1ClientChunkedResponse,
        Http1ClientCloseDelimitedResponse>;

    explicit constexpr Http1ClientResponseWithZeroContent(Framing framing) noexcept
        : framing_(framing) {}

    Framing framing_;
};

class Http1ClientConnectTunnel final {
private:
    friend struct detail::Http1ClientResponsePlanAccess;
    constexpr Http1ClientConnectTunnel() noexcept = default;
};

class Http1ClientProtocolUpgrade final {
private:
    friend struct detail::Http1ClientResponsePlanAccess;
    constexpr Http1ClientProtocolUpgrade() noexcept = default;
};

// Immutable RFC 9110/9112 response framing and lifecycle contract. The eight
// alternatives mirror message-length precedence and content semantics directly:
// informational, no-content final, zero-content-with-framing, exact-length,
// final-chunked, close-delimited, CONNECT tunnel, or protocol upgrade.
// Alternative-specific payload is only reachable from the alternative that owns
// it.
class Http1ClientResponsePlan final {
public:
    [[nodiscard]] constexpr const Http1ClientInformationalResponse* informational()
        const& noexcept {
        return std::get_if<Http1ClientInformationalResponse>(&state_);
    }
    const Http1ClientInformationalResponse* informational() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientResponseWithoutContent* withoutContent()
        const& noexcept {
        return std::get_if<Http1ClientResponseWithoutContent>(&state_);
    }
    const Http1ClientResponseWithoutContent* withoutContent() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientResponseWithZeroContent* zeroContent()
        const& noexcept {
        return std::get_if<Http1ClientResponseWithZeroContent>(&state_);
    }
    const Http1ClientResponseWithZeroContent* zeroContent() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientKnownLengthResponse* knownLength() const& noexcept {
        return std::get_if<Http1ClientKnownLengthResponse>(&state_);
    }
    const Http1ClientKnownLengthResponse* knownLength() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientChunkedResponse* chunked() const& noexcept {
        return std::get_if<Http1ClientChunkedResponse>(&state_);
    }
    const Http1ClientChunkedResponse* chunked() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientCloseDelimitedResponse* closeDelimited()
        const& noexcept {
        return std::get_if<Http1ClientCloseDelimitedResponse>(&state_);
    }
    const Http1ClientCloseDelimitedResponse* closeDelimited() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientConnectTunnel* connectTunnel() const& noexcept {
        return std::get_if<Http1ClientConnectTunnel>(&state_);
    }
    const Http1ClientConnectTunnel* connectTunnel() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientProtocolUpgrade* protocolUpgrade() const& noexcept {
        return std::get_if<Http1ClientProtocolUpgrade>(&state_);
    }
    const Http1ClientProtocolUpgrade* protocolUpgrade() const&& = delete;

    [[nodiscard]] constexpr std::optional<HttpClientRequestContentSignal> requestContentSignal()
        const noexcept {
        return requestContentSignal_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    using State = std::variant<Http1ClientInformationalResponse, Http1ClientResponseWithoutContent,
        Http1ClientResponseWithZeroContent, Http1ClientKnownLengthResponse,
        Http1ClientChunkedResponse, Http1ClientCloseDelimitedResponse, Http1ClientConnectTunnel,
        Http1ClientProtocolUpgrade>;

    Http1ClientResponsePlan(
        State state, std::optional<HttpClientRequestContentSignal> requestContentSignal) noexcept
        : state_(state),
          requestContentSignal_(requestContentSignal) {}

    State state_;
    std::optional<HttpClientRequestContentSignal> requestContentSignal_;
};

// Protocol failures are typed and allocation-free. Resource exhaustion can
// still throw while materializing a successful owning response. Exchange
// termination is NOT a parse error: parsing after the exchange completed or
// failed is reported through Http1ClientResponseParseTerminal instead.
enum class Http1ClientResponseParseError : std::uint8_t {
    kHeaderTooLarge,
    kInvalidStatusLine,
    kUnsupportedHttpVersion,
    kInvalidStatusCode,
    kInvalidReasonPhrase,
    kInvalidHeader,
    kInvalidConnection,
    kInvalidUpgrade,
    kTooManyHeaders,
    kInvalidContentLength,
    kConflictingContentLength,
    kInvalidTransferEncoding,
    kUnsupportedTransferEncoding,
    kTransferEncodingInHttp10,
    kContentLengthAndTransferEncoding,
    kInvalidProtocolSwitch,
    kTooManyInformationalResponses,
};

[[nodiscard]] std::string_view http1ClientResponseParseErrorMessage(
    Http1ClientResponseParseError error) noexcept;

class Http1ClientResponseNeedMore final {
private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    explicit constexpr Http1ClientResponseNeedMore() noexcept = default;
};

// One complete response head. The response owns status/header storage through
// PMR; consumedBytes() is the exact boundary after CRLF CRLF, so a sans-I/O
// driver can feed the remaining bytes to the body/tunnel/upgrade path directly.
class Http1ParsedClientResponseHead final {
public:
    Http1ParsedClientResponseHead(const Http1ParsedClientResponseHead&) = delete;
    Http1ParsedClientResponseHead& operator=(const Http1ParsedClientResponseHead&) = delete;
    Http1ParsedClientResponseHead(Http1ParsedClientResponseHead&&) noexcept = default;
    Http1ParsedClientResponseHead& operator=(Http1ParsedClientResponseHead&&) = delete;

    [[nodiscard]] const HttpClientResponseHead& head() const& noexcept {
        return head_;
    }
    [[nodiscard]] const HttpClientResponseHead& head() const&& = delete;

    [[nodiscard]] HttpClientResponseHead takeHead() && noexcept {
        return std::move(head_);
    }

    [[nodiscard]] const Http1ClientResponsePlan& plan() const& noexcept {
        return plan_;
    }
    [[nodiscard]] const Http1ClientResponsePlan& plan() const&& = delete;

    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    Http1ParsedClientResponseHead(HttpClientResponseHead head, Http1ClientResponsePlan plan,
        std::size_t consumedBytes) noexcept
        : head_(std::move(head)),
          plan_(plan),
          consumedBytes_(consumedBytes) {}

    HttpClientResponseHead head_;
    Http1ClientResponsePlan plan_;
    std::size_t consumedBytes_{0};
};

class Http1ClientResponseParseFailure final {
public:
    [[nodiscard]] constexpr Http1ClientResponseParseError error() const noexcept {
        return error_;
    }

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    explicit constexpr Http1ClientResponseParseFailure(Http1ClientResponseParseError error) noexcept
        : error_(error) {}

    Http1ClientResponseParseError error_;
};

// Parsing after the exchange already completed or failed is not a wire error;
// it is a state-machine termination report. completed() names the outcome that
// ended the exchange.
class Http1ClientResponseParseTerminal final {
public:
    [[nodiscard]] constexpr bool completed() const noexcept {
        return completed_;
    }

    [[nodiscard]] constexpr bool failed() const noexcept {
        return !completed_;
    }

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    explicit constexpr Http1ClientResponseParseTerminal(bool completed) noexcept
        : completed_(completed) {}

    bool completed_;
};

class Http1ClientResponseParseResult final {
public:
    Http1ClientResponseParseResult(const Http1ClientResponseParseResult&) = delete;
    Http1ClientResponseParseResult& operator=(const Http1ClientResponseParseResult&) = delete;
    Http1ClientResponseParseResult(Http1ClientResponseParseResult&&) noexcept = default;
    Http1ClientResponseParseResult& operator=(Http1ClientResponseParseResult&&) = delete;

    [[nodiscard]] const Http1ClientResponseNeedMore* needMore() const& noexcept {
        return std::get_if<Http1ClientResponseNeedMore>(&state_);
    }
    const Http1ClientResponseNeedMore* needMore() const&& = delete;

    [[nodiscard]] Http1ParsedClientResponseHead* parsed() & noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }

    [[nodiscard]] const Http1ParsedClientResponseHead* parsed() const& noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }
    Http1ParsedClientResponseHead* parsed() && = delete;
    const Http1ParsedClientResponseHead* parsed() const&& = delete;

    [[nodiscard]] const Http1ClientResponseParseFailure* failure() const& noexcept {
        return std::get_if<Http1ClientResponseParseFailure>(&state_);
    }
    const Http1ClientResponseParseFailure* failure() const&& = delete;

    // Non-null when the exchange already completed or failed and parsing
    // stopped; a driver must not feed more input to this parser.
    [[nodiscard]] const Http1ClientResponseParseTerminal* terminal() const& noexcept {
        return std::get_if<Http1ClientResponseParseTerminal>(&state_);
    }
    const Http1ClientResponseParseTerminal* terminal() const&& = delete;

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    explicit Http1ClientResponseParseResult(Http1ClientResponseNeedMore state) noexcept
        : state_(state) {}

    explicit Http1ClientResponseParseResult(Http1ParsedClientResponseHead state) noexcept
        : state_(std::move(state)) {}

    explicit Http1ClientResponseParseResult(Http1ClientResponseParseFailure state) noexcept
        : state_(state) {}

    explicit Http1ClientResponseParseResult(Http1ClientResponseParseTerminal state) noexcept
        : state_(state) {}

    std::variant<Http1ClientResponseNeedMore, Http1ParsedClientResponseHead,
        Http1ClientResponseParseFailure, Http1ClientResponseParseTerminal>
        state_;
};

// Per-request HTTP/1 response-head state machine. Construction consumes the
// owning exchange state from one successfully prepared request; informational
// responses advance the same
// exchange until a final response, CONNECT tunnel, or protocol switch completes
// it. Header validation is transactional and owning response allocation occurs
// only after protocol validation succeeds.
class Http1ClientResponseParser final {
public:
    struct Options final {
        std::pmr::memory_resource* resource{nullptr};
    };

    explicit Http1ClientResponseParser(Http1ClientExchangeState exchangeState) noexcept;
    explicit Http1ClientResponseParser(
        Http1ClientExchangeState exchangeState, Options options) noexcept;

    Http1ClientResponseParser(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser& operator=(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser(Http1ClientResponseParser&&) = delete;
    Http1ClientResponseParser& operator=(Http1ClientResponseParser&&) = delete;

    [[nodiscard]] Http1ClientRequestContentCompletionStatus completeRequestContent() noexcept;

    [[nodiscard]] Http1ClientResponseParseResult parse(std::string_view buffer);

private:
    [[nodiscard]] static constexpr detail::Http1ClientRequestContentPhase
    initialRequestContentPhase(const Http1ClientExchangeState& state) noexcept {
        switch (detail::Http1ClientExchangeStateAccess::contentState(state)) {
            case detail::Http1ClientInitialContentState::kComplete:
                return detail::Http1ClientRequestContentPhase::kContentComplete;
            case detail::Http1ClientInitialContentState::kPending:
                return detail::Http1ClientRequestContentPhase::kContentPending;
            case detail::Http1ClientInitialContentState::kAwaitingContinue:
                return detail::Http1ClientRequestContentPhase::kAwaitingContinue;
        }
        return detail::Http1ClientRequestContentPhase::kContentComplete;
    }

    enum class Phase : std::uint8_t {
        kAwaitResponse,
        kComplete,
        kFailed,
    };

    Http1ClientExchangeState exchangeState_;
    std::pmr::memory_resource* resource_;
    Phase phase_{Phase::kAwaitResponse};
    detail::Http1ClientRequestContentPhase requestContentPhase_;
    std::uint8_t informationalResponseCount_{0};
};

inline Http1ClientResponseParser::Http1ClientResponseParser(
    Http1ClientExchangeState exchangeState) noexcept
    : Http1ClientResponseParser(std::move(exchangeState), Options{}) {}

inline Http1ClientResponseParser::Http1ClientResponseParser(
    Http1ClientExchangeState exchangeState, Options options) noexcept
    : exchangeState_(std::move(exchangeState)),
      resource_(options.resource),
      requestContentPhase_(initialRequestContentPhase(exchangeState_)) {}

}  // namespace ruvia
