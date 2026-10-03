#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/http/http3_critical_stream_output.h"
#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {

// Owner-affine, allocation-free driver for the three local HTTP/3 critical
// streams. A connection owner supplies its typed QUIC open/write operations;
// it must keep this driver at a stable address while a write is pending, retry
// it after QUIC events/credit and close the connection on fatal. It can queue
// one GOAWAY after SETTINGS; the opened streams are NEVER concluded here.
class Http3CriticalStreamDriver final {
public:
    using Kind = ruvia::http3_critical_stream_output::stream_kind;
    using StreamId = std::uint64_t;
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

    // open(Kind) -> quic_stream_open_result; write(id, bytes) ->
    // quic_stream_write_result. At most one write per stream per tick.
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
                if (created.status == ruvia::quic_operation_status::would_block ||
                    created.status == ruvia::quic_operation_status::need_input) {
                    continue;
                }
                if (created.status != ruvia::quic_operation_status::accepted) {
                    fatal_ = true;
                    return Result::kFatal;
                }
                streams_[i] = created.stream_id;
                progress = true;
            }
            const auto offered = output_->next(kind);
            if (offered.empty()) {
                continue;
            }
            const auto result = write(*streams_[i], offered);
            switch (result.status) {
                case ruvia::quic_operation_status::accepted:
                    if (result.accepted > offered.size() ||
                        !output_->acknowledge(kind, result.accepted)) {
                        fatal_ = true;
                        return Result::kFatal;
                    }
                    progress = progress || result.accepted != 0;
                    break;
                case ruvia::quic_operation_status::would_block:
                case ruvia::quic_operation_status::need_input:
                    (void)output_->acknowledge(kind, 0);
                    break;
                default:
                    fatal_ = true;
                    return Result::kFatal;
            }
        }
        if (output_->complete()) {
            return Result::kReady;
        }
        return progress ? Result::kProgress : Result::kBlocked;
    }

    // Server-only: identifier is a client-initiated request-stream boundary.
    [[nodiscard]] bool queueGoaway(std::uint64_t identifier) noexcept {
        return !fatal_ && output_->queue_goaway(identifier);
    }

    // Rejected 0-RTT removes the QUIC streams and rolls back their bytes.
    // Recreate the per-connection stream IDs and output cursors from SETTINGS.
    void restart(Http3LocalCriticalStreams prefixes) noexcept {
        output_.reset();
        output_.emplace(std::move(prefixes));
        streams_.fill(std::nullopt);
        fatal_ = false;
    }

    // True when all prefixes and any queued GOAWAY have been accepted by QUIC.
    [[nodiscard]] bool complete() const noexcept {
        return output_->complete();
    }

    [[nodiscard]] std::optional<StreamId> streamId(Kind kind) const noexcept {
        const auto index = static_cast<std::size_t>(kind);
        return index < streams_.size() ? streams_[index] : std::nullopt;
    }

private:
    std::optional<ruvia::http3_critical_stream_output> output_;
    std::array<std::optional<StreamId>, 3> streams_{};
    bool fatal_{false};
};

}  // namespace ruvia::detail
