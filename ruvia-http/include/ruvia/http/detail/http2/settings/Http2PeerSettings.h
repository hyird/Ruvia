#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2Role.h"

namespace ruvia::detail {

enum class Http2PeerSettingError : std::uint8_t { kInvalidEnablePush, kInvalidInitialWindow, kInvalidMaxFrameSize, kInvalidEnableConnectProtocol, kInvalidEnableConnectProtocolTransition };

class Http2PeerSettingApplyResult;

// The setting was valid and required no stream-window propagation. This also
// represents an unknown setting identifier, which RFC 9113 requires peers to
// ignore while continuing to process the SETTINGS frame.
class Http2PeerSettingApplied final {
private:
    friend class Http2PeerSettingApplyResult;

    constexpr Http2PeerSettingApplied() noexcept = default;
};

// SETTINGS_INITIAL_WINDOW_SIZE is the only setting whose application must be
// propagated to every active stream. The signed difference remains observable
// only on this alternative; a reduction can legitimately make a window negative.
class Http2PeerInitialWindowChange final {
public:
    [[nodiscard]] constexpr std::int64_t delta() const noexcept {
        return delta_;
    }

private:
    friend class Http2PeerSettingApplyResult;

    explicit constexpr Http2PeerInitialWindowChange(std::int64_t delta) noexcept
        : delta_(delta) {}

    std::int64_t delta_;
};

class Http2PeerSettingFailure final {
public:
    [[nodiscard]] constexpr Http2PeerSettingError error() const noexcept {
        return error_;
    }

private:
    friend class Http2PeerSettingApplyResult;

    explicit constexpr Http2PeerSettingFailure(Http2PeerSettingError error) noexcept
        : error_(error) {}

    Http2PeerSettingError error_;
};

// Applying one peer setting has exactly one outcome. Ordinary application owns
// no payload, an initial-window change owns its delta, and failure owns only the
// protocol reason. There is no status/changed/delta tuple with invalid mixtures.
class Http2PeerSettingApplyResult final {
public:
    [[nodiscard]] constexpr const Http2PeerSettingApplied* applied() const& noexcept {
        return std::get_if<Http2PeerSettingApplied>(&value_);
    }
    [[nodiscard]] constexpr const Http2PeerSettingApplied* applied() const&& = delete;

    [[nodiscard]] constexpr const Http2PeerInitialWindowChange* initialWindowChange() const& noexcept {
        return std::get_if<Http2PeerInitialWindowChange>(&value_);
    }
    [[nodiscard]] constexpr const Http2PeerInitialWindowChange* initialWindowChange() const&& = delete;

    [[nodiscard]] constexpr const Http2PeerSettingFailure* failure() const& noexcept {
        return std::get_if<Http2PeerSettingFailure>(&value_);
    }
    [[nodiscard]] constexpr const Http2PeerSettingFailure* failure() const&& = delete;

private:
    friend class Http2PeerSettings;

    using Value = std::variant<Http2PeerSettingApplied, Http2PeerInitialWindowChange, Http2PeerSettingFailure>;

    template <typename Alternative>
    explicit constexpr Http2PeerSettingApplyResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr Http2PeerSettingApplyResult makeApplied() noexcept {
        return Http2PeerSettingApplyResult(Http2PeerSettingApplied());
    }

    [[nodiscard]] static constexpr Http2PeerSettingApplyResult makeInitialWindowChange(std::int64_t delta) noexcept {
        return Http2PeerSettingApplyResult(Http2PeerInitialWindowChange(delta));
    }

    [[nodiscard]] static constexpr Http2PeerSettingApplyResult makeFailure(Http2PeerSettingError error) noexcept {
        return Http2PeerSettingApplyResult(Http2PeerSettingFailure(error));
    }

    Value value_;
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
    return Http2SettingEntry{.id = static_cast<Http2SettingId>(http2Read16(data)), .value = http2Read32(data + 2)};
}

[[nodiscard]] inline Http2ErrorCode http2PeerSettingErrorCode(Http2PeerSettingError error) noexcept {
    return error == Http2PeerSettingError::kInvalidInitialWindow ? Http2ErrorCode::kFlowControlError : Http2ErrorCode::kProtocolError;
}

[[nodiscard]] inline std::string_view http2PeerSettingErrorMessage(Http2PeerSettingError error) noexcept {
    switch (error) {
        case Http2PeerSettingError::kInvalidEnablePush:
            return "invalid ENABLE_PUSH";
        case Http2PeerSettingError::kInvalidInitialWindow:
            return "invalid initial window";
        case Http2PeerSettingError::kInvalidMaxFrameSize:
            return "invalid max frame size";
        case Http2PeerSettingError::kInvalidEnableConnectProtocol:
            return "invalid ENABLE_CONNECT_PROTOCOL";
        case Http2PeerSettingError::kInvalidEnableConnectProtocolTransition:
            return "invalid ENABLE_CONNECT_PROTOCOL transition";
    }
    return {};
}

class Http2PeerSettings final {
public:
    // SETTINGS semantics are directional. In particular, RFC 9113 permits a client
    // to send ENABLE_PUSH=1 but requires a client endpoint to reject that value from
    // a server peer, so the state cannot be constructed without its local role.
    explicit Http2PeerSettings(Http2Role localRole) noexcept
        : localRole_(localRole) {}

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

