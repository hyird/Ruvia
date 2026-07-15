#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/core/detail/WorkerSignal.h"

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
    constexpr
    Http2BufferedResponseWritePeerAbortedBeforeCommit() noexcept = default;
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
private:
    friend class Http2BufferedResponseWriteResult;
    constexpr Http2BufferedResponseWriteFailedBeforeCommit() noexcept = default;
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

// The writer completes transport recovery (including RESET_STREAM) before
// returning. Each terminal alternative preserves both the response commit
// boundary and whether termination came from the peer or the local write path.
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
    makeFailedBeforeCommit() noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWriteFailedBeforeCommit{});
    }

    [[nodiscard]] static constexpr Http2BufferedResponseWriteResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http2BufferedResponseWriteResult(
            Http2BufferedResponseWriteFailedAfterCommit(status));
    }

    [[nodiscard]] constexpr const Http2BufferedResponseWriteCompleted*
    completed() const & noexcept {
        return std::get_if<Http2BufferedResponseWriteCompleted>(&value_);
    }
    const Http2BufferedResponseWriteCompleted* completed() const && = delete;

    [[nodiscard]] constexpr const
    Http2BufferedResponseWritePeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const & noexcept {
        return std::get_if<
            Http2BufferedResponseWritePeerAbortedBeforeCommit>(&value_);
    }
    const Http2BufferedResponseWritePeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const && = delete;

    [[nodiscard]] constexpr const
    Http2BufferedResponseWritePeerAbortedAfterCommit*
    peerAbortedAfterCommit() const & noexcept {
        return std::get_if<
            Http2BufferedResponseWritePeerAbortedAfterCommit>(&value_);
    }
    const Http2BufferedResponseWritePeerAbortedAfterCommit*
    peerAbortedAfterCommit() const && = delete;

    [[nodiscard]] constexpr const Http2BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const & noexcept {
        return std::get_if<Http2BufferedResponseWriteFailedBeforeCommit>(
            &value_);
    }
    const Http2BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const && = delete;

    [[nodiscard]] constexpr const Http2BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const & noexcept {
        return std::get_if<Http2BufferedResponseWriteFailedAfterCommit>(&value_);
    }
    const Http2BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const && = delete;

    [[nodiscard]] constexpr std::optional<std::uint16_t>
    committedStatus() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = peerAbortedAfterCommit()) {
            return value->status();
        }
        if (const auto* value = failedAfterCommit()) {
            return value->status();
        }
        return std::nullopt;
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
        : value_(alternative) {}

    Value value_;
};

static_assert(std::is_trivially_copyable_v<Http2BufferedResponseWriteResult>);
static_assert(sizeof(Http2BufferedResponseWriteResult) <= 4);

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
        WorkerSignal& writeSignal) noexcept;

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

    [[nodiscard]] Task<DataWriteResult> writeData(
        std::uint32_t streamId,
        std::string_view chunk,
        Http2EndStream endStream);
    void wakeWriter() noexcept;

    Http2Connection* connection_;
    Http2SansIoStreamRuntimeTable* streamRuntimes_;
    WorkerMemory* worker_;
    WorkerSignal* writeSignal_;
};

}  // namespace ruvia::detail
