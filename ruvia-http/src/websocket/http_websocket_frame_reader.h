#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/protocol_byte_limit.h"

#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_frame_view.h"
#include "websocket/http_websocket_payload_validation.h"

namespace ruvia::detail {

// Drop the bytes a completed frame consumed, compacting the buffer only once the
// dead prefix is worth the move.
inline void compact_websocket_read_buffer(
    std::pmr::string& buffer, std::size_t& offset, std::size_t& pending_compact_until) noexcept {
    if (pending_compact_until == 0) {
        return;
    }

    const auto consumed = std::exchange(pending_compact_until, 0);
    if (consumed >= buffer.size()) {
        buffer.clear();
        if (buffer.capacity() > 4096) {
            std::pmr::string(buffer.get_allocator()).swap(buffer);
        }
        offset = 0;
        return;
    }

    offset = consumed;
    constexpr std::size_t compact_threshold_bytes = 16 * 1024;
    if (consumed < compact_threshold_bytes && consumed < buffer.size() - consumed) {
        return;
    }

    const auto remaining = buffer.size() - consumed;
    std::memmove(buffer.data(), buffer.data() + consumed, remaining);
    buffer.resize(remaining);
    offset = 0;
}

class websocket_frame_read_result;

class websocket_frame_need_input final {
private:
    friend class websocket_frame_read_result;
    constexpr websocket_frame_need_input() noexcept = default;
};

class websocket_frame_read_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_frame_read_result;

    explicit constexpr websocket_frame_read_failure(websocket_protocol_failure error) noexcept
        : error_(error) {}

    websocket_protocol_failure error_;
};

// Parsing either needs more input, exposes one complete borrowed frame, or reports
// one typed protocol failure. No default frame, byte-count hint, EOF boolean, or
// wire-format exception can coexist with another outcome.
class websocket_frame_read_result final {
public:
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const& noexcept {
        return std::get_if<websocket_frame_need_input>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_view* frame() const& noexcept {
        return std::get_if<websocket_frame_view>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_view* frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const& noexcept {
        return std::get_if<websocket_frame_read_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const&& = delete;

private:
    friend websocket_frame_read_result websocket_try_read_frame(
        std::pmr::string&, std::size_t&, std::size_t&, protocol_byte_limit, bool, bool);

    using value_type =
        std::variant<websocket_frame_need_input, websocket_frame_view, websocket_frame_read_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_frame_read_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr websocket_frame_read_result make_need_input() noexcept {
        return websocket_frame_read_result(websocket_frame_need_input());
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_frame(
        const websocket_frame_start& start, std::string_view payload_value) noexcept {
        return websocket_frame_read_result(websocket_frame_view(start, payload_value));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_failure(
        websocket_protocol_failure error) noexcept {
        return websocket_frame_read_result(websocket_frame_read_failure(error));
    }

    value_type value_;
};

// Single owner of websocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, role-correct masking validation, control-frame and length-limit
// validation, and in-place unmasking when the peer is a client.
// It never performs I/O and never throws for peer bytes; callers append transport
// input after need_input(), while failure() carries the Close reason.
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(std::pmr::string& buffer,
    std::size_t& offset, std::size_t& pending_compact_until, protocol_byte_limit message_limit,
    bool permessage_deflate, bool expect_masked) {
    compact_websocket_read_buffer(buffer, offset, pending_compact_until);
    const auto available = buffer.size() - offset;
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    const auto first = static_cast<unsigned char>(buffer[offset]);
    const auto second = static_cast<unsigned char>(buffer[offset + 1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start =
        decode_websocket_frame_start(first, second, permessage_deflate, expect_masked);
    if (!frame_start.has_value()) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (length == 126) {
        if (available < header_size + 2) {
            return websocket_frame_read_result::make_need_input();
        }
        length = read_websocket_uint16(buffer.data() + offset + header_size);
        header_size += 2;
        // RFC 6455 §5.2: the minimal number of length bytes MUST be used, so a
        // value <126 may not use the 16-bit form (e.g. 126,0,124 for a 124-byte
        // payload). A conformant peer never emits this; reject the non-minimal frame.
        if (length < 126) {
            return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
        }
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = read_websocket_uint64(
            std::span<const char, 8>(buffer.data() + offset + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
        // RFC 6455 §5.2 minimal-length rule: a value fitting the 16-bit form may
        // not use the 64-bit form.
        if (length <= 0xFFFFU) {
            return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
        }
    }

    if (is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::message_too_large);
    }
    const auto mask_bytes = expect_masked ? std::size_t{4} : std::size_t{0};
    if (header_size > (std::numeric_limits<std::size_t>::max)() - mask_bytes ||
        length > static_cast<std::uint64_t>(
                     (std::numeric_limits<std::size_t>::max)() - header_size - mask_bytes)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    const auto total_frame_bytes = header_size + mask_bytes + static_cast<std::size_t>(length);
    if (available < total_frame_bytes) {
        return websocket_frame_read_result::make_need_input();
    }

    const auto mask_offset = offset + header_size;
    const auto payload_offset = mask_offset + mask_bytes;
    const auto payload_size = static_cast<std::size_t>(length);
    auto* payload_value = buffer.data() + payload_offset;
    if (expect_masked) {
        decode_masked_websocket_payload(payload_value, payload_size, buffer.data() + mask_offset);
    }
    const auto payload_view = std::string_view(payload_value, payload_size);
    if (frame_start->kind() == websocket_frame_kind::close) {
        if (const auto failure = websocket_close_payload_failure(payload_view)) {
            return websocket_frame_read_result::make_failure(*failure);
        }
    }
    offset = payload_offset + payload_size;
    pending_compact_until = offset;
    return websocket_frame_read_result::make_frame(*frame_start, payload_view);
}

[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(std::pmr::string& buffer,
    std::size_t& offset, std::size_t& pending_compact_until, protocol_byte_limit message_limit,
    bool permessage_deflate) {
    return websocket_try_read_frame(
        buffer, offset, pending_compact_until, message_limit, permessage_deflate, true);
}
}  // namespace ruvia::detail