    // A complete SETTINGS frame is validated against a detached candidate and
    // committed only after every entry has passed. The local role is immutable,
    // so only the peer-controlled values need to be copied into the live state.
    void replaceValuesFrom(const Http2PeerSettings& candidate) noexcept {
        maxFrameSize_ = candidate.maxFrameSize_;
        initialWindowSize_ = candidate.initialWindowSize_;
        maxConcurrentStreams_ = candidate.maxConcurrentStreams_;
        enableConnectProtocol_ = candidate.enableConnectProtocol_;
        headerTableSize_ = candidate.headerTableSize_;
        maxHeaderListSize_ = candidate.maxHeaderListSize_;
    }

    [[nodiscard]] Http2PeerSettingApplyResult apply(Http2SettingId id, std::uint32_t value) noexcept {
        switch (id) {
            case Http2SettingId::kHeaderTableSize:
                headerTableSize_ = value;
                return Http2PeerSettingApplyResult::makeApplied();
            case Http2SettingId::kEnablePush:
                if (value > 1 || (localRole_ == Http2Role::kClient && value == 1)) {
                    return Http2PeerSettingApplyResult::makeFailure(Http2PeerSettingError::kInvalidEnablePush);
                }
                return Http2PeerSettingApplyResult::makeApplied();
            case Http2SettingId::kMaxConcurrentStreams:
                maxConcurrentStreams_ = value;
                return Http2PeerSettingApplyResult::makeApplied();
            case Http2SettingId::kInitialWindowSize: {
                if (!std::in_range<std::int32_t>(value)) {
                    return Http2PeerSettingApplyResult::makeFailure(Http2PeerSettingError::kInvalidInitialWindow);
                }
                const auto delta = static_cast<std::int64_t>(value) - initialWindowSize_;
                initialWindowSize_ = static_cast<std::int32_t>(value);
                return Http2PeerSettingApplyResult::makeInitialWindowChange(delta);
            }
            case Http2SettingId::kMaxFrameSize:
                if (value < kHttp2DefaultMaxFrameSize || value > kHttp2MaxFrameSizeLimit) {
                    return Http2PeerSettingApplyResult::makeFailure(Http2PeerSettingError::kInvalidMaxFrameSize);
                }
                maxFrameSize_ = value;
                return Http2PeerSettingApplyResult::makeApplied();
            case Http2SettingId::kMaxHeaderListSize:
                maxHeaderListSize_ = value;
                return Http2PeerSettingApplyResult::makeApplied();
            case Http2SettingId::kEnableConnectProtocol:
                if (value != 0 && value != 1) {
                    return Http2PeerSettingApplyResult::makeFailure(Http2PeerSettingError::kInvalidEnableConnectProtocol);
                }
                if (enableConnectProtocol_ && value == 0) {
                    return Http2PeerSettingApplyResult::makeFailure(Http2PeerSettingError::kInvalidEnableConnectProtocolTransition);
                }
                enableConnectProtocol_ = value == 1;
                return Http2PeerSettingApplyResult::makeApplied();
        }
        return Http2PeerSettingApplyResult::makeApplied();
    }

private:
    const Http2Role localRole_;
    std::uint32_t maxFrameSize_{kHttp2DefaultMaxFrameSize};
    // headerTableSize_ and maxHeaderListSize_ are parsed and stored to keep the
    // peer-settings model complete, but no encode path reads them, by design. The
    // encoder never indexes the HPACK dynamic table (static index + literal
    // without indexing only), so the peer's table size cannot be exceeded; and
    // SETTINGS_MAX_HEADER_LIST_SIZE is advisory (RFC 7540 6.5.2) while outbound
    // header blocks are already bounded by the local kMaxHttpHeaderBytes cap.
    std::uint32_t headerTableSize_{4096};
    std::uint32_t maxConcurrentStreams_{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t maxHeaderListSize_{std::numeric_limits<std::uint32_t>::max()};
    std::int32_t initialWindowSize_{kHttp2DefaultInitialWindowSize};
    bool enableConnectProtocol_{false};
};

}  // namespace ruvia::detail
