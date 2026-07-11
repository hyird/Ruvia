#pragma once

#include <cstdint>
#include <variant>

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace ruvia::detail {

class Http1ResponseHeadPlan;
class Http1BufferedResponsePlan;

// A complete buffered representation is length-delimited by the response writer.
class Http1BufferedResponseHead final {
public:
    [[nodiscard]] constexpr std::uint64_t contentLength() const noexcept {
        return contentLength_;
    }

private:
    friend class Http1ResponseHeadPlan;

    explicit constexpr Http1BufferedResponseHead(
        std::uint64_t contentLength) noexcept
        : contentLength_(contentLength) {}

    std::uint64_t contentLength_{0};
};

// The response writer owns canonical Transfer-Encoding: chunked and the runtime
// emits the matching chunk frames. Application headers cannot redefine framing.
class Http1ChunkedResponseStreamHead final {
private:
    friend class Http1ResponseHeadPlan;

    constexpr Http1ChunkedResponseStreamHead() noexcept = default;
};

// The response has no declared message-body length. When content is allowed, the
// runtime closes the connection to delimit it; application Content-Length and
// Transfer-Encoding fields therefore cannot survive into the wire head.
class Http1CloseDelimitedResponseStreamHead final {
private:
    friend class Http1ResponseHeadPlan;

    constexpr Http1CloseDelimitedResponseStreamHead() noexcept = default;
};

// Final HTTP/1 head framing is an exclusive protocol value, not the old
// "suppress automatic Content-Length" boolean. The body plan keeps method/status
// content semantics attached to the exact wire-framing alternative selected by
// the HTTP/1 planner.
class Http1ResponseHeadPlan final {
public:
    [[nodiscard]] constexpr const Http1BufferedResponseHead*
    buffered() const noexcept {
        return std::get_if<Http1BufferedResponseHead>(&framing_);
    }

    [[nodiscard]] constexpr const Http1ChunkedResponseStreamHead*
    chunkedStream() const noexcept {
        return std::get_if<Http1ChunkedResponseStreamHead>(&framing_);
    }

    [[nodiscard]] constexpr const Http1CloseDelimitedResponseStreamHead*
    closeDelimitedStream() const noexcept {
        return std::get_if<Http1CloseDelimitedResponseStreamHead>(&framing_);
    }

    [[nodiscard]] constexpr const HttpResponseBodyPlan& bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] constexpr HttpProtocolVersion
    protocolVersion() const noexcept {
        return protocolVersion_;
    }

private:
    friend Http1BufferedResponsePlan http1BufferedResponsePlan(
        HttpBufferedResponseWritePlan,
        Http1ServerConnectionPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
        HttpResponseBodyPlan,
        Http1ServerConnectionPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1CloseDelimitedResponseStreamHeadPlan(
        HttpResponseBodyPlan,
        Http1ServerConnectionPlan) noexcept;

    using Framing = std::variant<
        Http1BufferedResponseHead,
        Http1ChunkedResponseStreamHead,
        Http1CloseDelimitedResponseStreamHead>;

    [[nodiscard]] static constexpr Framing bufferedFraming(
        std::uint64_t contentLength) noexcept {
        return Framing(Http1BufferedResponseHead(contentLength));
    }

    [[nodiscard]] static constexpr Framing chunkedStreamFraming() noexcept {
        return Framing(Http1ChunkedResponseStreamHead());
    }

    [[nodiscard]] static constexpr Framing closeDelimitedStreamFraming() noexcept {
        return Framing(Http1CloseDelimitedResponseStreamHead());
    }

    constexpr Http1ResponseHeadPlan(
        HttpResponseBodyPlan bodyPlan,
        HttpProtocolVersion protocolVersion,
        Framing framing) noexcept
        : bodyPlan_(bodyPlan),
          protocolVersion_(protocolVersion),
          framing_(framing) {}

    HttpResponseBodyPlan bodyPlan_;
    HttpProtocolVersion protocolVersion_;
    Framing framing_;
};

[[nodiscard]] constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan,
    Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan,
        connectionPlan.protocolVersion(),
        Http1ResponseHeadPlan::chunkedStreamFraming());
}

[[nodiscard]] constexpr Http1ResponseHeadPlan
http1CloseDelimitedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan,
    Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan,
        connectionPlan.protocolVersion(),
        Http1ResponseHeadPlan::closeDelimitedStreamFraming());
}

// The runtime writes one inseparable HTTP/1 buffered response contract instead
// of independently passing body and head plans that can disagree on version or
// representation length.
class Http1BufferedResponsePlan final {
public:
    [[nodiscard]] const HttpBufferedResponseWritePlan&
    writePlan() const noexcept {
        return writePlan_;
    }

    [[nodiscard]] constexpr const Http1ResponseHeadPlan&
    headPlan() const noexcept {
        return headPlan_;
    }

private:
    friend Http1BufferedResponsePlan http1BufferedResponsePlan(
        HttpBufferedResponseWritePlan,
        Http1ServerConnectionPlan) noexcept;

    Http1BufferedResponsePlan(
        HttpBufferedResponseWritePlan writePlan,
        Http1ResponseHeadPlan headPlan) noexcept
        : writePlan_(writePlan), headPlan_(headPlan) {}

    HttpBufferedResponseWritePlan writePlan_;
    Http1ResponseHeadPlan headPlan_;
};

[[nodiscard]] inline Http1BufferedResponsePlan http1BufferedResponsePlan(
    HttpBufferedResponseWritePlan writePlan,
    Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1BufferedResponsePlan(
        writePlan,
        Http1ResponseHeadPlan(
            writePlan.bodyPlan(),
            connectionPlan.protocolVersion(),
            Http1ResponseHeadPlan::bufferedFraming(
                writePlan.contentLength())));
}

}  // namespace ruvia::detail
