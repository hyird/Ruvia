#pragma once

#include <variant>

#include "ruvia/http/detail/http2/stream/Http2StreamCloseSource.h"

namespace ruvia::detail {

class Http2LocalSendState;
class Http2StreamLifecycle;

class Http2LocalHeadPending final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalHeadPending() noexcept = default;
};

class Http2LocalRequestContentOpen final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalRequestContentOpen() noexcept = default;
};

class Http2LocalResponseContentOpen final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalResponseContentOpen() noexcept = default;
};

class Http2LocalResponseTrailersOnly final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalResponseTrailersOnly() noexcept = default;
};

class Http2LocalConnectPending final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalConnectPending() noexcept = default;
};

class Http2LocalTunnelOpen final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalTunnelOpen() noexcept = default;
};

// END_STREAM is accepted by the core but still sits behind flow-control-blocked
// DATA or a deferred trailer section. No further semantic submission is legal.
class Http2LocalEndStreamQueued final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalEndStreamQueued() noexcept = default;
};

// The terminal HEADERS or DATA carrying END_STREAM has been materialized in the
// core-owned outbound buffer. This endpoint is now half-closed(local).
class Http2LocalEndStreamCommitted final {
private:
    friend class Http2LocalSendState;

    constexpr Http2LocalEndStreamCommitted() noexcept = default;
};

// Only abnormal whole-stream termination owns a close source. This covers a local
// or peer RST_STREAM as well as a GOAWAY last-stream-id exclusion; GOAWAY is not a
// reset, so naming this alternative after RST_STREAM would be protocol-inaccurate.
// Open and normally ended states cannot expose a close source.
class Http2StreamAborted final {
public:
    [[nodiscard]] constexpr Http2StreamCloseSource source() const noexcept {
        return source_;
    }

private:
    friend class Http2LocalSendState;

    explicit constexpr Http2StreamAborted(Http2StreamCloseSource source) noexcept
        : source_(source) {}

    Http2StreamCloseSource source_;
};

// Local frame permission is one exclusive protocol state. Request content,
// response content, response-trailers-only, and tunnel DATA are deliberately
// distinct, so a separate message-kind enum cannot contradict the active phase.
class Http2LocalSendState final {
private:
    friend class Http2StreamLifecycle;

    constexpr Http2LocalSendState() noexcept
        : state_(Http2LocalHeadPending()) {}

    [[nodiscard]] bool beginRequestContent() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalRequestContentOpen());
        return true;
    }

    [[nodiscard]] bool beginResponseContent() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalResponseContentOpen());
        return true;
    }

    [[nodiscard]] bool beginResponseTrailersOnly() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalResponseTrailersOnly());
        return true;
    }

    [[nodiscard]] bool beginConnectRequest() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalConnectPending());
        return true;
    }

    // A client opens from connect-pending after a peer 2xx; a server opens from
    // head-pending when it submits that 2xx. The owning stream validates the
    // separate CONNECT semantic state before calling this transition.
    [[nodiscard]] bool openTunnel() noexcept {
        if (headPending() == nullptr && connectPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalTunnelOpen());
        return true;
    }

    [[nodiscard]] bool commitHeadEndStream() noexcept {
        if (headPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalEndStreamCommitted());
        return true;
    }

    [[nodiscard]] bool rejectConnect() noexcept {
        if (connectPending() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalEndStreamCommitted());
        return true;
    }

    [[nodiscard]] bool queueEndStream() noexcept {
        if (!contentOrTrailersOpen()) {
            return false;
        }
        state_ = State(Http2LocalEndStreamQueued());
        return true;
    }

    [[nodiscard]] bool commitEndStream() noexcept {
        if (!contentOrTrailersOpen() && endStreamQueued() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalEndStreamCommitted());
        return true;
    }

    [[nodiscard]] bool abort(Http2StreamCloseSource source) noexcept {
        if (!http2IsValidStreamCloseSource(source) || aborted() != nullptr) {
            return false;
        }
        state_ = State(Http2StreamAborted(source));
        return true;
    }

public:
    [[nodiscard]] constexpr const Http2LocalHeadPending*
    headPending() const & noexcept {
        return std::get_if<Http2LocalHeadPending>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalHeadPending*
    headPending() const && = delete;

    [[nodiscard]] constexpr const Http2LocalRequestContentOpen*
    requestContentOpen() const & noexcept {
        return std::get_if<Http2LocalRequestContentOpen>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalRequestContentOpen*
    requestContentOpen() const && = delete;

    [[nodiscard]] constexpr const Http2LocalResponseContentOpen*
    responseContentOpen() const & noexcept {
        return std::get_if<Http2LocalResponseContentOpen>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalResponseContentOpen*
    responseContentOpen() const && = delete;

    [[nodiscard]] constexpr const Http2LocalResponseTrailersOnly*
    responseTrailersOnly() const & noexcept {
        return std::get_if<Http2LocalResponseTrailersOnly>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalResponseTrailersOnly*
    responseTrailersOnly() const && = delete;

    [[nodiscard]] constexpr const Http2LocalConnectPending*
    connectPending() const & noexcept {
        return std::get_if<Http2LocalConnectPending>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalConnectPending*
    connectPending() const && = delete;

    [[nodiscard]] constexpr const Http2LocalTunnelOpen*
    tunnelOpen() const & noexcept {
        return std::get_if<Http2LocalTunnelOpen>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalTunnelOpen*
    tunnelOpen() const && = delete;

    [[nodiscard]] constexpr const Http2LocalEndStreamQueued*
    endStreamQueued() const & noexcept {
        return std::get_if<Http2LocalEndStreamQueued>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalEndStreamQueued*
    endStreamQueued() const && = delete;

    [[nodiscard]] constexpr const Http2LocalEndStreamCommitted*
    endStreamCommitted() const & noexcept {
        return std::get_if<Http2LocalEndStreamCommitted>(&state_);
    }
    [[nodiscard]] constexpr const Http2LocalEndStreamCommitted*
    endStreamCommitted() const && = delete;

    [[nodiscard]] constexpr const Http2StreamAborted*
    aborted() const & noexcept {
        return std::get_if<Http2StreamAborted>(&state_);
    }
    [[nodiscard]] constexpr const Http2StreamAborted*
    aborted() const && = delete;

private:
    [[nodiscard]] bool contentOrTrailersOpen() const noexcept {
        return requestContentOpen() != nullptr ||
            responseContentOpen() != nullptr ||
            responseTrailersOnly() != nullptr ||
            tunnelOpen() != nullptr;
    }

    using State = std::variant<
        Http2LocalHeadPending,
        Http2LocalRequestContentOpen,
        Http2LocalResponseContentOpen,
        Http2LocalResponseTrailersOnly,
        Http2LocalConnectPending,
        Http2LocalTunnelOpen,
        Http2LocalEndStreamQueued,
        Http2LocalEndStreamCommitted,
        Http2StreamAborted>;

    State state_;
};

}  // namespace ruvia::detail
