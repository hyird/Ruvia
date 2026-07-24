#pragma once

#include <system_error>
#include <utility>
#include <variant>

namespace ruvia::detail {

class WsTransportReadData final {
private:
    constexpr WsTransportReadData() noexcept = default;
    friend class WsTransportReadResult;
};

class WsTransportReadEnd final {
private:
    constexpr WsTransportReadEnd() noexcept = default;
    friend class WsTransportReadResult;
};

class WsTransportReadFailure final {
public:
    [[nodiscard]] std::error_code errorCode() const noexcept {
        return errorCode_;
    }

private:
    explicit WsTransportReadFailure(std::error_code errorCode) noexcept
        : errorCode_(errorCode) {}
    friend class WsTransportReadResult;

    std::error_code errorCode_;
};

class WsTransportReadResult final {
public:
    [[nodiscard]] static constexpr WsTransportReadResult makeData() noexcept {
        return WsTransportReadResult(WsTransportReadData{});
    }

    [[nodiscard]] static constexpr WsTransportReadResult makeEnd() noexcept {
        return WsTransportReadResult(WsTransportReadEnd{});
    }

    [[nodiscard]] static WsTransportReadResult makeFailure(std::error_code errorCode) noexcept {
        return WsTransportReadResult(WsTransportReadFailure(errorCode));
    }

    [[nodiscard]] const WsTransportReadData* data() const& noexcept {
        return std::get_if<WsTransportReadData>(&value_);
    }
    const WsTransportReadData* data() const&& = delete;

    [[nodiscard]] const WsTransportReadEnd* end() const& noexcept {
        return std::get_if<WsTransportReadEnd>(&value_);
    }
    const WsTransportReadEnd* end() const&& = delete;

    [[nodiscard]] const WsTransportReadFailure* failure() const& noexcept {
        return std::get_if<WsTransportReadFailure>(&value_);
    }
    const WsTransportReadFailure* failure() const&& = delete;

private:
    explicit constexpr WsTransportReadResult(WsTransportReadData data) noexcept
        : value_(data) {}

    explicit constexpr WsTransportReadResult(WsTransportReadEnd end) noexcept
        : value_(end) {}

    explicit WsTransportReadResult(WsTransportReadFailure failure) noexcept
        : value_(std::move(failure)) {}

    std::variant<WsTransportReadData, WsTransportReadEnd, WsTransportReadFailure> value_;
};

}  // namespace ruvia::detail
