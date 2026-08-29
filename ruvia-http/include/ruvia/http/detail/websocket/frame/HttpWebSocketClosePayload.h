#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>

// Building the payload an endpoint sends in a Close frame: a validated status code
// followed by an optional UTF-8 reason, capped at the 125-byte control-frame
// limit. Encoding either yields the bytes or one typed reason it could not.

namespace ruvia::detail {

enum class WebSocketClosePayloadEncodeError : std::uint8_t {
    kInvalidCode,
    kInvalidReason,
    kReasonTooLarge,
};

class WebSocketClosePayloadEncodeResult;

class WebSocketEncodedClosePayload final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const&& = delete;

private:
    friend class WebSocketClosePayloadEncodeResult;
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t, std::string_view) noexcept;

    WebSocketEncodedClosePayload(std::uint16_t code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_{};
    std::uint8_t size_{0};
};

class WebSocketClosePayloadEncodeFailure final {
public:
    [[nodiscard]] constexpr WebSocketClosePayloadEncodeError error() const noexcept {
        return error_;
    }

private:
    friend class WebSocketClosePayloadEncodeResult;
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t, std::string_view) noexcept;

    explicit constexpr WebSocketClosePayloadEncodeFailure(
        WebSocketClosePayloadEncodeError error) noexcept
        : error_(error) {}

    WebSocketClosePayloadEncodeError error_;
};

class WebSocketClosePayloadEncodeResult final {
public:
    [[nodiscard]] constexpr const WebSocketEncodedClosePayload* encoded() const& noexcept {
        return std::get_if<WebSocketEncodedClosePayload>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketEncodedClosePayload* encoded() const&& = delete;

    [[nodiscard]] constexpr const WebSocketClosePayloadEncodeFailure* failure() const& noexcept {
        return std::get_if<WebSocketClosePayloadEncodeFailure>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketClosePayloadEncodeFailure* failure() const&& = delete;

private:
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t, std::string_view) noexcept;

    explicit WebSocketClosePayloadEncodeResult(WebSocketEncodedClosePayload encoded) noexcept
        : value_(encoded) {}

    explicit constexpr WebSocketClosePayloadEncodeResult(
        WebSocketClosePayloadEncodeFailure failure) noexcept
        : value_(failure) {}

    using Value = std::variant<WebSocketEncodedClosePayload, WebSocketClosePayloadEncodeFailure>;
    Value value_;
};

[[nodiscard]] WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
    std::uint16_t code, std::string_view reason) noexcept;
}  // namespace ruvia::detail
