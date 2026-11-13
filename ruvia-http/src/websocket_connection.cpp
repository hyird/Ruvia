#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"

#include "http_websocket_permessage_deflate.h"
// The Close frame payload (RFC 6455 section 5.5.1): encoding the one an
// endpoint sends (a validated status code followed by an optional UTF-8
// reason, capped at the 125-byte control-frame limit) and checking the one a
// peer sent.

#include <bit>
#include <exception>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "http_connection_fields.h"
#include "http_zlib_stream.h"
namespace ruvia::detail {

enum class websocket_close_payload_encode_error : std::uint8_t {
    invalid_reason,
    reason_too_large,
};

class websocket_close_payload_encode_result;

class websocket_encoded_close_payload final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const&& = delete;

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_;
    std::uint8_t size_{0};
};

class websocket_close_payload_encode_failure final {
public:
    [[nodiscard]] constexpr websocket_close_payload_encode_error error() const noexcept {
        return error_;
    }

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    explicit constexpr websocket_close_payload_encode_failure(
        websocket_close_payload_encode_error error) noexcept
        : error_(error) {}

    websocket_close_payload_encode_error error_;
};

class websocket_close_payload_encode_result final {
public:
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const&& = delete;

    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const&& = delete;

private:
    friend websocket_close_payload_encode_result encode_websocket_close_payload(
        websocket_close_code, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept
        : value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(
        websocket_close_payload_encode_failure failure) noexcept
        : value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(
    websocket_close_code code, std::string_view reason) noexcept;

// The protocol failure a received Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

websocket_encoded_close_payload::websocket_encoded_close_payload(
    websocket_close_code code, std::string_view reason) noexcept
    : size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    const auto code_value = code.value();
    bytes_[0] = static_cast<char>((code_value >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code_value & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(
    websocket_close_code code, std::string_view reason) noexcept {
    if (reason.size() > 123) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::reason_too_large));
    }
    if (!::ruvia::is_valid_utf8(reason)) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_reason));
    }
    return websocket_close_payload_encode_result(websocket_encoded_close_payload(code, reason));
}

