#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "http_protocol.h"
#include "websocket_connection.h"
#include "websocket_deflate.h"
#include "websocket_frame.h"

namespace ruvia::detail {

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

    constexpr websocket_inbound_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept :
        opcode_(opcode),
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

    constexpr websocket_inbound_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept :
        message_(message),
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

    explicit constexpr websocket_inbound_failure(websocket_protocol_failure error) noexcept :
        error_(error) {}

    websocket_protocol_failure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives.
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

    using value_type = std::variant<websocket_inbound_continue, websocket_inbound_control_frame, websocket_inbound_message, websocket_inbound_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_inbound_result(alternative_type alternative) noexcept :
        value_(alternative) {}

    [[nodiscard]] static constexpr websocket_inbound_result make_continue() noexcept {
        return websocket_inbound_result(websocket_inbound_continue());
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_inbound_result(websocket_inbound_control_frame(opcode, payload_value));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept {
        return websocket_inbound_result(websocket_inbound_message(message, content_encoding));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_failure(websocket_protocol_failure error) noexcept {
        return websocket_inbound_result(websocket_inbound_failure(error));
    }

    value_type value_;
};

// Single owner of the websocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation.
struct websocket_inbound_idle final {};

class websocket_inbound_fragmented final {
public:
    constexpr websocket_inbound_fragmented(websocket_opcode opcode, websocket_inbound_content_encoding encoding) noexcept :
        opcode_(opcode),
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

    void release_completed(websocket_receive_storage& storage) noexcept;

    [[nodiscard]] bool fragmented() const noexcept {
        return std::holds_alternative<websocket_inbound_fragmented>(state_);
    }

    [[nodiscard]] websocket_inbound_result accept(const websocket_frame_view& frame, protocol_byte_limit message_limit, websocket_receive_storage& storage);

private:
    std::string message_;
    std::variant<websocket_inbound_idle, websocket_inbound_fragmented> state_;
};

}  // namespace ruvia::detail
