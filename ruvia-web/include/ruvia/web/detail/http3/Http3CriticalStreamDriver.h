#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/web/detail/http3/Http3CriticalStreamOutput.h"
#include "ruvia/web/detail/http3/Http3QuicStreamSet.h"

namespace ruvia::detail {

// Owner-affine, allocation-free driver for the three local HTTP/3 critical
// streams. A connection owner supplies its typed QUIC open/write operations;
// it must keep this driver at a stable address while a write is pending, retry
// it after QUIC events/credit and close the connection on fatal. It can queue
// one GOAWAY after SETTINGS; the opened streams are NEVER concluded here.
class Http3CriticalStreamDriver final {
public:
    using Kind = Http3CriticalStreamOutput::Kind;
    using StreamId = Http3QuicStreamSet::StreamId;
    enum class Result : std::uint8_t { kBlocked,
        kProgress,
        kReady,
        kFatal };

    explicit Http3CriticalStreamDriver(Http3LocalCriticalStreams prefixes) noexcept
        : output_(prefixes) {}
    Http3CriticalStreamDriver(const Http3CriticalStreamDriver&) = delete;
    Http3CriticalStreamDriver& operator=(const Http3CriticalStreamDriver&) = delete;
    Http3CriticalStreamDriver(Http3CriticalStreamDriver&&) = delete;
    Http3CriticalStreamDriver& operator=(Http3CriticalStreamDriver&&) = delete;

    // open(Kind) -> Http3QuicStreamSet::OpenStream; write(id, bytes) ->
    // Http3QuicStreamSet::StreamWrite. At most one write per stream per tick.
    // Partial acceptance returns kProgress, so the owner can schedule another
    // tick; WANT/stream credit returns kBlocked without consuming any input.
    template <typename Open, typename Write>
    [[nodiscard]] Result drive(Open&& open, Write&& write) {
        if (fatal_) {
            return Result::kFatal;
        }
        bool progress = false;
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            const auto kind = static_cast<Kind>(i);
            if (!streams_[i]) {
                const auto created = open(kind);
                if (created.error == Http3QuicStreamSet::Error::kStreamLimitRetry ||
                    created.error == Http3QuicStreamSet::Error::kHandshakePending) {
                    continue;
                }
                if (created.error != Http3QuicStreamSet::Error::kNone) {
                    fatal_ = true;
                    return Result::kFatal;
                }
                streams_[i] = created.id;
                progress = true;
            }
            const auto offered = output_.next(kind);
            if (offered.empty()) {
                continue;
            }
            const auto result = write(*streams_[i], offered);
            switch (result.status) {
                case Http3QuicStreamSet::StreamWrite::Status::kAccepted:
                    if (result.bytes > offered.size() ||
                        !output_.acknowledge(kind, result.bytes)) {
                        fatal_ = true;
                        return Result::kFatal;
                    }
                    progress = progress || result.bytes != 0;
                    break;
                case Http3QuicStreamSet::StreamWrite::Status::kWouldBlock:
                    (void)output_.acknowledge(kind, 0);
                    break;
                default:
                    fatal_ = true;
                    return Result::kFatal;
            }
        }
        if (output_.complete()) {
            return Result::kReady;
        }
        return progress ? Result::kProgress : Result::kBlocked;
    }

    // Server-only: identifier is a client-initiated request-stream boundary.
    [[nodiscard]] bool queueGoaway(std::uint64_t identifier) noexcept {
        return !fatal_ && output_.queueGoaway(identifier);
    }

    // True when all prefixes and any queued GOAWAY have been accepted by QUIC.
    [[nodiscard]] bool complete() const noexcept {
        return output_.complete();
    }

    [[nodiscard]] std::optional<StreamId> streamId(Kind kind) const noexcept {
        const auto index = static_cast<std::size_t>(kind);
        return index < streams_.size() ? streams_[index] : std::nullopt;
    }

private:
    Http3CriticalStreamOutput output_;
    std::array<std::optional<StreamId>, 3> streams_{};
    bool fatal_{false};
};

}  // namespace ruvia::detail
