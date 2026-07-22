#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/settings/Http2PeerSettings.h"

namespace {

using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2PeerSettingError;
using ruvia::detail::Http2PeerSettings;
using ruvia::detail::Http2Role;
using ruvia::detail::Http2SettingId;
using ruvia::detail::http2PeerSettingErrorCode;
using ruvia::detail::http2SettingsPayloadSizeValid;

}  // namespace

RUVIA_TEST(http2_settings_enable_push) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto disabled = settings.apply(Http2SettingId::kEnablePush, 0);
    RUVIA_CHECK(disabled.applied() != nullptr);
    const auto enabled = settings.apply(Http2SettingId::kEnablePush, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    // Only 0 or 1 are legal (RFC 9113 Section 6.5.2).
    const auto invalid = settings.apply(Http2SettingId::kEnablePush, 2);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == Http2PeerSettingError::kInvalidEnablePush);
}

RUVIA_TEST(http2_settings_initial_window_size) {
    Http2PeerSettings settings(Http2Role::kServer);
    // Exactly 2^31-1 is valid; the delta from the default is reported.
    const auto ok = settings.apply(Http2SettingId::kInitialWindowSize, 0x7fffffffU);
    RUVIA_CHECK(ok.initialWindowChange() != nullptr);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), std::int32_t{0x7fffffff});
    // Above 2^31-1 is a FLOW_CONTROL_ERROR, not a protocol error.
    const auto invalid = settings.apply(Http2SettingId::kInitialWindowSize, 0x80000000U);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == Http2PeerSettingError::kInvalidInitialWindow);
    RUVIA_CHECK(http2PeerSettingErrorCode(Http2PeerSettingError::kInvalidInitialWindow) ==
                Http2ErrorCode::kFlowControlError);
    // A non-flow error maps to PROTOCOL_ERROR.
    RUVIA_CHECK(http2PeerSettingErrorCode(Http2PeerSettingError::kInvalidEnablePush) ==
                Http2ErrorCode::kProtocolError);
}

RUVIA_TEST(http2_settings_max_frame_size_bounds) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto minimum = settings.apply(Http2SettingId::kMaxFrameSize, 16384);
    RUVIA_CHECK(minimum.applied() != nullptr);
    const auto maximum = settings.apply(Http2SettingId::kMaxFrameSize, 16777215);
    RUVIA_CHECK(maximum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), std::uint32_t{16777215});
    const auto below = settings.apply(Http2SettingId::kMaxFrameSize, 16383);
    RUVIA_CHECK(below.failure() != nullptr);  // below the range
    RUVIA_CHECK(below.failure()->error() == Http2PeerSettingError::kInvalidMaxFrameSize);
    const auto above = settings.apply(Http2SettingId::kMaxFrameSize, 16777216);
    RUVIA_CHECK(above.failure() != nullptr);  // above the range
    RUVIA_CHECK(above.failure()->error() == Http2PeerSettingError::kInvalidMaxFrameSize);
}

RUVIA_TEST(http2_settings_unknown_ignored_and_payload_size) {
    Http2PeerSettings settings(Http2Role::kServer);
    // An unknown setting identifier must be ignored (RFC 9113 Section 6.5.2).
    const auto unknown =
        settings.apply(static_cast<Http2SettingId>(0xABCD), 999);
    RUVIA_CHECK(unknown.applied() != nullptr);
    // A SETTINGS payload length must be a multiple of six.
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view()));       // empty
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string(12, 'x')));     // two entries
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string(5, 'x')));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string(7, 'x')));
}

RUVIA_TEST(http2_settings_enable_connect_protocol) {
    Http2PeerSettings settings(Http2Role::kServer);
    RUVIA_CHECK(!settings.enableConnectProtocol());
    const auto enabled =
        settings.apply(Http2SettingId::kEnableConnectProtocol, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    RUVIA_CHECK(settings.enableConnectProtocol());
    // Once enabled it must not be turned off (RFC 8441).
    const auto disabled = settings.apply(Http2SettingId::kEnableConnectProtocol, 0);
    RUVIA_CHECK(disabled.failure() != nullptr);
    RUVIA_CHECK(disabled.failure()->error() ==
                Http2PeerSettingError::kInvalidEnableConnectProtocolTransition);
    // A value other than 0/1 is invalid.
    const auto invalid = settings.apply(Http2SettingId::kEnableConnectProtocol, 2);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() ==
                Http2PeerSettingError::kInvalidEnableConnectProtocol);
}
