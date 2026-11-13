#pragma once

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"

#include "field/http_origin_payload.h"
#include "wire/quic_varint.h"

namespace ruvia::detail {

// Dynamic ORIGIN frames share the control stream with nghttp3 output. The
// segment ledger preserves their wire order without advancing nghttp3 offsets
// for extension bytes. Each frame owns address-stable storage until its ACK.
class http3_control_extension_output final {
    struct frame final {
        explicit frame(std::size_t size)
            : bytes_(size) {}

        std::vector<std::byte> bytes_;
        std::size_t written_{0};
        std::size_t acknowledged_{0};
    };

    struct segment final {
        bool nghttp3_{true};
        std::size_t bytes_{0};
    };

public:
    http3_control_extension_output() noexcept = default;

    http3_control_extension_output(const http3_control_extension_output&) = delete;
    http3_control_extension_output& operator=(const http3_control_extension_output&) = delete;
    http3_control_extension_output(http3_control_extension_output&&) = delete;
    http3_control_extension_output& operator=(http3_control_extension_output&&) = delete;

    [[nodiscard]] http3_submit_status queue(std::span<const std::string_view> origins, std::size_t limit) {
        if (shutdown_) {
            return http3_submit_status::invalid_state;
        }
        const auto payload_size = http_origin_payload_size(origins, 16384);
        if (payload_size.index() != 0) {
            return http3_submit_status::invalid_message;
        }
        const auto length = std::get<0>(payload_size);
        const auto length_varint_bytes = quic_varint_size(length);
        const auto total_bytes = 1 + length_varint_bytes + length;
        if (total_bytes > limit - (std::min)(limit, pending_bytes_)) {
            return http3_submit_status::backpressured;
        }

        // All allocation precedes publication of counters. Appending never
        // relocates another frame's bytes already borrowed by QUIC.
        auto& queued = frames_.emplace_back(total_bytes);
        auto* const dst = queued.bytes_.data();
        dst[0] = std::byte{0x0c};
        (void)encode_quic_varint(
            {reinterpret_cast<char*>(dst + 1), length_varint_bytes}, length);
        write_http_origin_payload(
            {reinterpret_cast<char*>(dst + 1 + length_varint_bytes), length}, origins);
        pending_bytes_ += total_bytes;
        unwritten_bytes_ += total_bytes;
        return http3_submit_status::accepted;
    }

    // nullopt leaves output to nghttp3; the caller owns the control stream id
    // and initializes output before trying either source.
    [[nodiscard]] std::optional<http3_transport_result> peek(nghttp3_conn* connection,
        std::int64_t control_id, std::size_t max_bytes, http3_stream_output& output) noexcept {
        peeked_ = false;
        if (blocked_ || shutdown_ || unwritten_bytes_ == 0 ||
            (!frame_in_progress_ && nghttp3_conn_is_stream_flushed(connection, control_id) == 0)) {
            return std::nullopt;
        }
        if (max_bytes == 0) {
            return http3_transport_result{.status_ = http3_transport_status::would_block};
        }
        const auto& current = frames_[write_frame_];
        const auto offered = (std::min)(current.bytes_.size() - current.written_, max_bytes);
        output.stream_id_ = control_id;
        output.buffers_[0] = {current.bytes_.data() + current.written_, offered};
        output.buffer_count_ = 1;
        output.fin_ = false;
        peeked_ = true;
        return http3_transport_result{};
    }

    [[nodiscard]] int commit(nghttp3_conn* connection, std::int64_t id, std::size_t bytes) noexcept {
        const bool from_nghttp3 = !peeked_;
        peeked_ = false;
        if (!from_nghttp3) {
            if (bytes > unwritten_bytes_) {
                return NGHTTP3_ERR_INVALID_ARGUMENT;
            }
            if (bytes != 0) {
                if (!segments_.empty() && !segments_.back().nghttp3_) {
                    segments_.back().bytes_ += bytes;
                } else {
                    try {
                        segments_.push_back({false, bytes});
                    } catch (...) {
                        return NGHTTP3_ERR_H3_INTERNAL_ERROR;
                    }
                }
            }
            auto remaining = bytes;
            while (remaining != 0) {
                auto& current = frames_[write_frame_];
                const auto step = (std::min)(remaining, current.bytes_.size() - current.written_);
                current.written_ += step;
                remaining -= step;
                if (current.written_ == current.bytes_.size()) {
                    ++write_frame_;
                }
            }
            unwritten_bytes_ -= bytes;
            frame_in_progress_ = unwritten_bytes_ != 0;
            return 0;
        }

        const bool extend_last = bytes != 0 && !segments_.empty() && segments_.back().nghttp3_;
        if (bytes != 0 && !extend_last) {
            try {
                segments_.push_back({true, 0});
            } catch (...) {
                return NGHTTP3_ERR_H3_INTERNAL_ERROR;
            }
        }
        const int result = nghttp3_conn_add_write_offset(connection, id, bytes);
        if (result != 0) {
            if (bytes != 0 && !extend_last) {
                segments_.pop_back();
            }
            return result;
        }
        if (bytes != 0) {
            segments_.back().bytes_ += bytes;
        }
        return 0;
    }

    [[nodiscard]] int acknowledge(nghttp3_conn* connection, std::int64_t id, std::uint64_t bytes) noexcept {
        if (!segments_.empty()) {
            while (bytes != 0 && !segments_.empty()) {
                auto& current = segments_.front();
                const auto step = static_cast<std::size_t>(
                    (std::min)(bytes, static_cast<std::uint64_t>(current.bytes_)));
                if (current.nghttp3_) {
                    const int result = nghttp3_conn_add_ack_offset(connection, id, step);
                    if (result != 0) {
                        return result;
                    }
                } else {
                    acknowledge_frames((std::min)(step, pending_bytes_ - unwritten_bytes_));
                }
                current.bytes_ -= step;
                bytes -= step;
                if (current.bytes_ == 0) {
                    segments_.pop_front();
                }
            }
            if (bytes == 0) {
                return 0;
            }
        }
        return nghttp3_conn_add_ack_offset(connection, id, bytes);
    }

    void block() noexcept {
        blocked_ = true;
    }

    void unblock() noexcept {
        blocked_ = false;
    }

    void shutdown() noexcept {
        shutdown_ = true;
        reset();
        // Outstanding extension segments still consume their wire ACK ranges;
        // later nghttp3 segments must retain their original relative offsets.
    }

    [[nodiscard]] bool shutdown_requested() const noexcept {
        return shutdown_;
    }

private:
    void acknowledge_frames(std::size_t bytes) noexcept {
        while (bytes != 0) {
            auto& current = frames_.front();
            const auto step = (std::min)(bytes, current.written_ - current.acknowledged_);
            current.acknowledged_ += step;
            pending_bytes_ -= step;
            bytes -= step;
            if (current.acknowledged_ == current.bytes_.size()) {
                frames_.pop_front();
                --write_frame_;
            }
        }
        if (pending_bytes_ == 0) {
            reset();
        }
    }

    void reset() noexcept {
        frames_.clear();
        pending_bytes_ = 0;
        unwritten_bytes_ = 0;
        write_frame_ = 0;
        frame_in_progress_ = false;
    }

    std::deque<frame> frames_;
    std::deque<segment> segments_;
    std::size_t pending_bytes_{0};
    std::size_t unwritten_bytes_{0};
    std::size_t write_frame_{0};
    bool blocked_{false};
    bool shutdown_{false};
    bool peeked_{false};
    bool frame_in_progress_{false};
};

}  // namespace ruvia::detail
