#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/steady_timer.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/http/detail/http2/Http2Connection.h"

namespace ruvia {
class HttpResponse;
class WorkerMemory;
}

namespace ruvia::detail {
class Http2SansIoStreamRuntimeTable;
class HttpBufferedResponseWritePlan;

class Http2BufferedResponseWriteCompleted final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseWriteResult;

    explicit constexpr Http2BufferedResponseWriteCompleted(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class Http2BufferedResponseWritePeerAbortedBeforeCommit final {
private:
    friend class Http2BufferedResponseWriteResult;

    constexpr Http2BufferedResponseWritePeerAbortedBeforeCommit() noexcept = default;
};

class Http2BufferedResponseWritePeerAbortedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseWriteResult;

    explicit constexpr Http2BufferedResponseWritePeerAbortedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class Http2BufferedResponseWriteFailedBeforeCommit final {
public:
    [[nodiscard]] constexpr Http2ResponseHeadSubmitError error() const noexcept {
        return error_;
    }

private:
    friend class Http2BufferedResponseWriteResult;

    explicit constexpr Http2BufferedResponseWriteFailedBeforeCommit(
        Http2ResponseHeadSubmitError error) noexcept
        : error_(error) {}

    Http2ResponseHeadSubmitError error_;
};

class Http2BufferedResponseWriteFailedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http2BufferedResponseWriteResult;

    explicit constexpr Http2BufferedResponseWriteFailedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

// A buffered HTTP/2 response has an HTTP status only after the core commits the
// final HEADERS transaction. Every terminal alternative therefore owns exactly
// the metadata valid for that point in the send lifecycle: pre-commit aborts and
// failures cannot manufacture a status, while all post-commit outcomes carry the
// status from the submitted HttpBufferedResponseWritePlan.
class Http2BufferedResponseWriteResult final {
public:
    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makeCompleted(std::uint16_t status) noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWriteCompleted(status));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makePeerAbortedBeforeCommit() noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWritePeerAbortedBeforeCommit{});
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makePeerAbortedAfterCommit(std::uint16_t status) noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWritePeerAbortedAfterCommit(status));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makeFailedBeforeCommit(Http2ResponseHeadSubmitError error) noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWriteFailedBeforeCommit(error));
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWriteFailedAfterCommit(status));
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWriteCompleted*
    completed() const noexcept {
        return std::get_if<Http2BufferedResponseWriteCompleted>(&value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWritePeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const noexcept {
        return std::get_if<Http2BufferedResponseWritePeerAbortedBeforeCommit>(
            &value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWritePeerAbortedAfterCommit*
    peerAbortedAfterCommit() const noexcept {
        return std::get_if<Http2BufferedResponseWritePeerAbortedAfterCommit>(
            &value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const noexcept {
        return std::get_if<Http2BufferedResponseWriteFailedBeforeCommit>(&value_);
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const noexcept {
        return std::get_if<Http2BufferedResponseWriteFailedAfterCommit>(&value_);
    }

private:
    using Value = std::variant<
        Http2BufferedResponseWriteCompleted,
        Http2BufferedResponseWritePeerAbortedBeforeCommit,
        Http2BufferedResponseWritePeerAbortedAfterCommit,
        Http2BufferedResponseWriteFailedBeforeCommit,
        Http2BufferedResponseWriteFailedAfterCommit>;

    template <typename Alternative>
    explicit constexpr Http2BufferedResponseWriteResult(
        Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, file I/O, and typed terminal result chain. Keeping it
// non-template compiles that policy once for plain and TLS sessions.
class Http2BufferedResponseWriter final {
public:
    Http2BufferedResponseWriter(
        Http2Connection& connection,
        Http2SansIoStreamRuntimeTable& streamRuntimes,
        WorkerMemory& worker,
        asio::steady_timer& writeSignal) noexcept;

    [[nodiscard]] Task<Http2BufferedResponseWriteResult> write(
        std::uint32_t streamId,
        const HttpResponse& response,
        HttpBufferedResponseWritePlan writePlan);

private:
    enum class DataWriteResult : std::uint8_t {
        kCompleted,
        kPeerAborted,
        kFailed
    };

    [[nodiscard]] Task<bool> awaitSendWindow(std::uint32_t streamId);
    [[nodiscard]] Task<DataWriteResult> writeData(
        std::uint32_t streamId,
        std::string_view chunk,
        Http2EndStream endStream);
    void wakeWriter() noexcept;

    Http2Connection* connection_;
    Http2SansIoStreamRuntimeTable* streamRuntimes_;
    WorkerMemory* worker_;
    asio::steady_timer* writeSignal_;
};

}  // namespace ruvia::detail
