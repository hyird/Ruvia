#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "ruvia/http/http3_critical_stream_output.h"
#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {

// Owner-affine, allocation-free driver for the three local HTTP/3 critical
// streams of one client or server connection. It opens the streams, writes the
// local prefixes and any queued GOAWAY, then the protocol engine's pending
// critical output straight from its stable pending span. The owner retries it
// after QUIC events/credit and closes the connection on fatal. The opened
// streams are NEVER concluded here.
class http3_critical_stream_driver final {
public:
    using kind_type = ruvia::http3_critical_stream_output::stream_kind;
    using stream_id_type = std::uint64_t;
    enum class status_type : std::uint8_t { blocked,
        ready,
        fatal };
    struct result_type final {
        status_type status_{status_type::blocked};
        // Independent of readiness: accepting engine bytes after the prefixes
        // completed still advances the critical output's inactivity deadline.
        bool progress_{};
    };
    enum class engine_output : std::uint8_t { defer,
        send };

    explicit http3_critical_stream_driver(http3_local_critical_streams prefixes) noexcept
        : output_(prefixes) {}
    http3_critical_stream_driver(const http3_critical_stream_driver&) = delete;
    http3_critical_stream_driver& operator=(const http3_critical_stream_driver&) = delete;
    http3_critical_stream_driver(http3_critical_stream_driver&&) = delete;
    http3_critical_stream_driver& operator=(http3_critical_stream_driver&&) = delete;

    // The one critical-output pump for client and server connections. engine
    // exposes `std::span<const char> pending_critical_output(kind_type)` and
    // `bool consume_critical_output(kind_type, std::size_t)` (ruvia::http3_connection
    // or a session engine forwarding to it). Local prefix/GOAWAY bytes precede
    // engine bytes on each stream, and neither source starts while the other
    // has a partially accepted unit. ready means the local prefixes and queued
    // GOAWAY are accepted; engine output does not affect it. A client defers
    // engine bytes during 0-RTT so rejected early streams only need their
    // prefixes recreated; queued engine bytes stay owned until the handshake.
    template <typename engine_type>
    [[nodiscard]] result_type drive(ruvia::quic_connection& quic, engine_type& engine,
        engine_output output_policy) {
        if (fatal_) {
            return {.status_ = status_type::fatal};
        }
        bool progress_value = false;
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            const auto kind = static_cast<kind_type>(i);
            if (!streams_[i]) {
                const auto created = quic.open_stream(true);
                if (created.status_ == ruvia::quic_operation_status::would_block ||
                    created.status_ == ruvia::quic_operation_status::need_input) {
                    continue;
                }
                if (created.status_ != ruvia::quic_operation_status::accepted) {
                    fatal_ = true;
                    return {.status_ = status_type::fatal};
                }
                streams_[i] = created.stream_id_;
                progress_value = true;
            }
            if (!engine_partial_[i]) {
                const auto offered = output_->next(kind);
                if (!offered.empty()) {
                    const auto written = quic.write_stream(*streams_[i], std::as_bytes(offered));
                    if (!accept_write(written, offered.size()) ||
                        !output_->acknowledge(kind, written.accepted_)) {
                        fatal_ = true;
                        return {.status_ = status_type::fatal};
                    }
                    progress_value = progress_value || written.accepted_ != 0;
                    if (!output_->next(kind).empty()) {
                        continue;
                    }
                }
            }
            if (output_policy == engine_output::defer) {
                continue;
            }
            const auto pending = engine.pending_critical_output(kind);
            if (pending.empty()) {
                continue;
            }
            const auto written = quic.write_stream(*streams_[i], std::as_bytes(pending));
            if (!accept_write(written, pending.size()) ||
                (written.accepted_ != 0 && !engine.consume_critical_output(kind, written.accepted_))) {
                fatal_ = true;
                return {.status_ = status_type::fatal};
            }
            if (written.accepted_ != 0) {
                engine_partial_[i] = written.accepted_ < pending.size();
                progress_value = true;
            }
        }
        return {.status_ = output_->complete() ? status_type::ready : status_type::blocked,
            .progress_ = progress_value};
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
        engine_partial_.fill(false);
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
    [[nodiscard]] static bool accept_write(
        const ruvia::quic_stream_write_result& written, std::size_t offered) noexcept {
        if (written.status_ == ruvia::quic_operation_status::would_block ||
            written.status_ == ruvia::quic_operation_status::need_input) {
            return written.accepted_ == 0;
        }
        return written.status_ == ruvia::quic_operation_status::accepted && written.accepted_ <= offered;
    }

    std::optional<ruvia::http3_critical_stream_output> output_;
    std::array<std::optional<stream_id_type>, 3> streams_{};
    std::array<bool, 3> engine_partial_{};
    bool fatal_{false};
};

}  // namespace ruvia::detail
