#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/HttpTransferCoding.h"
#include "ruvia/http/Http1ClientRequestWriter.h"

namespace ruvia {

namespace detail {

struct Http1ClientResponseParseResultAccess;
struct Http1ClientResponsePlanAccess;

}  // namespace detail

// Connection lifecycle after a self-delimited final response has been consumed.
// Informational responses, close-delimited responses, tunnels, and upgrades are
// separate alternatives and therefore cannot be mistaken for reusable messages.
enum class Http1ClientResponsePersistence : std::uint8_t {
    kReuse,
    kClose
};

// Signal for a request body gated by Expect: 100-continue. It is deliberately
// separate from wait duration: the protocol core reports Continue or
// exchange-complete progress, while an external I/O runtime owns its finite
// timeout policy. Most response heads emit no request-content event, represented
// by an empty optional rather than a non-event enum member.
enum class Http1ClientRequestContentSignal : std::uint8_t {
    kContinue,
    kExchangeComplete,
};

// Result of notifying the exchange that the external runtime finished writing
// every byte in the prepared request content plan. This event is required before
// a content-bearing request can accept 101 Switching Protocols.
enum class Http1ClientRequestContentCompletionStatus : std::uint8_t {
    kCompleted,
    kAlreadyComplete,
    kExchangeTerminal,
};

class Http1ClientInformationalResponse final {
private:
    friend struct detail::Http1ClientResponsePlanAccess;
    constexpr Http1ClientInformationalResponse() noexcept = default;
};

class Http1ClientResponseWithoutContent final {
public:
    [[nodiscard]] constexpr Http1ClientResponsePersistence persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    explicit constexpr Http1ClientResponseWithoutContent(
        Http1ClientResponsePersistence persistence) noexcept
        : persistence_(persistence) {}

    Http1ClientResponsePersistence persistence_;
};

class Http1ClientKnownLengthResponse final {
public:
    [[nodiscard]] constexpr std::size_t contentLength() const noexcept {
        return contentLength_;
    }

    [[nodiscard]] constexpr bool requiresBodyConsumption() const noexcept {
        return contentLength_ != 0;
    }

    [[nodiscard]] constexpr Http1ClientResponsePersistence persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    constexpr Http1ClientKnownLengthResponse(
        std::size_t contentLength,
        Http1ClientResponsePersistence persistence) noexcept
        : contentLength_(contentLength), persistence_(persistence) {}

    std::size_t contentLength_;
    Http1ClientResponsePersistence persistence_;
};

class Http1ClientChunkedResponse final {
public:
    // Transfer codings preceding the terminal chunked framing. The runtime
    // removes chunk framing first and then drives this decoder list.
    [[nodiscard]] constexpr const detail::HttpTransferCodings&
    transferCodings() const & noexcept {
        return transferCodings_;
    }
    [[nodiscard]] constexpr const detail::HttpTransferCodings&
    transferCodings() const && = delete;

    [[nodiscard]] constexpr Http1ClientResponsePersistence persistence() const noexcept {
        return persistence_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    constexpr Http1ClientChunkedResponse(
        detail::HttpTransferCodings transferCodings,
        Http1ClientResponsePersistence persistence) noexcept
        : transferCodings_(transferCodings), persistence_(persistence) {}

    detail::HttpTransferCodings transferCodings_;
    Http1ClientResponsePersistence persistence_;
};

class Http1ClientCloseDelimitedResponse final {
public:
    // Any non-chunked transfer coding is decoded after EOF delimits the message.
    // This alternative always consumes through EOF and always closes; it exposes
    // no independent persistence field that could contradict those facts.
    [[nodiscard]] constexpr const detail::HttpTransferCodings&
    transferCodings() const & noexcept {
        return transferCodings_;
    }
    [[nodiscard]] constexpr const detail::HttpTransferCodings&
    transferCodings() const && = delete;

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    explicit constexpr Http1ClientCloseDelimitedResponse(
        detail::HttpTransferCodings transferCodings) noexcept
        : transferCodings_(transferCodings) {}

