#pragma once

#include <cstdint>
#include <utility>
#include <variant>

#include "ruvia/http/detail/http2/Http2Connection.h"

namespace ruvia::detail {

class Http2BufferedResponseCompleted final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseDispatchResult;

    explicit constexpr Http2BufferedResponseCompleted(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class Http2BufferedResponsePeerAbortedBeforeCommit final {
private:
    friend class Http2BufferedResponseDispatchResult;

    constexpr Http2BufferedResponsePeerAbortedBeforeCommit() noexcept = default;
};

class Http2BufferedResponsePeerAbortedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseDispatchResult;

    explicit constexpr Http2BufferedResponsePeerAbortedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class Http2BufferedResponseFailedBeforeCommit final {
public:
    [[nodiscard]] constexpr Http2ResponseHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2BufferedResponseDispatchResult;

    explicit constexpr Http2BufferedResponseFailedBeforeCommit(
        Http2ResponseHeadSubmitError error) noexcept
        : error_(error) {}

    Http2ResponseHeadSubmitError error_;
};

class Http2BufferedResponseFailedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseDispatchResult;

    explicit constexpr Http2BufferedResponseFailedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

// A buffered HTTP/2 response has an HTTP status only after the core commits the
// final HEADERS transaction. Every terminal alternative therefore owns exactly
// the metadata valid for that point in the send lifecycle: pre-commit aborts and
// failures cannot manufacture a status, while all post-commit outcomes carry the
// status from the submitted HttpBufferedResponseWritePlan.
class Http2BufferedResponseDispatchResult final {
public:
    [[nodiscard]] static constexpr Http2BufferedResponseDispatchResult
    makeCompleted(std::uint16_t status) noexcept {
        return Http2BufferedResponseDispatchResult(
            Http2BufferedResponseCompleted(status));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseDispatchResult
    makePeerAbortedBeforeCommit() noexcept {
        return Http2BufferedResponseDispatchResult(
            Http2BufferedResponsePeerAbortedBeforeCommit{});
    }

    [[nodiscard]] static constexpr Http2BufferedResponseDispatchResult
    makePeerAbortedAfterCommit(std::uint16_t status) noexcept {
        return Http2BufferedResponseDispatchResult(
            Http2BufferedResponsePeerAbortedAfterCommit(status));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseDispatchResult
    makeFailedBeforeCommit(Http2ResponseHeadSubmitError error) noexcept {
        return Http2BufferedResponseDispatchResult(
            Http2BufferedResponseFailedBeforeCommit(error));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseDispatchResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http2BufferedResponseDispatchResult(
            Http2BufferedResponseFailedAfterCommit(status));
    }

    [[nodiscard]] constexpr const Http2BufferedResponseCompleted*
    completed() const noexcept {
        return std::get_if<Http2BufferedResponseCompleted>(&value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponsePeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const noexcept {
        return std::get_if<Http2BufferedResponsePeerAbortedBeforeCommit>(
            &value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponsePeerAbortedAfterCommit*
    peerAbortedAfterCommit() const noexcept {
        return std::get_if<Http2BufferedResponsePeerAbortedAfterCommit>(
            &value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseFailedBeforeCommit*
    failedBeforeCommit() const noexcept {
        return std::get_if<Http2BufferedResponseFailedBeforeCommit>(&value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseFailedAfterCommit*
    failedAfterCommit() const noexcept {
        return std::get_if<Http2BufferedResponseFailedAfterCommit>(&value_);
    }

private:
    using Value = std::variant<
        Http2BufferedResponseCompleted,
        Http2BufferedResponsePeerAbortedBeforeCommit,
        Http2BufferedResponsePeerAbortedAfterCommit,
        Http2BufferedResponseFailedBeforeCommit,
        Http2BufferedResponseFailedAfterCommit>;

    template <typename Alternative>
    explicit constexpr Http2BufferedResponseDispatchResult(
        Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

}  // namespace ruvia::detail
