#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>

// Building the payload an endpoint sends in a Close frame: a validated status code
// followed by an optional UTF-8 reason, capped at the 125-byte control-frame
// limit. Encoding either yields the bytes or one typed reason it could not.

namespace ruvia::detail {

enum class websocket_close_payload_encode_error : std::uint8_t {
    invalid_code,
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
        std::uint16_t, std::string_view) noexcept;

    websocket_encoded_close_payload(std::uint16_t code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_{};
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
        std::uint16_t, std::string_view) noexcept;

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
        std::uint16_t, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept
        : value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(
        websocket_close_payload_encode_failure failure) noexcept
        : value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(
    std::uint16_t code, std::string_view reason) noexcept;
}  // namespace ruvia::detail
