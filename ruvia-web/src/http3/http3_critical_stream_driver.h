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
class http3_critical_stream_driver final {
public:
    using kind_type = ruvia::http3_critical_stream_output::stream_kind;
    using stream_id_type = std::uint64_t;
    enum class result_type : std::uint8_t { blocked,
        progress,
        ready,
        fatal };

    explicit http3_critical_stream_driver(http3_local_critical_streams prefixes) noexcept
        : output_(prefixes) {}
    http3_critical_stream_driver(const http3_critical_stream_driver&) = delete;
    http3_critical_stream_driver& operator=(const http3_critical_stream_driver&) = delete;
    http3_critical_stream_driver(http3_critical_stream_driver&&) = delete;
    http3_critical_stream_driver& operator=(http3_critical_stream_driver&&) = delete;

    // open(Kind) -> quic_stream_open_result; write(id, bytes) ->
    // quic_stream_write_result. At most one write per stream per tick.
    // Partial acceptance returns progress, so the owner can schedule another
    // tick; WANT/stream credit returns blocked without consuming any input.
    template <typename open_type, typename write_type>
    [[nodiscard]] result_type drive(open_type&& open, write_type&& write) {
        if (fatal_) {
            return result_type::fatal;
        }
        bool progress_value = false;
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            const auto kind = static_cast<kind_type>(i);
            if (!streams_[i]) {
                const auto created = open(kind);
                if (created.status_ == ruvia::quic_operation_status::would_block ||
                    created.status_ == ruvia::quic_operation_status::need_input) {
                    continue;
                }
                if (created.status_ != ruvia::quic_operation_status::accepted) {
                    fatal_ = true;
                    return result_type::fatal;
                }
                streams_[i] = created.stream_id_;
                progress_value = true;
            }
            const auto offered = output_->next(kind);
            if (offered.empty()) {
                continue;
            }
            const auto result_value = write(*streams_[i], offered);
            switch (result_value.status_) {
                case ruvia::quic_operation_status::accepted:
                    if (result_value.accepted_ > offered.size() ||
                        !output_->acknowledge(kind, result_value.accepted_)) {
                        fatal_ = true;
                        return result_type::fatal;
                    }
                    progress_value = progress_value || result_value.accepted_ != 0;
                    break;
                case ruvia::quic_operation_status::would_block:
                case ruvia::quic_operation_status::need_input:
                    (void)output_->acknowledge(kind, 0);
                    break;
                default:
                    fatal_ = true;
                    return result_type::fatal;
            }
        }
        if (output_->complete()) {
            return result_type::ready;
        }
        return progress_value ? result_type::progress : result_type::blocked;
    }

    // Server-only: identifier is a client-initiated request-stream boundary.
    [[nodiscard]] bool queue_goaway(std::uint64_t identifier) noexcept {
        return !fatal_ && output_->queue_goaway(identifier);
    }

    // Rejected 0-RTT removes the QUIC streams and rolls back their bytes.
    // Recreate the per-connection stream IDs and output cursors from SETTINGS.
    void restart(http3_local_critical_streams prefixes) noexcept {
        output_.reset();
        output_.emplace(std::move(prefixes));
        streams_.fill(std::nullopt);
        fatal_ = false;
    }

    // True when all prefixes and any queued GOAWAY have been accepted by QUIC.
    [[nodiscard]] bool complete() const noexcept {
        return output_->complete();
    }

    [[nodiscard]] std::optional<stream_id_type> stream_id(kind_type kind) const noexcept {
        const auto index = static_cast<std::size_t>(kind);
        return index < streams_.size() ? streams_[index] : std::nullopt;
    }

private:
    std::optional<ruvia::http3_critical_stream_output> output_;
    std::array<std::optional<stream_id_type>, 3> streams_{};
    bool fatal_{false};
};

}  // namespace ruvia::detail