    detail::HttpTransferCodings transferCodings_;
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

// Immutable RFC 9110/9112 response framing and lifecycle contract. The seven
// alternatives mirror the message-length precedence directly: informational,
// no-content final, exact-length final, final-chunked, close-delimited, CONNECT
// tunnel, or protocol upgrade. Alternative-specific payload is only reachable
// from the alternative that owns it.
class Http1ClientResponsePlan final {
public:
    [[nodiscard]] constexpr const Http1ClientInformationalResponse*
    informational() const & noexcept {
        return std::get_if<Http1ClientInformationalResponse>(&state_);
    }
    const Http1ClientInformationalResponse* informational() const && = delete;

    [[nodiscard]] constexpr const Http1ClientResponseWithoutContent*
    withoutContent() const & noexcept {
        return std::get_if<Http1ClientResponseWithoutContent>(&state_);
    }
    const Http1ClientResponseWithoutContent* withoutContent() const && = delete;

    [[nodiscard]] constexpr const Http1ClientKnownLengthResponse*
    knownLength() const & noexcept {
        return std::get_if<Http1ClientKnownLengthResponse>(&state_);
    }
    const Http1ClientKnownLengthResponse* knownLength() const && = delete;

    [[nodiscard]] constexpr const Http1ClientChunkedResponse*
    chunked() const & noexcept {
        return std::get_if<Http1ClientChunkedResponse>(&state_);
    }
    const Http1ClientChunkedResponse* chunked() const && = delete;

    [[nodiscard]] constexpr const Http1ClientCloseDelimitedResponse*
    closeDelimited() const & noexcept {
        return std::get_if<Http1ClientCloseDelimitedResponse>(&state_);
    }
    const Http1ClientCloseDelimitedResponse*
    closeDelimited() const && = delete;

    [[nodiscard]] constexpr const Http1ClientConnectTunnel*
    connectTunnel() const & noexcept {
        return std::get_if<Http1ClientConnectTunnel>(&state_);
    }
    const Http1ClientConnectTunnel* connectTunnel() const && = delete;

    [[nodiscard]] constexpr const Http1ClientProtocolUpgrade*
    protocolUpgrade() const & noexcept {
        return std::get_if<Http1ClientProtocolUpgrade>(&state_);
    }
    const Http1ClientProtocolUpgrade* protocolUpgrade() const && = delete;

    [[nodiscard]] constexpr std::optional<Http1ClientRequestContentSignal>
    requestContentSignal() const noexcept {
        return requestContentSignal_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    using State = std::variant<
        Http1ClientInformationalResponse,
        Http1ClientResponseWithoutContent,
        Http1ClientKnownLengthResponse,
        Http1ClientChunkedResponse,
        Http1ClientCloseDelimitedResponse,
        Http1ClientConnectTunnel,
        Http1ClientProtocolUpgrade>;

    Http1ClientResponsePlan(
        State state,
        std::optional<Http1ClientRequestContentSignal>
            requestContentSignal) noexcept
        : state_(std::move(state)),
          requestContentSignal_(requestContentSignal) {}

    State state_;
    std::optional<Http1ClientRequestContentSignal> requestContentSignal_;
};

// Protocol failures are typed and allocation-free. Resource exhaustion can
// still throw while materializing a successful owning response.
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
    kExchangeComplete,
    kExchangeFailed,
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

    [[nodiscard]] const HttpClientResponse& response() const & noexcept {
        return response_;
    }
    [[nodiscard]] const HttpClientResponse& response() const && = delete;

    [[nodiscard]] HttpClientResponse takeResponse() && noexcept {
        return std::move(response_);
    }

    [[nodiscard]] const Http1ClientResponsePlan& plan() const & noexcept {
        return plan_;
    }
    [[nodiscard]] const Http1ClientResponsePlan& plan() const && = delete;

    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    Http1ParsedClientResponseHead(
        HttpClientResponse response,
        Http1ClientResponsePlan plan,
        std::size_t consumedBytes) noexcept
        : response_(std::move(response)),
          plan_(std::move(plan)),
          consumedBytes_(consumedBytes) {}

    HttpClientResponse response_;
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

    explicit constexpr Http1ClientResponseParseFailure(
        Http1ClientResponseParseError error) noexcept
        : error_(error) {}

