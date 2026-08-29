#pragma once

#include <cstdint>
#include <type_traits>
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

    explicit constexpr Http1BufferedResponseHead(std::uint64_t contentLength) noexcept
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

// A streamed response with an exact representation length. The runtime writes
// chunks incrementally, while the HTTP plan owns the one canonical
// Content-Length field and permits connection reuse on HTTP/1.0 and HTTP/1.1.
class Http1KnownLengthResponseStreamHead final {
public:
    [[nodiscard]] constexpr std::uint64_t contentLength() const noexcept {
        return contentLength_;
    }

private:
    friend class Http1ResponseHeadPlan;

    explicit constexpr Http1KnownLengthResponseStreamHead(std::uint64_t contentLength) noexcept
        : contentLength_(contentLength) {}

    std::uint64_t contentLength_{0};
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
    [[nodiscard]] constexpr const Http1BufferedResponseHead* buffered() const& noexcept {
        return std::get_if<Http1BufferedResponseHead>(&framing_);
    }
    [[nodiscard]] constexpr const Http1BufferedResponseHead* buffered() const&& = delete;

    [[nodiscard]] constexpr const Http1ChunkedResponseStreamHead* chunkedStream() const& noexcept {
        return std::get_if<Http1ChunkedResponseStreamHead>(&framing_);
    }
    [[nodiscard]] constexpr const Http1ChunkedResponseStreamHead* chunkedStream() const&& = delete;

    [[nodiscard]] constexpr const Http1KnownLengthResponseStreamHead* knownLengthStream()
        const& noexcept {
        return std::get_if<Http1KnownLengthResponseStreamHead>(&framing_);
    }
    [[nodiscard]] constexpr const Http1KnownLengthResponseStreamHead* knownLengthStream() const&& =
        delete;

    [[nodiscard]] constexpr const Http1CloseDelimitedResponseStreamHead* closeDelimitedStream()
        const& noexcept {
        return std::get_if<Http1CloseDelimitedResponseStreamHead>(&framing_);
    }
    [[nodiscard]] constexpr const Http1CloseDelimitedResponseStreamHead* closeDelimitedStream()
        const&& = delete;

    [[nodiscard]] constexpr HttpResponseBodyPlan bodyPlan() const noexcept {
        return bodyPlan_;
    }

    [[nodiscard]] constexpr HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

private:
    friend Http1BufferedResponsePlan http1BufferedResponsePlan(
        HttpBufferedResponseWritePlan, Http1ServerConnectionPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1KnownLengthResponseStreamHeadPlan(
        HttpResponseBodyPlan, Http1ServerConnectionPlan, std::uint64_t) noexcept;
    friend constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
        HttpResponseBodyPlan, Http1ServerConnectionPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1CloseDelimitedResponseStreamHeadPlan(
        HttpResponseBodyPlan, Http1ServerConnectionPlan) noexcept;

    using Framing = std::variant<Http1BufferedResponseHead, Http1KnownLengthResponseStreamHead,
        Http1ChunkedResponseStreamHead, Http1CloseDelimitedResponseStreamHead>;

    [[nodiscard]] static constexpr Framing bufferedFraming(std::uint64_t contentLength) noexcept {
        return Framing(Http1BufferedResponseHead(contentLength));
    }

    [[nodiscard]] static constexpr Framing chunkedStreamFraming() noexcept {
        return Framing(Http1ChunkedResponseStreamHead());
    }

    [[nodiscard]] static constexpr Framing knownLengthStreamFraming(
        std::uint64_t contentLength) noexcept {
        return Framing(Http1KnownLengthResponseStreamHead(contentLength));
    }

    [[nodiscard]] static constexpr Framing closeDelimitedStreamFraming() noexcept {
        return Framing(Http1CloseDelimitedResponseStreamHead());
    }

    constexpr Http1ResponseHeadPlan(HttpResponseBodyPlan bodyPlan,
        HttpProtocolVersion protocolVersion, Framing framing) noexcept
        : bodyPlan_(bodyPlan),
          protocolVersion_(protocolVersion),
          framing_(framing) {}

    HttpResponseBodyPlan bodyPlan_;
    HttpProtocolVersion protocolVersion_;
    Framing framing_;
};

[[nodiscard]] constexpr Http1ResponseHeadPlan http1KnownLengthResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan, Http1ServerConnectionPlan connectionPlan,
    std::uint64_t contentLength) noexcept {
    return Http1ResponseHeadPlan(bodyPlan, connectionPlan.protocolVersion(),
        Http1ResponseHeadPlan::knownLengthStreamFraming(contentLength));
}

[[nodiscard]] constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan, Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan, connectionPlan.protocolVersion(), Http1ResponseHeadPlan::chunkedStreamFraming());
}

[[nodiscard]] constexpr Http1ResponseHeadPlan http1CloseDelimitedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan, Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1ResponseHeadPlan(bodyPlan, connectionPlan.protocolVersion(),
        Http1ResponseHeadPlan::closeDelimitedStreamFraming());
}

// The runtime writes one inseparable HTTP/1 buffered response contract. The
// embedded head owns method/status/body semantics and representation length
// exactly once; direct fact access avoids restoring a parallel write-plan copy.
class Http1BufferedResponsePlan final {
public:
    [[nodiscard]] constexpr HttpResponseBodyPlan bodyPlan() const noexcept {
        return headPlan_.bodyPlan();
    }

    [[nodiscard]] HttpStatusCode responseStatus() const noexcept {
        return bodyPlan().responseStatus();
    }

    [[nodiscard]] constexpr std::uint64_t contentLength() const noexcept {
        return headPlan_.buffered()->contentLength();
    }

    [[nodiscard]] bool sendBody() const noexcept {
        return !bodyPlan().bodySuppressed() && contentLength() != 0;
    }

    [[nodiscard]] constexpr const Http1ResponseHeadPlan& headPlan() const& noexcept {
        return headPlan_;
    }
    [[nodiscard]] constexpr const Http1ResponseHeadPlan& headPlan() const&& = delete;

private:
    friend Http1BufferedResponsePlan http1BufferedResponsePlan(
        HttpBufferedResponseWritePlan, Http1ServerConnectionPlan) noexcept;

    explicit constexpr Http1BufferedResponsePlan(Http1ResponseHeadPlan headPlan) noexcept
        : headPlan_(headPlan) {}

    Http1ResponseHeadPlan headPlan_;
};

static_assert(std::is_trivially_copyable_v<Http1BufferedResponsePlan>);
static_assert(sizeof(Http1BufferedResponsePlan) == sizeof(Http1ResponseHeadPlan));

[[nodiscard]] inline Http1BufferedResponsePlan http1BufferedResponsePlan(
    HttpBufferedResponseWritePlan writePlan, Http1ServerConnectionPlan connectionPlan) noexcept {
    return Http1BufferedResponsePlan(
        Http1ResponseHeadPlan(writePlan.bodyPlan(), connectionPlan.protocolVersion(),
            Http1ResponseHeadPlan::bufferedFraming(writePlan.contentLength())));
}

}  // namespace ruvia::detail
