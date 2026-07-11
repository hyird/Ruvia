#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
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

enum class Http1ClientResponseBodyMode : std::uint8_t {
    kNone,
    kContentLength,
    kChunked,
    kCloseDelimited,
    // Bytes after the head belong to a CONNECT tunnel or an upgraded protocol,
    // not to an HTTP response body.
    kOpaque
};

enum class Http1ClientConnectionDisposition : std::uint8_t {
    // An informational response completed, but the request still awaits its final
    // response; the connection must not be released to another request.
    kAwaitFinalResponse,
    kReuse,
    kClose,
    kConnectTunnel,
    kUpgrade
};

// Signal for a request body gated by Expect: 100-continue. It is deliberately
// separate from wait duration: the protocol core reports Continue or
// exchange-complete progress, while an external I/O runtime owns its finite
// timeout policy.
enum class Http1ClientRequestContentSignal : std::uint8_t {
    kNone,
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

// Immutable RFC 9110/9112 response framing and connection contract. External
// I/O runtimes drive exactly this plan instead of reconstructing body length,
// persistence, CONNECT, or Upgrade transitions from independent parser facts.
class Http1ClientResponsePlan final {
public:
    [[nodiscard]] Http1ClientResponseBodyMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] bool hasContentLength() const noexcept {
        return mode_ == Http1ClientResponseBodyMode::kContentLength;
    }

    [[nodiscard]] std::size_t contentLength() const noexcept {
        return contentLength_;
    }

    [[nodiscard]] bool isChunked() const noexcept {
        return mode_ == Http1ClientResponseBodyMode::kChunked;
    }

    [[nodiscard]] bool isCloseDelimited() const noexcept {
        return mode_ == Http1ClientResponseBodyMode::kCloseDelimited;
    }

    [[nodiscard]] bool isOpaque() const noexcept {
        return mode_ == Http1ClientResponseBodyMode::kOpaque;
    }

    [[nodiscard]] bool isConnectTunnel() const noexcept {
        return connectionDisposition_ ==
            Http1ClientConnectionDisposition::kConnectTunnel;
    }

    [[nodiscard]] bool isUpgrade() const noexcept {
        return connectionDisposition_ ==
            Http1ClientConnectionDisposition::kUpgrade;
    }

    // Chunked requires its terminal zero chunk; close-delimited requires EOF.
    [[nodiscard]] bool requiresBodyConsumption() const noexcept {
        switch (mode_) {
            case Http1ClientResponseBodyMode::kContentLength:
                return contentLength_ != 0;
            case Http1ClientResponseBodyMode::kChunked:
            case Http1ClientResponseBodyMode::kCloseDelimited:
                return true;
            case Http1ClientResponseBodyMode::kNone:
            case Http1ClientResponseBodyMode::kOpaque:
                return false;
        }
        return false;
    }

    [[nodiscard]] bool selfDelimited() const noexcept {
        return mode_ == Http1ClientResponseBodyMode::kNone ||
            mode_ == Http1ClientResponseBodyMode::kContentLength ||
            mode_ == Http1ClientResponseBodyMode::kChunked;
    }

    // Transfer codings preceding final chunked, or the sole supported coding on
    // a close-delimited response. The runtime removes message framing first and
    // then drives this decoder list.
    [[nodiscard]] const detail::HttpTransferCodings& transferCodings() const noexcept {
        return transferCodings_;
    }

    [[nodiscard]] Http1ClientConnectionDisposition connectionDisposition() const noexcept {
        return connectionDisposition_;
    }

    [[nodiscard]] Http1ClientRequestContentSignal requestContentSignal() const noexcept {
        return requestContentSignal_;
    }

private:
    friend struct detail::Http1ClientResponsePlanAccess;

    Http1ClientResponsePlan(
        Http1ClientResponseBodyMode mode,
        std::size_t contentLength,
        detail::HttpTransferCodings transferCodings,
        Http1ClientConnectionDisposition connectionDisposition,
        Http1ClientRequestContentSignal requestContentSignal) noexcept
        : mode_(mode),
          contentLength_(contentLength),
          transferCodings_(transferCodings),
          connectionDisposition_(connectionDisposition),
          requestContentSignal_(requestContentSignal) {}

    Http1ClientResponseBodyMode mode_{Http1ClientResponseBodyMode::kNone};
    std::size_t contentLength_{0};
    detail::HttpTransferCodings transferCodings_;
    Http1ClientConnectionDisposition connectionDisposition_{
        Http1ClientConnectionDisposition::kClose};
    Http1ClientRequestContentSignal requestContentSignal_{
        Http1ClientRequestContentSignal::kNone};
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
    Http1ParsedClientResponseHead& operator=(Http1ParsedClientResponseHead&&) noexcept = default;

    [[nodiscard]] const HttpClientResponse& response() const noexcept {
        return response_;
    }

    [[nodiscard]] HttpClientResponse takeResponse() && noexcept {
        return std::move(response_);
    }

    [[nodiscard]] const Http1ClientResponsePlan& plan() const noexcept {
        return plan_;
    }

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
          plan_(plan),
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
    Http1ClientResponseParseResult& operator=(Http1ClientResponseParseResult&&) noexcept = default;

    [[nodiscard]] Http1ClientResponseParseKind kind() const noexcept {
        if (std::holds_alternative<Http1ParsedClientResponseHead>(state_)) {
            return Http1ClientResponseParseKind::kParsed;
        }
        return std::holds_alternative<Http1ClientResponseParseFailure>(state_)
            ? Http1ClientResponseParseKind::kFailure
            : Http1ClientResponseParseKind::kNeedMore;
    }

    [[nodiscard]] const Http1ClientResponseNeedMore* needMore() const noexcept {
        return std::get_if<Http1ClientResponseNeedMore>(&state_);
    }

    [[nodiscard]] Http1ParsedClientResponseHead* parsed() noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }

    [[nodiscard]] const Http1ParsedClientResponseHead* parsed() const noexcept {
        return std::get_if<Http1ParsedClientResponseHead>(&state_);
    }

    [[nodiscard]] const Http1ClientResponseParseFailure* failure() const noexcept {
        return std::get_if<Http1ClientResponseParseFailure>(&state_);
    }

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
          requestContentComplete_(request.contentPlan_.bytes().empty()) {}

    Http1ClientResponseParser(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser& operator=(const Http1ClientResponseParser&) = delete;
    Http1ClientResponseParser(Http1ClientResponseParser&&) = delete;
    Http1ClientResponseParser& operator=(Http1ClientResponseParser&&) = delete;

    [[nodiscard]] Http1ClientRequestContentCompletionStatus
    completeRequestContent() noexcept;

    [[nodiscard]] Http1ClientResponseParseResult parse(
        std::string_view buffer);

private:
    enum class Phase : std::uint8_t {
        kAwaitResponse,
        kComplete,
        kFailed,
    };

    detail::Http1ClientRequestContext request_;
    std::pmr::memory_resource* resource_;
    Phase phase_{Phase::kAwaitResponse};
    bool sawContinue_{false};
    bool requestContentComplete_{false};
};

}  // namespace ruvia