    Http1ClientResponseParseError error_;
};

enum class Http1ClientResponseParseKind : std::uint8_t {
    kNeedMore,
    kParsed,
    kFailure
};

class Http1ClientResponseParseResult final {
public:
    Http1ClientResponseParseResult(const Http1ClientResponseParseResult&) = delete;
    Http1ClientResponseParseResult& operator=(const Http1ClientResponseParseResult&) = delete;
    Http1ClientResponseParseResult(Http1ClientResponseParseResult&&) noexcept = default;
    Http1ClientResponseParseResult& operator=(Http1ClientResponseParseResult&&) = delete;

    [[nodiscard]] Http1ClientResponseParseKind kind() const noexcept {
        if (std::holds_alternative<Http1ParsedClientResponseHead>(state_)) {
            return Http1ClientResponseParseKind::kParsed;
        }
        return std::holds_alternative<Http1ClientResponseParseFailure>(state_)
            ? Http1ClientResponseParseKind::kFailure
            : Http1ClientResponseParseKind::kNeedMore;
    }

    [[nodiscard]] const Http1ClientResponseNeedMore* needMore() const & noexcept {
        return std::get_if<Http1ClientResponseNeedMore>(&state_);
    }
    const Http1ClientResponseNeedMore* needMore() const && = delete;

    [[nodiscard]] Http1ParsedClientResponseHead* parsed() & noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }

    [[nodiscard]] const Http1ParsedClientResponseHead* parsed() const & noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }
    Http1ParsedClientResponseHead* parsed() && = delete;
    const Http1ParsedClientResponseHead* parsed() const && = delete;

    [[nodiscard]] const Http1ClientResponseParseFailure* failure() const & noexcept {
        return std::get_if<Http1ClientResponseParseFailure>(&state_);
    }
    const Http1ClientResponseParseFailure* failure() const && = delete;

private:
    friend struct detail::Http1ClientResponseParseResultAccess;

    explicit Http1ClientResponseParseResult(
        Http1ClientResponseNeedMore state) noexcept
        : state_(std::move(state)) {}

    explicit Http1ClientResponseParseResult(
        Http1ParsedClientResponseHead state) noexcept
        : state_(std::move(state)) {}

    explicit Http1ClientResponseParseResult(
        Http1ClientResponseParseFailure state) noexcept
        : state_(std::move(state)) {}

    std::variant<
        Http1ClientResponseNeedMore,
        Http1ParsedClientResponseHead,
        Http1ClientResponseParseFailure> state_;
};

// Per-request HTTP/1 response-head state machine. Construction is bound to one
// successfully prepared request; informational responses advance the same
// exchange until a final response, CONNECT tunnel, or protocol switch completes
// it. Header validation is transactional and owning response allocation occurs
// only after protocol validation succeeds.
class Http1ClientResponseParser final {
public:
    explicit Http1ClientResponseParser(
        const PreparedHttp1ClientRequest& request,
        std::pmr::memory_resource* resource = nullptr) noexcept
        : request_(request.responseContext_),
          resource_(resource),
          continueGated_(request.contentPlan_.continueGated() != nullptr),
          requestContentComplete_(
              requestContentStartsComplete(request.contentPlan_)) {}

    Http1ClientResponseParser(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser& operator=(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser(Http1ClientResponseParser&&) = delete;
    Http1ClientResponseParser& operator=(Http1ClientResponseParser&&) = delete;

    [[nodiscard]] Http1ClientRequestContentCompletionStatus
    completeRequestContent() noexcept;

    [[nodiscard]] Http1ClientResponseParseResult parse(
        std::string_view buffer);

private:
    [[nodiscard]] static constexpr bool requestContentStartsComplete(
        const Http1ClientRequestContentPlan& plan) noexcept {
        if (plan.withoutContent() != nullptr) {
            return true;
        }
        if (const auto* immediate = plan.immediate()) {
            return immediate->bytes().empty();
        }
        return false;
    }

    enum class Phase : std::uint8_t {
        kAwaitResponse,
        kComplete,
        kFailed,
    };

    detail::Http1ClientRequestContext request_;
    std::pmr::memory_resource* resource_;
    Phase phase_{Phase::kAwaitResponse};
    bool continueGated_{false};
    bool sawContinue_{false};
    bool requestContentComplete_{false};
};

}  // namespace ruvia
