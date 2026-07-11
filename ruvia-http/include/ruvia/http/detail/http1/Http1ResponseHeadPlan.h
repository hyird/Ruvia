#pragma once

#include <variant>

#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace ruvia::detail {

class Http1ResponseHeadPlan;

// A complete buffered representation is length-delimited by the response writer.
class Http1BufferedResponseHead final {
private:
    friend class Http1ResponseHeadPlan;

    constexpr Http1BufferedResponseHead() noexcept = default;
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

private:
    friend constexpr Http1ResponseHeadPlan http1BufferedResponseHeadPlan(
        HttpResponseBodyPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
        HttpResponseBodyPlan) noexcept;
    friend constexpr Http1ResponseHeadPlan http1CloseDelimitedResponseStreamHeadPlan(
        HttpResponseBodyPlan) noexcept;

    using Framing = std::variant<
        Http1BufferedResponseHead,
        Http1ChunkedResponseStreamHead,
        Http1CloseDelimitedResponseStreamHead>;

    [[nodiscard]] static constexpr Framing bufferedFraming() noexcept {
        return Framing(Http1BufferedResponseHead());
    }

    [[nodiscard]] static constexpr Framing chunkedStreamFraming() noexcept {
        return Framing(Http1ChunkedResponseStreamHead());
    }

    [[nodiscard]] static constexpr Framing closeDelimitedStreamFraming() noexcept {
        return Framing(Http1CloseDelimitedResponseStreamHead());
    }

    constexpr Http1ResponseHeadPlan(
        HttpResponseBodyPlan bodyPlan,
        Framing framing) noexcept
        : bodyPlan_(bodyPlan), framing_(framing) {}

    HttpResponseBodyPlan bodyPlan_;
    Framing framing_;
};

[[nodiscard]] constexpr Http1ResponseHeadPlan http1BufferedResponseHeadPlan(
    HttpResponseBodyPlan bodyPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan,
        Http1ResponseHeadPlan::bufferedFraming());
}

[[nodiscard]] constexpr Http1ResponseHeadPlan http1ChunkedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan,
        Http1ResponseHeadPlan::chunkedStreamFraming());
}

[[nodiscard]] constexpr Http1ResponseHeadPlan
http1CloseDelimitedResponseStreamHeadPlan(
    HttpResponseBodyPlan bodyPlan) noexcept {
    return Http1ResponseHeadPlan(
        bodyPlan,
        Http1ResponseHeadPlan::closeDelimitedStreamFraming());
}

}  // namespace ruvia::detail
