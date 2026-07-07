#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "Http2Frame.h"

namespace ruvia::detail {

enum class Http2PeerSettingsStatus : std::uint8_t {
    kOk,
    kInvalidEnablePush,
    kInvalidInitialWindow,
    kInvalidMaxFrameSize,
    kInvalidEnableConnectProtocol,
    kInvalidEnableConnectProtocolTransition
};

struct Http2PeerSettingsResult final {
    Http2PeerSettingsStatus status{Http2PeerSettingsStatus::kOk};
    std::int64_t initialWindowDelta{0};
    bool initialWindowChanged{false};
};

struct Http2SettingEntry final {
    Http2SettingId id{Http2SettingId::kHeaderTableSize};
    std::uint32_t value{0};
};

[[nodiscard]] inline bool http2SettingsPayloadSizeValid(std::string_view payload) noexcept {
    return payload.size() % 6 == 0;
}

[[nodiscard]] inline Http2SettingEntry http2ReadSettingEntry(std::string_view payload, std::size_t offset) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(payload.data() + offset);
    return Http2SettingEntry{
        .id = static_cast<Http2SettingId>(http2Read16(data)),
        .value = http2Read32(data + 2)};
}

[[nodiscard]] inline Http2ErrorCode http2PeerSettingsErrorCode(Http2PeerSettingsStatus status) noexcept {
    return status == Http2PeerSettingsStatus::kInvalidInitialWindow
        ? Http2ErrorCode::kFlowControlError
        : Http2ErrorCode::kProtocolError;
}

[[nodiscard]] inline std::string_view http2PeerSettingsErrorMessage(Http2PeerSettingsStatus status) noexcept {
    switch (status) {
        case Http2PeerSettingsStatus::kInvalidEnablePush:
            return "invalid ENABLE_PUSH";
        case Http2PeerSettingsStatus::kInvalidInitialWindow:
            return "invalid initial window";
        case Http2PeerSettingsStatus::kInvalidMaxFrameSize:
            return "invalid max frame size";
        case Http2PeerSettingsStatus::kInvalidEnableConnectProtocol:
            return "invalid ENABLE_CONNECT_PROTOCOL";
        case Http2PeerSettingsStatus::kInvalidEnableConnectProtocolTransition:
            return "invalid ENABLE_CONNECT_PROTOCOL transition";
        case Http2PeerSettingsStatus::kOk:
            break;
    }
    return {};
}

class Http2PeerSettings final {
public:
    [[nodiscard]] std::uint32_t maxFrameSize() const noexcept {
        return maxFrameSize_;
    }

    [[nodiscard]] std::int32_t initialWindowSize() const noexcept {
        return initialWindowSize_;
    }

    [[nodiscard]] std::uint32_t maxConcurrentStreams() const noexcept {
        return maxConcurrentStreams_;
    }

    [[nodiscard]] bool enableConnectProtocol() const noexcept {
        return enableConnectProtocol_;
    }

    [[nodiscard]] Http2PeerSettingsResult apply(Http2SettingId id, std::uint32_t value) noexcept {
        switch (id) {
            case Http2SettingId::kHeaderTableSize:
                headerTableSize_ = value;
                return {};
            case Http2SettingId::kEnablePush:
                return value == 0 || value == 1
                    ? Http2PeerSettingsResult{}
                    : Http2PeerSettingsResult{.status = Http2PeerSettingsStatus::kInvalidEnablePush};
            case Http2SettingId::kMaxConcurrentStreams:
                maxConcurrentStreams_ = value;
                return {};
            case Http2SettingId::kInitialWindowSize: {
                if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                    return Http2PeerSettingsResult{.status = Http2PeerSettingsStatus::kInvalidInitialWindow};
                }
                const auto delta = static_cast<std::int64_t>(value) - initialWindowSize_;
                initialWindowSize_ = static_cast<std::int32_t>(value);
                return Http2PeerSettingsResult{.initialWindowDelta = delta, .initialWindowChanged = true};
            }
            case Http2SettingId::kMaxFrameSize:
                if (value < kHttp2DefaultMaxFrameSize || value > kHttp2MaxFrameSizeLimit) {
                    return Http2PeerSettingsResult{.status = Http2PeerSettingsStatus::kInvalidMaxFrameSize};
                }
                maxFrameSize_ = value;
                return {};
            case Http2SettingId::kMaxHeaderListSize:
                maxHeaderListSize_ = value;
                return {};
            case Http2SettingId::kEnableConnectProtocol:
                if (value != 0 && value != 1) {
                    return Http2PeerSettingsResult{.status = Http2PeerSettingsStatus::kInvalidEnableConnectProtocol};
                }
                if (enableConnectProtocol_ && value == 0) {
                    return Http2PeerSettingsResult{.status = Http2PeerSettingsStatus::kInvalidEnableConnectProtocolTransition};
                }
                enableConnectProtocol_ = value == 1;
                return {};
        }
        return {};
    }

private:
    std::uint32_t maxFrameSize_{kHttp2DefaultMaxFrameSize};
    std::uint32_t headerTableSize_{4096};
    std::uint32_t maxConcurrentStreams_{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t maxHeaderListSize_{std::numeric_limits<std::uint32_t>::max()};
    std::int32_t initialWindowSize_{kHttp2DefaultInitialWindowSize};
    bool enableConnectProtocol_{false};
};

}  // namespace ruvia::detail