std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept {
    // A malformed frame is a protocol error (close 1002); an otherwise-valid
    // frame whose reason is not valid UTF-8 is invalid payload data (close
    // 1007, RFC 6455 section 8.1).
    if (payload_value.size() == 1) {
        return websocket_protocol_failure::protocol_error;
    }
    if (payload_value.size() < 2) {
        return std::nullopt;
    }
    if (!websocket_close_code::is_valid_value(read_websocket_uint16(payload_value.data()))) {
        return websocket_protocol_failure::protocol_error;
    }
    if (!::ruvia::is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

class websocket_frame_read_result;

// One borrowed frame published by the wire reader, which owns masking, length,
// opcode/compression combinations, and Close payload validation. Payload
// storage must outlive the view.
class websocket_frame_view final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class websocket_frame_read_result;

    constexpr websocket_frame_view(
        const websocket_frame_start& start, std::string_view payload_value) noexcept
        : kind_(start.kind()),
          payload_(payload_value),
          final_(start.final()),
          compressed_(start.compressed()) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};

// Drop the bytes the previously returned frame occupied. Consuming never moves
// bytes; the buffer moves the unread tail only when a later read needs room.
// A drained buffer keeps at most websocket_retained_buffer_bytes, so a burst
// of large messages does not stay pinned to the connection.
inline void consume_websocket_read_frame(http_io_buffer& buffer, std::size_t& pending_consume) noexcept {
    if (pending_consume == 0) {
        return;
    }
    buffer.consume(std::exchange(pending_consume, 0));
    if (buffer.empty()) {
        buffer.clear_retaining_small(websocket_retained_buffer_bytes);
    }
}

class websocket_frame_need_input final {
public:
    // Bytes the frame still lacks once its header is complete; 0 while the
    // header itself is incomplete. The header length is already within the
    // message limit.
    [[nodiscard]] constexpr std::size_t missing_bytes() const noexcept {
        return missing_bytes_;
    }

private:
    friend class websocket_frame_read_result;
    explicit constexpr websocket_frame_need_input(std::size_t missing_bytes) noexcept
        : missing_bytes_(missing_bytes) {}

    std::size_t missing_bytes_;
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
// one typed protocol failure. No default frame, EOF boolean, or wire-format
// exception can coexist with another outcome; only need_input() carries a
// byte count.
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
        http_io_buffer&, std::size_t&, protocol_byte_limit, bool, bool);

    using value_type =
        std::variant<websocket_frame_need_input, websocket_frame_view, websocket_frame_read_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_frame_read_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr websocket_frame_read_result make_need_input(
        std::size_t missing_bytes = 0) noexcept {
        return websocket_frame_read_result(websocket_frame_need_input(missing_bytes));
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
// input after need_input(), while failure() carries the Close reason. A returned
// frame stays in the buffer, counted by `pending_consume`, until the next call.
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(http_io_buffer& buffer,
    std::size_t& pending_consume, protocol_byte_limit message_limit, bool permessage_deflate, bool expect_masked) {
    consume_websocket_read_frame(buffer, pending_consume);
    const auto available = buffer.size();
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    const auto first = static_cast<unsigned char>(buffer.data()[0]);
    const auto second = static_cast<unsigned char>(buffer.data()[1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start =
        decode_websocket_frame_start(first, second, permessage_deflate, expect_masked);
    if (!frame_start.has_value()) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    // RFC 6455 section 5.2 obliges the sender to use the minimal length form;
    // the frame stays unambiguous either way, so a receiver does not reject a
    // longer form (sender MUST, not recipient rule).
    if (length == 126) {
        if (available < header_size + 2) {
            return websocket_frame_read_result::make_need_input();
        }
        length = read_websocket_uint16(buffer.data() + header_size);
        header_size += 2;
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = read_websocket_uint64(
            std::span<const char, 8>(buffer.data() + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
    }

    if (is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::message_too_large);
    }
    const auto mask_bytes = expect_masked ? std::size_t{4} : std::size_t{0};
    // header_size is at most 10, so only the payload length can overflow.
    if (length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() - header_size - mask_bytes)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    const auto total_frame_bytes = header_size + mask_bytes + static_cast<std::size_t>(length);
    if (available < total_frame_bytes) {
        return websocket_frame_read_result::make_need_input(total_frame_bytes - available);
    }

    const auto mask_offset = header_size;
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
    pending_consume = total_frame_bytes;
    return websocket_frame_read_result::make_frame(*frame_start, payload_view);
}

class websocket_inbound_result;

class websocket_inbound_continue final {
private:
    friend class websocket_inbound_result;
    constexpr websocket_inbound_continue() noexcept = default;
};

class websocket_inbound_control_frame final {
public:
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_control_frame(
        websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}

    websocket_opcode opcode_;
    std::string_view payload_;
};

enum class websocket_inbound_content_encoding : std::uint8_t {
    identity,
    per_message_deflate,
};

class websocket_inbound_message final {
public:
    [[nodiscard]] constexpr const websocket_message& message() const& noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const websocket_message& message() const&& = delete;

    [[nodiscard]] constexpr websocket_inbound_content_encoding content_encoding() const noexcept {
        return content_encoding_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_message(
        websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept
        : message_(message),
          content_encoding_(content_encoding) {}

    websocket_message message_;
    websocket_inbound_content_encoding content_encoding_;
};

class websocket_inbound_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_inbound_result;

    explicit constexpr websocket_inbound_failure(websocket_protocol_failure error) noexcept
        : error_(error) {}

    websocket_protocol_failure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class websocket_inbound_result final {
public:
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const& noexcept {
        return std::get_if<websocket_inbound_continue>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const& noexcept {
        return std::get_if<websocket_inbound_control_frame>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_message* message() const& noexcept {
        return std::get_if<websocket_inbound_message>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_message* message() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const& noexcept {
        return std::get_if<websocket_inbound_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const&& = delete;

private:
    friend class websocket_inbound_assembler;

    using value_type = std::variant<websocket_inbound_continue, websocket_inbound_control_frame,
        websocket_inbound_message, websocket_inbound_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_inbound_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr websocket_inbound_result make_continue() noexcept {
        return websocket_inbound_result(websocket_inbound_continue());
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_control_frame(
        websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_inbound_result(websocket_inbound_control_frame(opcode, payload_value));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_message(
        websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept {
        return websocket_inbound_result(websocket_inbound_message(message, content_encoding));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_failure(
        websocket_protocol_failure error) noexcept {
        return websocket_inbound_result(websocket_inbound_failure(error));
    }

    value_type value_;
};

// Single owner of the websocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Every wire failure is returned as
// a typed alternative carrying the exact RFC Close reason; none is thrown.
struct websocket_inbound_idle final {};

class websocket_inbound_fragmented final {
public:
    constexpr websocket_inbound_fragmented(
        websocket_opcode opcode, websocket_inbound_content_encoding encoding) noexcept
        : opcode_(opcode),
          encoding_(encoding) {}

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr websocket_inbound_content_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    websocket_opcode opcode_;
    websocket_inbound_content_encoding encoding_;
};

class websocket_inbound_assembler final {
public:
    websocket_inbound_assembler() = default;

    // A delivered view expires at the next poll, while a fragmented message
    // remains pinned until its final continuation or connection teardown. A
    // delivered message keeps only a bounded scratch, so streams of small
    // fragmented messages do not allocate per message and large ones are not
    // pinned to the connection.
    void release_completed() noexcept {
        if (std::holds_alternative<websocket_inbound_idle>(state_)) {
            http_clear_string_retaining_small(message_, websocket_retained_buffer_bytes);
        }
    }

    [[nodiscard]] websocket_inbound_result accept(
        const websocket_frame_view& frame, protocol_byte_limit message_limit) {
        if (is_websocket_control_frame_kind(frame.kind())) {
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            return websocket_inbound_result::make_control_frame(opcode, frame.payload());
        }
        if (frame.kind() == websocket_frame_kind::continuation) {
            const auto* fragmented = std::get_if<websocket_inbound_fragmented>(&state_);
            if (fragmented == nullptr) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::protocol_error);
            }
            if (message_limit.addition_exceeds(message_.size(), frame.payload().size())) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::message_too_large);
            }
            message_.append(frame.payload().data(), frame.payload().size());
            if (!frame.final()) {
                return websocket_inbound_result::make_continue();
            }
            const auto opcode = fragmented->opcode();
            const auto encoding = fragmented->encoding();
            state_.template emplace<websocket_inbound_idle>();
            const auto message = websocket_message::borrow(opcode, std::string_view(message_));
            if (encoding == websocket_inbound_content_encoding::per_message_deflate) {
                return websocket_inbound_result::make_message(
                    message, websocket_inbound_content_encoding::per_message_deflate);
            }
            if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(message_)) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::invalid_payload_data);
            }
            return websocket_inbound_result::make_message(
                message, websocket_inbound_content_encoding::identity);
        }
        if (frame.kind() == websocket_frame_kind::text ||
            frame.kind() == websocket_frame_kind::binary) {
            if (std::get_if<websocket_inbound_fragmented>(&state_) != nullptr) {
                return websocket_inbound_result::make_failure(
                    websocket_protocol_failure::protocol_error);
            }
            // websocket_try_read_frame already bounded this first frame by the message limit.
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            if (frame.final()) {
                const auto message = websocket_message::borrow(opcode, frame.payload());
                if (frame.compressed()) {
                    return websocket_inbound_result::make_message(
                        message, websocket_inbound_content_encoding::per_message_deflate);
                }
                if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(frame.payload())) {
                    return websocket_inbound_result::make_failure(
                        websocket_protocol_failure::invalid_payload_data);
                }
                return websocket_inbound_result::make_message(
                    message, websocket_inbound_content_encoding::identity);
            }
            message_.assign(frame.payload().data(), frame.payload().size());
            state_.template emplace<websocket_inbound_fragmented>(
                opcode, frame.compressed() ? websocket_inbound_content_encoding::per_message_deflate
                                           : websocket_inbound_content_encoding::identity);
        }
        return websocket_inbound_result::make_continue();
    }

private:
    std::string message_;
    std::variant<websocket_inbound_idle, websocket_inbound_fragmented> state_;
};

}  // namespace ruvia::detail
namespace ruvia {

namespace {

enum class websocket_payload_storage : std::uint8_t {
    copy,
    borrow,
};

[[nodiscard]] constexpr websocket_termination websocket_failure_termination(
    detail::websocket_protocol_failure failure) noexcept {
    switch (failure) {
        case detail::websocket_protocol_failure::protocol_error:
            break;
        case detail::websocket_protocol_failure::invalid_payload_data:
            return websocket_termination::invalid_payload_data;
        case detail::websocket_protocol_failure::message_too_large:
            return websocket_termination::message_too_large;
    }
    return websocket_termination::protocol_error;
}

void add_saturating(std::uint64_t& total, std::size_t bytes_value) noexcept {
    const auto remaining = std::numeric_limits<std::uint64_t>::max() - total;
    total += bytes_value < remaining ? bytes_value : remaining;
}

// Where a data frame sits in its message (RFC 6455 section 5.4): only the
// first frame carries the opcode, only the last one sets FIN.
enum class websocket_fragment_position : std::uint8_t {
    whole,
    first,
    middle,
    last,
};

// UTF-8 of a text message submitted in fragments: a code point may straddle
// fragments, so the bytes of one left incomplete wait for the next fragment.
class websocket_utf8_fragments final {
public:
    [[nodiscard]] bool append(std::string_view fragment) noexcept {
        std::size_t index = 0;
        if (pending_size_ != 0) {
            const auto lead = static_cast<unsigned char>(pending_[0]);
            const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                                                                       : 2;
            while (pending_size_ < length && index < fragment.size()) {
                pending_[pending_size_++] = fragment[index++];
            }
            const auto sequence = scan_utf8_sequence(std::string_view(pending_.data(), pending_size_), 0);
            if (pending_size_ < length) {
                return sequence.length_ == pending_size_;
            }
            if (!sequence.well_formed_) {
                return false;
            }
            pending_size_ = 0;
        }
        while (index < fragment.size()) {
            const auto lead = static_cast<unsigned char>(fragment[index]);
            if (lead < 0x80) {
                ++index;
                continue;
            }
            const auto sequence = scan_utf8_sequence(fragment, index);
            if (sequence.well_formed_) {
                index += sequence.length_;
                continue;
            }
            // A valid lead and continuation bytes cut off by the fragment end.
            if (index + sequence.length_ != fragment.size() || lead < 0xC2 || lead > 0xF4) {
                return false;
            }
            for (; index < fragment.size(); ++index) {
                pending_[pending_size_++] = fragment[index];
            }
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return pending_size_ == 0;
    }

private:
    std::array<char, 4> pending_{};
    std::size_t pending_size_{0};
};

// The data message being submitted as fragments.
struct websocket_outbound_fragments final {
    websocket_opcode opcode_;
    // RSV1 went out on the first frame: every fragment continues one
    // permessage-deflate stream.
    bool compressed_;
    std::size_t size_;
    websocket_utf8_fragments utf8_;
};

}  // namespace

class websocket_connection::impl_type final {
public:
    enum class close_phase_type : std::uint8_t {
        open,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        local_close_queued,
        awaiting_peer_close,
        // A peer Close was received (or the connection failed) and the final
        // queued output still needs to be flushed before transport end. It may
        // be a Pong queued for an earlier Ping, not another Close.
        final_output_queued,
        // Client only (RFC 6455 section 7.1.1): the close handshake is complete
        // and the final output is still queued; once flushed, the server is
        // expected to close the transport first.
        handshake_output_queued,
        awaiting_peer_eof,
        transport_end_ready,
        closed,
    };

    // A null `input` gives the connection an input buffer of its own.
    impl_type(http_io_buffer* input, websocket_connection_options options)
        : input_(input != nullptr ? input : &owned_input_.emplace()),
          max_buffered_input_bytes_(options.max_buffered_input_bytes_),
          message_limit_(options.message_limit_),
          receive_message_limit_(options.receive_message_limit_.read_ceiling() < options.message_limit_.read_ceiling()
                                     ? options.receive_message_limit_
                                     : options.message_limit_),
          role_(options.role_),
          mask_key_generator_(options.mask_key_generator_),
          mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    void initialize_codecs(const websocket_connection_options& options) {
        const auto& compression = options.compression_;
        if (max_buffered_input_bytes_ == 0) {
            throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
        }
        if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
            throw std::invalid_argument("invalid WebSocket connection role");
        }
        if (compression.server_max_window_bits_.value_or(15) < 8 ||
            compression.server_max_window_bits_.value_or(15) > 15 ||
            compression.client_max_window_bits_.value_or(15) < 8 ||
            compression.client_max_window_bits_.value_or(15) > 15) {
            throw std::invalid_argument("invalid WebSocket compression window");
        }
        if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
            throw std::invalid_argument("WebSocket client connection requires a mask key generator");
        }
        validate_websocket_deflater_config(options.deflater_);
        if (compression.enabled_) {
            const bool server = role_ == websocket_connection_role::server;
            const bool send_takeover =
                !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
            const bool receive_takeover =
                !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
            const int send_window =
                (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
            const int receive_window =
                (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
            deflate_.emplace(options.shared_deflate_, options.deflater_,
                send_takeover, receive_takeover, send_window, receive_window);
        }
    }

    [[nodiscard]] std::size_t pending_input_bytes() const noexcept {
        return input_->size() - pending_consume_;
    }

    void release_consumed_input() noexcept {
        // Keep the storage: the caller may append bytes that alias the released frame.
        input_->consume(std::exchange(pending_consume_, 0));
    }

    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept {
        switch (close_phase_) {
            case close_phase_type::open:
                return websocket_liveness_mode::open;
            case close_phase_type::local_close_queued:
            case close_phase_type::awaiting_peer_close:
                return websocket_liveness_mode::awaiting_peer_close;
            case close_phase_type::handshake_output_queued:
            case close_phase_type::awaiting_peer_eof:
                return websocket_liveness_mode::awaiting_peer_eof;
            case close_phase_type::final_output_queued:
            case close_phase_type::transport_end_ready:
            case close_phase_type::closed:
                return websocket_liveness_mode::inactive;
        }
        return websocket_liveness_mode::inactive;
    }

    void append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1 = false,
        websocket_payload_storage storage = websocket_payload_storage::copy,
        websocket_fragment_position position = websocket_fragment_position::whole) {
        std::optional<std::string> owned_payload;
        if (detail::http_text_overlaps(payload_value, out_buffer_.view())) {
            // A plan may be its own append source. Snapshot it before prepare()
            // can move storage, and never borrow the temporary snapshot.
            payload_value = owned_payload.emplace(payload_value);
            storage = websocket_payload_storage::copy;
        }
        detail::websocket_frame_header_type header;
        const bool masked = role_ == websocket_connection_role::client;
        websocket_mask_key_type mask{};
        if (masked && !mask_key_generator_(mask_key_context_, mask)) {
            throw std::runtime_error("failed to generate WebSocket client mask key");
        }
        const bool borrow = storage == websocket_payload_storage::borrow && !masked;
        const bool continuation =
            position == websocket_fragment_position::middle || position == websocket_fragment_position::last;
        const bool final = position == websocket_fragment_position::whole || position == websocket_fragment_position::last;
        const auto header_size = detail::encode_websocket_frame_header(header,
            continuation ? std::uint8_t{0} : static_cast<std::uint8_t>(opcode), final, payload_value.size(), rsv1, masked);
        const auto mask_size = masked ? mask.size() : std::size_t{0};
        std::size_t additional_size = 0;
        for (const auto size : {borrowed_output_.size(), header_size, mask_size, borrow ? 0 : payload_value.size()}) {
            if (size > std::numeric_limits<std::size_t>::max() - additional_size) {
                throw std::length_error("WebSocket output frame size overflow");
            }
            additional_size += size;
        }
        // Prepare every byte before publishing anything. The appends below
        // then fit without allocation, so a failure cannot orphan a wire header.
        (void)out_buffer_.prepare(additional_size);
        // An earlier borrowed payload precedes this frame on the wire.
        out_buffer_.append(borrowed_output_);
        borrowed_output_ = {};
        out_buffer_.append(std::string_view(header.data(), header_size));
        if (masked) {
            out_buffer_.append(std::string_view(mask.data(), mask.size()));
            const auto payload_start = out_buffer_.size();
            out_buffer_.append(payload_value);
            detail::decode_masked_websocket_payload(
                out_buffer_.data() + payload_start, payload_value.size(), mask.data());
        } else if (borrow) {
            borrowed_output_ = payload_value;
        } else {
            out_buffer_.append(payload_value);
        }
    }

    void fail(std::uint16_t code) {
        if (close_phase_ == close_phase_type::open) {
            // Every protocol failure code is a valid Close code, so encoding succeeds.
            const auto payload_value = detail::encode_websocket_close_payload(websocket_close_code::from_value(code), {});
            append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
            close_phase_ = close_phase_type::final_output_queued;
            return;
        }
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = !out_buffer_.empty() || !borrowed_output_.empty()
                               ? close_phase_type::final_output_queued
                               : close_phase_type::transport_end_ready;
        }
    }

    // Both Close frames have been exchanged once the queued output (which may
    // end with a Close echo) is flushed. A server then ends the transport; a
    // client waits for the server to do so.
    [[nodiscard]] close_phase_type handshake_complete_phase() const noexcept {
        const bool output_queued = !out_buffer_.empty() || !borrowed_output_.empty();
        if (role_ == websocket_connection_role::client) {
            return output_queued ? close_phase_type::handshake_output_queued : close_phase_type::awaiting_peer_eof;
        }
        return output_queued ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
    }

    void receive_peer_close() noexcept {
        if (close_phase_ == close_phase_type::local_close_queued ||
            close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = handshake_complete_phase();
        }
    }

    void discard_input() noexcept {
        pending_consume_ = 0;
        input_->consume(input_->size());
    }

    void record_termination(websocket_termination cause) noexcept {
        if (termination_ == websocket_termination::none) {
            termination_ = cause;
        }
    }

    // The one submission behind both submit_frame() payload forms.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload_value,
        websocket_frame_options options, websocket_payload_storage storage) {
        if (close_phase_ != close_phase_type::open) {
            return websocket_frame_submit_status::not_open;
        }
        if (opcode == websocket_opcode::ping || opcode == websocket_opcode::pong) {
            // Control frames are never fragmented (RFC 6455 section 5.5).
            if (!options.final_) {
                return websocket_frame_submit_status::invalid_opcode;
            }
            if (payload_value.size() > 125) {
                return websocket_frame_submit_status::control_frame_too_large;
            }
            append_frame(opcode, payload_value, false, storage);
            return websocket_frame_submit_status::accepted;
        }
        if (opcode != websocket_opcode::text && opcode != websocket_opcode::binary) {
            return websocket_frame_submit_status::invalid_opcode;
        }
        if (outbound_fragments_.has_value() && outbound_fragments_->opcode_ != opcode) {
            return websocket_frame_submit_status::message_in_progress;
        }
        const bool first = !outbound_fragments_.has_value();
        const bool final = options.final_;
        const std::size_t uncompressed_size = payload_value.size();
        const std::size_t sent = first ? 0 : outbound_fragments_->size_;
        if (message_limit_.addition_exceeds(sent, uncompressed_size)) {
            return websocket_frame_submit_status::message_too_large;
        }
        auto utf8 = first ? websocket_utf8_fragments{} : outbound_fragments_->utf8_;
        if (opcode == websocket_opcode::text && (!utf8.append(payload_value) || (final && !utf8.complete()))) {
            return websocket_frame_submit_status::invalid_text_payload;
        }
        const bool compression_attempted =
            first ? options.compress_ && deflate_.has_value() : outbound_fragments_->compressed_;
        bool compressed = compression_attempted;
        const auto position = first ? (final ? websocket_fragment_position::whole : websocket_fragment_position::first)
                                    : (final ? websocket_fragment_position::last : websocket_fragment_position::middle);
        try {
            if (compressed) {
                outbound_deflated_.clear();
                if (position == websocket_fragment_position::whole) {
                    // A whole message is sent uncompressed unless deflate shrinks it.
                    if (deflate_->compress(payload_value, outbound_deflated_) &&
                        outbound_deflated_.size() < payload_value.size()) {
                        payload_value = outbound_deflated_;
                    } else {
                        deflate_->discard_compression();
                        compressed = false;
                    }
                } else if (deflate_->compress_fragment(payload_value, first, final, outbound_deflated_)) {
                    payload_value = outbound_deflated_;
                } else if (first) {
                    // Nothing went out yet: send this message uncompressed.
                    deflate_->discard_compression();
                    compressed = false;
                } else {
                    throw std::runtime_error("failed to compress a WebSocket message fragment");
                }
            }
            // A compressed payload lives in outbound_deflated_, which the next
            // message reuses, so only the caller's own bytes can be borrowed.
            append_frame(opcode, payload_value, first && compressed,
                compressed ? websocket_payload_storage::copy : storage, position);
        } catch (...) {
            if (compression_attempted) {
                http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
            }
            if (compressed) {
                // The peer never received a first frame's trial, so a later
                // message must not refer to its dictionary. Earlier fragments
                // already advanced the peer's inflater, so a later fragment
                // cannot be retried; nor can the first one once resetting its
                // trial fails.
                bool reusable = first;
                if (first) {
                    try {
                        deflate_->discard_compression();
                    } catch (...) {
                        reusable = false;
                    }
                }
                if (!reusable) {
                    close_phase_ = close_phase_type::transport_end_ready;
                    record_termination(websocket_termination::aborted);
                }
            }
            throw;
        }
        if (compression_attempted) {
            // The frame owns a copy now; keep only a bounded scratch.
            http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
        }
        if (final) {
            outbound_fragments_.reset();
        } else if (first) {
            outbound_fragments_.emplace(websocket_outbound_fragments{opcode, compressed, uncompressed_size, utf8});
        } else {
            outbound_fragments_->size_ = sent + uncompressed_size;
            outbound_fragments_->utf8_ = utf8;
        }
        return websocket_frame_submit_status::accepted;
    }

    std::optional<http_io_buffer> owned_input_;
    http_io_buffer* input_;
    const std::size_t max_buffered_input_bytes_;
    protocol_byte_limit message_limit_;
    protocol_byte_limit receive_message_limit_;
    // Bytes of the last returned frame, consumed by the next next_event().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    http_io_buffer out_buffer_;
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    detail::websocket_inbound_assembler assembler_;

    std::optional<detail::websocket_deflate> deflate_;
    std::string inbound_inflated_;
    std::string outbound_deflated_;
    std::optional<websocket_outbound_fragments> outbound_fragments_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
    websocket_termination termination_{websocket_termination::none};
    std::uint64_t bytes_received_{0};
    std::uint64_t bytes_sent_{0};
};

websocket_connection::websocket_connection(websocket_connection_options options)
    : impl_(std::make_unique<impl_type>(nullptr, options)) {}

websocket_connection::websocket_connection(http_io_buffer& input, websocket_connection_options options)
    : impl_(std::make_unique<impl_type>(&input, options)) {}

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    const auto liveness = impl_->liveness_mode();
    if (liveness == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (liveness == websocket_liveness_mode::awaiting_peer_eof) {
        // Bytes after the close handshake carry nothing; the client keeps
        // reading only to observe the server's EOF.
        return websocket_feed_status::accepted;
    }
    if (impl_->input_->size() > impl_->max_buffered_input_bytes_ ||
        input.size() > impl_->max_buffered_input_bytes_ - impl_->input_->size()) {
        if (impl_->pending_input_bytes() > impl_->max_buffered_input_bytes_ ||
            input.size() > impl_->max_buffered_input_bytes_ - impl_->pending_input_bytes()) {
            return websocket_feed_status::backpressured;
        }
        // The bound covers bytes awaiting next_event(), not the frame already
        // returned; otherwise a frame that fits could never arrive. append()
        // copies an `input` aliasing the buffer before it moves any byte.
        impl_->release_consumed_input();
    }
    impl_->input_->append(input);
    return websocket_feed_status::accepted;
}

std::optional<websocket_event> websocket_connection::next_event() & {
    using close_phase_type = impl_type::close_phase_type;
    try {
        impl_->assembler_.release_completed();
        // The last delivered message expires now. Its size is peer-controlled up
        // to the message limit, so only a bounded buffer stays for the next one.
        http_clear_string_retaining_small(
            impl_->inbound_inflated_, websocket_retained_buffer_bytes);
        impl_->needed_input_bytes_ = 0;
        if (impl_->close_phase_ == close_phase_type::final_output_queued ||
            impl_->close_phase_ == close_phase_type::transport_end_ready ||
            impl_->close_phase_ == close_phase_type::closed) {
            return websocket_event::make_transport_end();
        }
        if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
            // Queued output may still borrow input bytes: drop them once flushed.
            return std::nullopt;
        }
        if (impl_->close_phase_ == close_phase_type::awaiting_peer_eof) {
            impl_->discard_input();
            return std::nullopt;
        }

        const auto protocol_failure_event = [this](detail::websocket_protocol_failure failure) {
            const auto close_code = detail::websocket_protocol_failure_close_code(failure);
            impl_->fail(close_code);
            impl_->record_termination(websocket_failure_termination(failure));
            return websocket_event::protocol_error(close_code);
        };

        for (;;) {
            const auto read = detail::websocket_try_read_frame(*impl_->input_, impl_->pending_consume_,
                impl_->receive_message_limit_, impl_->deflate_.has_value(),
                impl_->role_ == websocket_connection_role::server);
            if (const auto* need_input = read.need_input()) {
                impl_->needed_input_bytes_ = need_input->missing_bytes();
                return std::nullopt;
            }
            if (const auto* failure = read.failure()) {
                return protocol_failure_event(failure->error());
            }

            add_saturating(impl_->bytes_received_, impl_->pending_consume_);
            const auto& frame = *read.frame();
            const auto inbound = impl_->assembler_.accept(frame, impl_->receive_message_limit_);
            if (const auto* failure = inbound.failure()) {
                return protocol_failure_event(failure->error());
            }
            if (inbound.continue_reading() != nullptr) {
                continue;
            }
            if (const auto* control = inbound.control_frame()) {
                const auto payload_value = control->payload();
                if (control->opcode() == websocket_opcode::ping) {
                    // RFC 6455 requires Pong until a peer Close has arrived. A Pong
                    // is a control frame, so it remains legal while a locally
                    // initiated Close waits for its peer response.
                    impl_->append_frame(websocket_opcode::pong, payload_value);
                    return websocket_event::ping(payload_value);
                }
                if (control->opcode() == websocket_opcode::pong) {
                    return websocket_event::pong(payload_value);
                }
                // Close: the frame reader admits no other control opcode.
                std::uint16_t code = 1005;
                if (payload_value.size() >= 2) {
                    code = detail::read_websocket_uint16(payload_value.data());
                }
                const auto reason = payload_value.size() > 2 ? payload_value.substr(2) : std::string_view{};
                if (impl_->close_phase_ == close_phase_type::open) {
                    impl_->append_frame(websocket_opcode::close, payload_value);
                    impl_->close_phase_ = impl_->handshake_complete_phase();
                    impl_->record_termination(websocket_termination::peer_close);
                } else {
                    impl_->receive_peer_close();
                    impl_->record_termination(websocket_termination::local_close);
                }
                return websocket_event::close(code, reason);
            }

            const auto& inbound_message = *inbound.message();
            const auto& message = inbound_message.message();
            if (inbound_message.content_encoding() == detail::websocket_inbound_content_encoding::identity) {
                if (impl_->close_phase_ != close_phase_type::open) {
                    impl_->assembler_.release_completed();
                    continue;
                }
                return websocket_event::message(message.opcode(), message.payload());
            }

            // decompress() only appends, so the buffer must be emptied per MESSAGE, not
            // per next_event(): one next_event() drains several frames, and a message suppressed
            // during the closing handshake (below) returns via `continue` with its bytes
            // still here. Inheriting them would make the next message's UTF-8 check read
            // the concatenation, and would charge its decompression-bomb limit for both.
            impl_->inbound_inflated_.clear();
            // Only a negotiated deflate admits RSV1, so a compressed message implies deflate_.
            const auto inflate_result = impl_->deflate_->decompress(
                message.payload(), impl_->inbound_inflated_, impl_->receive_message_limit_);
            if (inflate_result == websocket_inflate_result::too_large) {
                return protocol_failure_event(detail::websocket_protocol_failure::message_too_large);
            }
            if (inflate_result != websocket_inflate_result::ok) {
                return protocol_failure_event(detail::websocket_protocol_failure::protocol_error);
            }
            const std::string_view view = impl_->inbound_inflated_;
            if (message.opcode() == websocket_opcode::text && !is_valid_utf8(view)) {
                return protocol_failure_event(detail::websocket_protocol_failure::invalid_payload_data);
            }
            if (impl_->close_phase_ != close_phase_type::open) {
                impl_->assembler_.release_completed();
                http_clear_string_retaining_small(
                    impl_->inbound_inflated_, websocket_retained_buffer_bytes);
                continue;
            }
            return websocket_event::message(message.opcode(), view);
        }
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined. The transport has not ended yet, so the plan
        // and abort() must still tell the driver to end it, as after EOF.
        if (impl_->close_phase_ != close_phase_type::closed) {
            impl_->close_phase_ = close_phase_type::transport_end_ready;
        }
        impl_->record_termination(websocket_termination::aborted);
        throw;
    }
}

std::size_t websocket_connection::needed_input_bytes() const noexcept {
    return impl_->needed_input_bytes_;
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (impl_->close_phase_ == close_phase_type::transport_end_ready ||
        impl_->close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, {},
            impl_->close_phase_ == close_phase_type::transport_end_ready
                ? websocket_transport_disposition::end_transport
                : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value = impl_->out_buffer_.view();
    const auto disposition = impl_->close_phase_ == close_phase_type::final_output_queued
                                 ? websocket_transport_disposition::end_transport
                                 : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, impl_->borrowed_output_, disposition);
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (bytes_value == 0 && (impl_->close_phase_ == close_phase_type::transport_end_ready ||
                                impl_->close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto owned = impl_->out_buffer_.size();
    if (bytes_value > owned && bytes_value - owned > impl_->borrowed_output_.size()) {
        return websocket_output_consume_status::out_of_range;
    }
    add_saturating(impl_->bytes_sent_, bytes_value);
    if (bytes_value < owned) {
        impl_->out_buffer_.consume(bytes_value);
        return websocket_output_consume_status::pending;
    }
    // The transport has written every owned byte, so a large frame's storage
    // can go back to the resource instead of staying with the connection.
    impl_->out_buffer_.clear_retaining_small(websocket_retained_buffer_bytes);
    if (bytes_value - owned < impl_->borrowed_output_.size()) {
        impl_->borrowed_output_.remove_prefix(bytes_value - owned);
        return websocket_output_consume_status::pending;
    }
    impl_->borrowed_output_ = {};
    if (impl_->close_phase_ == close_phase_type::local_close_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (impl_->close_phase_ == close_phase_type::final_output_queued) {
        impl_->close_phase_ = close_phase_type::transport_end_ready;
    } else if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_eof;
    }
    return websocket_output_consume_status::drained;
}

void websocket_connection::commit_transport_end() noexcept {
    if (impl_->close_phase_ == impl_type::close_phase_type::transport_end_ready) {
        impl_->close_phase_ = impl_type::close_phase_type::closed;
    }
}

void websocket_connection::notify_transport_eof() noexcept {
    if (impl_->close_phase_ != impl_type::close_phase_type::closed) {
        impl_->close_phase_ = impl_type::close_phase_type::transport_end_ready;
    }
    impl_->record_termination(websocket_termination::eof);
}

websocket_abort_disposition websocket_connection::abort(websocket_termination cause) noexcept {
    impl_->record_termination(cause == websocket_termination::none ? websocket_termination::aborted : cause);
    if (impl_->close_phase_ == impl_type::close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    impl_->close_phase_ = impl_type::close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->liveness_mode();
}

websocket_termination websocket_connection::termination() const noexcept {
    return impl_->termination_;
}

std::uint64_t websocket_connection::bytes_received() const noexcept {
    return impl_->bytes_received_;
}

std::uint64_t websocket_connection::bytes_sent() const noexcept {
    return impl_->bytes_sent_;
}

websocket_frame_submit_status websocket_connection::submit_frame(
    websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value, options, websocket_payload_storage::copy);
}

websocket_frame_submit_status websocket_connection::submit_frame(
    websocket_opcode opcode, websocket_borrowed_payload payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value.bytes(), options, websocket_payload_storage::borrow);
}

websocket_close_submit_status websocket_connection::submit_close(
    websocket_close_code code, std::string_view reason) {
    using close_phase_type = impl_type::close_phase_type;
    if (impl_->close_phase_ == close_phase_type::closed) {
        return websocket_close_submit_status::closed;
    }
    if (impl_->close_phase_ != close_phase_type::open) {
        return websocket_close_submit_status::already_closing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (impl_->role_ == websocket_connection_role::server && code.value() == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = detail::encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case detail::websocket_close_payload_encode_error::invalid_reason:
                return websocket_close_submit_status::invalid_reason;
            case detail::websocket_close_payload_encode_error::reason_too_large:
                return websocket_close_submit_status::reason_too_large;
        }
    }
    impl_->append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
    impl_->close_phase_ = close_phase_type::local_close_queued;
    return websocket_close_submit_status::accepted;
}

}  // namespace ruvia

namespace ruvia {

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        delete codec;
    }
    delete inflater_;
}

bool websocket_deflate_codecs::deflate_message(
    std::string_view payload, std::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>(
        (window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = detail::acquire_codec(slot, compression_level,
        detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level,
        detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const detail::codec_reset_guard reset_guard(slot);
    return detail::websocket_deflate_message(*codec, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(
    std::string_view payload, std::string& out, protocol_byte_limit message_limit) {
    auto* codec = detail::acquire_codec(inflater_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const detail::codec_reset_guard reset_guard(inflater_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::vector<http_header> copy_websocket_response_headers(
    std::span<const http_header_view> headers, std::string_view subprotocol,
    std::string_view extension) {
    // Account for the whole emitted head: the fixed 101 prefix, the accept
    // key line, the negotiated and application fields, and the terminating
    // empty line.
    http_field_section_budget size;
    (void)size.add(websocket_switching_protocols_head_prefix.size() + websocket_accept_key_type{}.size() +
                   std::string_view("\r\n").size());
    (void)size.add(std::string_view("\r\n").size());
    if (!subprotocol.empty() && !size.add_field("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    if (!extension.empty() && !size.add_field("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    static constexpr std::string_view sec_websocket_prefix = "sec-websocket-";
    for (const auto& header : headers) {
        const auto name = header.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        // Every Sec-WebSocket-* name is handshake-owned, including ones this
        // implementation does not know yet.
        const auto kind = classify_http_field(name);
        const bool is_sec_websocket =
            name.size() >= sec_websocket_prefix.size() &&
            http_ascii_equals_ignore_case(
                name.substr(0, sec_websocket_prefix.size()), sec_websocket_prefix);
        if ((kind != http_field_kind::host &&
                kind != http_field_kind::expect &&
                is_protocol_managed_request_field(kind, name)) ||
            is_sec_websocket) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add_field(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
    }
    std::vector<http_header> result;
    result.reserve(headers.size());
    for (const auto& header : headers) {
        result.push_back(
            http_header::copy_with_lowercase_name(header.name(), header.value()));
    }
    return result;
}

void skip_websocket_extension_ows(std::string_view value, std::size_t& cursor_value) noexcept {
    while (cursor_value < value.size() && (value[cursor_value] == ' ' || value[cursor_value] == '\t')) {
        ++cursor_value;
    }
}

[[nodiscard]] bool consume_websocket_extension_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    const auto start = cursor_value;
    while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
        ++cursor_value;
    }
    return cursor_value != start;
}

// RFC 6455 section 4.3 narrows quoted-string extension values: after
// quoted-pair unescaping, the result still has to be a nonempty token.
[[nodiscard]] bool consume_websocket_extension_quoted_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    http_quoted_string_cursor quoted(value.substr(cursor_value));
    bool decoded_any = false;
    unsigned char byte = 0;
    while (quoted.next(byte)) {
        if (!is_http_token_char(byte)) {
            return false;
        }
        decoded_any = true;
    }
    if (!quoted.closed()) {
        return false;
    }
    cursor_value += quoted.consumed();
    return decoded_any;
}

}  // namespace

bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept {
    std::size_t cursor_value = 0;
    while (true) {
        skip_websocket_extension_ows(value, cursor_value);
        // RFC 2616's #rule permits null comma-list elements, but 1#extension
        // still requires at least one real extension across the logical field.
        while (cursor_value < value.size() && value[cursor_value] == ',') {
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
        }
        if (cursor_value == value.size()) {
            return true;
        }
        if (!consume_websocket_extension_token(value, cursor_value)) {
            return false;
        }
        has_extension = true;

        while (true) {
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return true;
            }
            if (value[cursor_value] == ',') {
                ++cursor_value;
                break;
            }
            if (value[cursor_value] != ';') {
                return false;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size() || value[cursor_value] != '=') {
                continue;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return false;
            }
            if (value[cursor_value] == '"') {
                if (!consume_websocket_extension_quoted_token(value, cursor_value)) {
                    return false;
                }
            } else if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
        }
    }
}

std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }
    if (offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept {
    if (supported.empty()) {
        return {};
    }
    websocket_subprotocol_set offered;
    if (!http_request_access::visit_known_headers(request, http_field_kind::sec_websocket_protocol,
            [&offered](std::string_view value) noexcept { return offered.append_offered_list(value); })) {
        return {};
    }
    return choose_websocket_subprotocol(offered, supported);
}

bool websocket_request_header_validator::observe_header(
    http_field_kind kind, std::string_view value) noexcept {
    if (kind == http_field_kind::sec_websocket_version) {
        version_ = value;
        if (version_present_) {
            valid_ = false;
        }
        version_present_ = true;
    } else if (kind == http_field_kind::sec_websocket_protocol) {
        protocols_present_ = true;
        if (valid_ && !(role_ == http_field_list_role::recipient ? protocols_.append_offered_list(value)
                                                                 : protocols_.append_list(value))) {
            valid_ = false;
        }
    } else if (kind == http_field_kind::sec_websocket_extensions) {
        extensions_present_ = true;
        if (valid_ && !append_websocket_extension_list(value, has_extension_)) {
            valid_ = false;
        }
    }
    return valid_;
}

bool websocket_request_header_validator::offers_valid() const noexcept {
    return valid_ && version_present_ && (!protocols_present_ || !protocols_.empty()) &&
           (!extensions_present_ || has_extension_);
}

bool websocket_request_header_validator::client_offers_valid() const noexcept {
    return offers_valid() && version_ == "13";
}

websocket_handshake_validation_result websocket_request_header_validator::finish(
    websocket_handshake_validation_options options) const noexcept {
    if (!offers_valid()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (http_trim_ows(version_) != "13") {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    if (options.require_subprotocol_ &&
        choose_websocket_subprotocol(protocols_, options.supported_subprotocols_).empty()) {
        return websocket_handshake_validation_result::make_unsupported_subprotocol();
    }
    return websocket_handshake_validation_result::make_accepted();
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol,
    websocket_compression compression, std::span<const http_header_view> response_headers_value)
    : subprotocol_(subprotocol),
      compression_(compression),
      extensions_(compression),
      response_headers_(copy_websocket_response_headers(
          response_headers_value, subprotocol, extensions_.view())) {}

websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, const websocket_server_handshake_options& options) {
    return websocket_server_negotiation(choose_websocket_subprotocol(request, options.supported_subprotocols_),
        websocket_negotiate_permessage_deflate(request, options.deflate_), options.response_headers_);
}

websocket_handshake_validation_result validate_extended_connect_websocket_headers(
    const http_request& request, websocket_handshake_validation_options options) noexcept {
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!validator.observe_header(http_request_access::header_kind(request, index), headers[index].value())) {
            return validator.finish(options);
        }
    }
    return validator.finish(options);
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options) {
    detail::websocket_accept_key_type accept;
    detail::encode_websocket_accept(
        accept, detail::http_request_access::known_header(request, detail::http_field_kind::sec_websocket_key));
    auto negotiation = detail::make_websocket_server_negotiation(request, options);
    return websocket_server_handshake(accept, std::move(negotiation).take_subprotocol(),
        negotiation.compression(), std::move(negotiation).take_response_headers());
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::uint64_t max_sha1_message_bytes = (std::numeric_limits<std::uint64_t>::max)() / 8;

[[nodiscard]] std::array<std::uint8_t, 20> sha1(
    std::string_view first, std::string_view second) noexcept {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::array<std::uint8_t, 64> block{};
    const auto total_size = first.size() + second.size();
    const std::uint64_t total_bits = static_cast<std::uint64_t>(total_size) * 8U;
    std::size_t offset = 0;

    const auto byte_at = [first, second](std::size_t index) noexcept {
        return index < first.size() ? static_cast<std::uint8_t>(first[index])
                                    : static_cast<std::uint8_t>(second[index - first.size()]);
    };

    const auto process = [&](const std::array<std::uint8_t, 64>& data) noexcept {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(data[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const auto temp = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    while (total_size - offset >= block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = byte_at(offset + i);
        }
        process(block);
        offset += block.size();
    }
    block.fill(0);
    const auto remaining = total_size - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = byte_at(offset + i);
    }
    block[remaining] = 0x80;
    if (remaining >= 56) {
        process(block);
        block.fill(0);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    process(block);

    std::array<std::uint8_t, 20> digest{};
    const std::array words{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(words[i] & 0xFF);
    }
    return digest;
}

}  // namespace

void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key) {
    // Check the raw view before trimming: http_trim_ows() must inspect the last
    // byte, and callers of this detail helper are not otherwise required to
    // prove that a view's advertised size is backed by a full object. The
    // SHA-1 length field is measured in bits and is only 64 bits wide.
    if (key.size() > (std::numeric_limits<std::size_t>::max)() - websocket_guid.size() ||
        key.size() > max_sha1_message_bytes - websocket_guid.size()) {
        throw std::length_error("WebSocket accept input is too large");
    }
    key = http_trim_ows(key);
    const auto digest = sha1(key, websocket_guid);
    http_base64_encode(output.data(), std::span<const std::uint8_t>(digest));
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A Sec-WebSocket-Key is the canonical padded base64 of a 16-byte nonce
// (RFC 6455 section 4.1), so exactly 24 characters.
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(
    std::string_view key) noexcept {
    key = http_trim_ows(key);
    std::array<std::uint8_t, 16> nonce{};
    if (key.size() != 24) {
        return std::nullopt;
    }
    const auto decoded =
        http_base64_decode(key, nonce, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded || *decoded != nonce.size()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake",
                "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields.
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_version",
                "unsupported WebSocket version");
        case kind_type::unsupported_subprotocol:
            return http_protocol_error(http_status::bad_request,
                "unsupported_websocket_subprotocol", "no supported WebSocket subprotocol offered");
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(const http_request& request,
    const http1_request_body_plan& body_plan, websocket_handshake_validation_options options) noexcept {
    if (request.known_method() != http_known_method::get ||
        request.protocol_version() != http_protocol_version::http11 ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    detail::websocket_request_header_validator websocket_headers(detail::http_field_list_role::recipient);
    std::string_view key;
    std::size_t key_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(
                    header_value.value(), detail::http_connection_field_role::request_recipient) !=
                detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient,
                    [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                        if (protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                            websocket_upgrade = true;
                        }
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::sec_websocket_key) {
            key = header_value.value();
            ++key_count;
        } else if (!websocket_headers.observe_header(kind, header_value.value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }

    if (!connection_options.upgrade() || !upgrade_protocols.has_protocol() || !websocket_upgrade ||
        key_count != 1 || !detail::decode_websocket_key(key).has_value()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return websocket_headers.finish(options);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using error_type = websocket_client_handshake_error;
std::optional<websocket_compression> parse_response_extension(std::string_view text, const websocket_client_deflate_offer& deflate) {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_) {
        return {};
    }
    const auto params = detail::parse_permessage_deflate_params(text);
    if (!params.has_value()) {
        return {};
    }
    // A response must give client_max_window_bits a value, and may only send it
    // when the offer included it (RFC 7692 section 7.1.2.2).
    if (params->client_max_window_bits_present_ &&
        (!params->client_max_window_bits_.has_value() ||
            (!deflate.offer_client_max_window_bits_ && !offer.client_max_window_bits_))) {
        return {};
    }
    websocket_compression result_value{.enabled_ = true,
        .server_no_context_takeover_ = params->server_no_context_takeover_,
        // The client's own takeover opt-out binds its deflater even when the server does not echo it.
        .client_no_context_takeover_ = offer.client_no_context_takeover_ || params->client_no_context_takeover_,
        .server_max_window_bits_ = params->server_max_window_bits_,
        .client_max_window_bits_ = params->client_max_window_bits_};
    if (offer.server_no_context_takeover_ && !result_value.server_no_context_takeover_) {
        return {};
    }
    if (offer.server_max_window_bits_ && (!result_value.server_max_window_bits_ || *result_value.server_max_window_bits_ > *offer.server_max_window_bits_)) {
        return {};
    }
    // An offered client window binds this client even when the response omits
    // it or selects a larger one (RFC 7692 section 7.1.2.2).
    if (offer.client_max_window_bits_ &&
        (!result_value.client_max_window_bits_ || *result_value.client_max_window_bits_ > *offer.client_max_window_bits_)) {
        result_value.client_max_window_bits_ = offer.client_max_window_bits_;
    }

    return result_value;
}
[[nodiscard]] bool subprotocol_offered(
    std::string_view offered_list, std::string_view selected) noexcept {
    if (selected.empty()) {
        return false;
    }
    return !detail::http_find_header_token(offered_list, [selected](std::string_view token) noexcept {
        return token == selected;
    }).empty();
}

std::variant<websocket_client_negotiation_result_view, error_type> validate(std::span<const http_header> fields_value,
    std::string_view offered_subprotocols, const websocket_client_deflate_offer& offer, bool extended) {
    websocket_client_negotiation_result_view result;
    std::size_t selected = 0;
    bool extensions_present = false;
    bool saw_extension = false;
    for (const auto& field : fields_value) {
        const auto kind = detail::classify_http_field(field.name());
        if (kind == detail::http_field_kind::sec_websocket_protocol) {
            ++selected;
            result.selected_subprotocol_ = http_trim_ows(field.value());
            if (selected > 1 || !subprotocol_offered(offered_subprotocols, result.selected_subprotocol_)) {
                return error_type::subprotocol;
            }
        } else if (kind == detail::http_field_kind::sec_websocket_extensions) {
            extensions_present = true;
            bool valid_line = true;
            http_visit_comma_separated_quoted_field_items(
                field.value(), [&](std::string_view item) noexcept {
                    if (item.empty()) {
                        return true;
                    }
                    saw_extension = true;
                    if (result.compression_.enabled_) {
                        valid_line = false;
                        return false;
                    }
                    auto negotiation = parse_response_extension(item, offer);
                    if (!negotiation) {
                        valid_line = false;
                        return false;
                    }
                    result.compression_ = *negotiation;
                    return true;
                });
            if (!valid_line) {
                return error_type::extensions;
            }
        } else if (extended && detail::is_forbidden_http_binary_response_field(field.name())) {
            // Connection-specific fields are malformed in HTTP/2 (RFC 9113
            // section 8.2.2). Sec-WebSocket-Accept is superseded by :protocol
            // and therefore not processed (RFC 8441 section 5).
            return error_type::forbidden_field;
        }
    }
    if (extensions_present && !saw_extension) {
        return error_type::extensions;
    }
    return result;
}

inline constexpr std::string_view version_header_name = "sec-websocket-version";
inline constexpr std::string_view version_storage = "sec-websocket-version13";
inline constexpr std::string_view protocol_header_name = "sec-websocket-protocol";
inline constexpr std::string_view extensions_header_name = "sec-websocket-extensions";
inline constexpr std::string_view client_window_bits_suffix = "; client_max_window_bits";

[[nodiscard]] const std::string_view* static_deflate_extension_storage(
    const websocket_client_deflate_offer& deflate) noexcept {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_ || offer.server_max_window_bits_.has_value() || offer.client_max_window_bits_.has_value()) {
        return nullptr;
    }
    static constexpr std::array<std::string_view, 8> table{
        "sec-websocket-extensionspermessage-deflate",
        "sec-websocket-extensionspermessage-deflate; client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; client_max_window_bits",
    };
    const std::size_t index = (offer.server_no_context_takeover_ ? 4U : 0U) |
                              (offer.client_no_context_takeover_ ? 2U : 0U) |
                              (deflate.offer_client_max_window_bits_ ? 1U : 0U);
    return &table[index];
}

// The validated request-field plan of one client configuration.
struct client_offer_plan final {
    // Bytes of the comma-joined Sec-WebSocket-Protocol value; zero without subprotocols.
    std::size_t subprotocol_bytes_{0};
    // Name-prefixed static storage of a parameterless deflate offer.
    const std::string_view* static_extension_{nullptr};
    // A deflate offer carrying window parameters.
    std::optional<detail::websocket_compression_extension> encoded_extension_;
    // The offer lists client_max_window_bits without a value after encoded_extension_.
    bool client_window_bits_suffix_{false};
};

[[nodiscard]] client_offer_plan make_client_offer_plan(const websocket_client_negotiation_config_view& config) {
    const auto& offer = config.deflate_.compression_;
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(offer.server_max_window_bits_) || !valid_window(offer.client_max_window_bits_) ||
        (offer.client_max_window_bits_ && !config.deflate_.offer_client_max_window_bits_)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    validate_websocket_deflater_config(config.deflate_.deflater_);
    client_offer_plan plan;
    websocket_subprotocol_set seen;
    for (const auto protocol_view : config.subprotocols_) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (plan.subprotocol_bytes_ != 0) {
            if (plan.subprotocol_bytes_ > max_http_header_bytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            plan.subprotocol_bytes_ += 2;
        }
        if (protocol_view.size() > max_http_header_bytes - plan.subprotocol_bytes_) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        plan.subprotocol_bytes_ += protocol_view.size();
    }
    // The generated Sec-WebSocket-* fields alone must fit the field section;
    // the handshake owner budgets the application headers it accepts.
    http_field_section_budget budget;
    auto add = [&budget](std::string_view name, std::size_t value_bytes) {
        if (!budget.add_field(name.size(), value_bytes)) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
    };
    add(version_header_name, 2);
    if (plan.subprotocol_bytes_ != 0) {
        add(protocol_header_name, plan.subprotocol_bytes_);
    }
    if (offer.enabled_) {
        plan.static_extension_ = static_deflate_extension_storage(config.deflate_);
        if (plan.static_extension_ != nullptr) {
            add(extensions_header_name, plan.static_extension_->size() - extensions_header_name.size());
        } else {
            const auto& encoded = plan.encoded_extension_.emplace(offer);
            plan.client_window_bits_suffix_ =
                config.deflate_.offer_client_max_window_bits_ && !offer.client_max_window_bits_;
            add(extensions_header_name,
                encoded.view().size() + (plan.client_window_bits_suffix_ ? client_window_bits_suffix.size() : 0));
        }
    }
    return plan;
}

[[nodiscard]] http_header make_subprotocol_field(
    std::span<const std::string_view> subprotocols, std::size_t joined_bytes) {
    if (subprotocols.size() == 1U) {
        return http_header(protocol_header_name, subprotocols.front());
    }
    static constexpr std::size_t inline_joined_capacity = 256;
    if (joined_bytes <= inline_joined_capacity) {
        std::array<char, inline_joined_capacity> joined_buffer;
        std::size_t offset = 0;
        for (const auto protocol_view : subprotocols) {
            if (offset != 0) {
                joined_buffer[offset++] = ',';
                joined_buffer[offset++] = ' ';
            }
            std::memcpy(joined_buffer.data() + offset, protocol_view.data(), protocol_view.size());
            offset += protocol_view.size();
        }
        return http_header(protocol_header_name, std::string_view(joined_buffer.data(), offset));
    }
    std::string joined;
    joined.reserve(joined_bytes);
    for (const auto protocol_view : subprotocols) {
        if (!joined.empty()) {
            joined.append(", ");
        }
        joined.append(protocol_view);
    }
    return http_header(protocol_header_name, joined);
}

// The static offer (a private, storage-borrowing http_header) is built by the
// constructor; this handles the encoded offers that need owned storage.
[[nodiscard]] http_header make_extension_field(const client_offer_plan& plan) {
    const auto encoded = plan.encoded_extension_->view();
    if (!plan.client_window_bits_suffix_) {
        return http_header(extensions_header_name, encoded);
    }
    std::array<char, 128> offer_buffer;
    std::memcpy(offer_buffer.data(), encoded.data(), encoded.size());
    std::memcpy(offer_buffer.data() + encoded.size(), client_window_bits_suffix.data(), client_window_bits_suffix.size());
    return http_header(extensions_header_name,
        std::string_view(offer_buffer.data(), encoded.size() + client_window_bits_suffix.size()));
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    (void)make_client_offer_plan(config);
}
websocket_client_negotiation::websocket_client_negotiation(websocket_client_negotiation_config_view config)
    : deflate_(config.deflate_) {
    const auto plan = make_client_offer_plan(config);
    fields_[field_count_++] = http_header(version_storage.data(), version_header_name.size(), 2);
    if (plan.subprotocol_bytes_ != 0) {
        fields_[field_count_] = make_subprotocol_field(config.subprotocols_.view(), plan.subprotocol_bytes_);
        offered_subprotocols_ = fields_[field_count_++].value();
    }
    if (config.deflate_.compression_.enabled_) {
        fields_[field_count_++] = plan.static_extension_ != nullptr
                                      ? http_header(plan.static_extension_->data(), extensions_header_name.size(),
                                            plan.static_extension_->size() - extensions_header_name.size())
                                      : make_extension_field(plan);
    }
}
std::variant<websocket_client_negotiation_result_view, error_type>
websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return detail::validate_websocket_client_selection(*this, fields_value, false);
}

std::variant<websocket_client_negotiation_result_view, error_type> validate_extended_connect_websocket_client_response(
    const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open) {
    if (!head.status().is_successful()) {
        return error_type::response_status;
    }
    if (!stream_open) {
        return error_type::stream_closed;
    }
    return detail::validate_websocket_client_selection(negotiation, head.headers(), true);
}

namespace detail {
std::variant<websocket_client_negotiation_result_view, error_type> validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended) {
    return validate(fields, negotiation.offered_subprotocols_, negotiation.deflate_, extended);
}
}  // namespace detail
}  // namespace ruvia
