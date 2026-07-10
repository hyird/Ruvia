#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2PeerSettings.h"

namespace {

using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2PeerSettings;
using ruvia::detail::Http2PeerSettingsStatus;
using ruvia::detail::Http2SettingId;
using ruvia::detail::http2PeerSettingsErrorCode;
using ruvia::detail::http2SettingsPayloadSizeValid;

}  // namespace

RUVIA_TEST(http2_settings_enable_push) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 0).status == Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 1).status == Http2PeerSettingsStatus::kOk);
    // Only 0 or 1 are legal (RFC 7540 6.5.2).
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 2).status ==
                Http2PeerSettingsStatus::kInvalidEnablePush);
}

RUVIA_TEST(http2_settings_initial_window_size) {
    Http2PeerSettings settings;
    // Exactly 2^31-1 is valid; the delta from the default is reported.
    const auto ok = settings.apply(Http2SettingId::kInitialWindowSize, 0x7fffffffU);
    RUVIA_CHECK(ok.status == Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(ok.initialWindowChanged);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), std::int32_t{0x7fffffff});
    // Above 2^31-1 is a FLOW_CONTROL_ERROR, not a protocol error.
    RUVIA_CHECK(settings.apply(Http2SettingId::kInitialWindowSize, 0x80000000U).status ==
                Http2PeerSettingsStatus::kInvalidInitialWindow);
    RUVIA_CHECK(http2PeerSettingsErrorCode(Http2PeerSettingsStatus::kInvalidInitialWindow) ==
                Http2ErrorCode::kFlowControlError);
    // A non-flow error maps to PROTOCOL_ERROR.
    RUVIA_CHECK(http2PeerSettingsErrorCode(Http2PeerSettingsStatus::kInvalidEnablePush) ==
                Http2ErrorCode::kProtocolError);
}

RUVIA_TEST(http2_settings_max_frame_size_bounds) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, 16384).status ==
                Http2PeerSettingsStatus::kOk);  // minimum
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, 16777215).status ==
                Http2PeerSettingsStatus::kOk);  // maximum
    RUVIA_CHECK_EQ(settings.maxFrameSize(), std::uint32_t{16777215});
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, 16383).status ==
                Http2PeerSettingsStatus::kInvalidMaxFrameSize);  // below the range
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, 16777216).status ==
                Http2PeerSettingsStatus::kInvalidMaxFrameSize);  // above the range
}

RUVIA_TEST(http2_settings_unknown_ignored_and_payload_size) {
    Http2PeerSettings settings;
    // An unknown setting identifier must be ignored (RFC 7540 6.5.2).
    RUVIA_CHECK(settings.apply(static_cast<Http2SettingId>(0xABCD), 999).status ==
                Http2PeerSettingsStatus::kOk);
    // A SETTINGS payload length must be a multiple of six.
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view()));       // empty
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string(12, 'x')));     // two entries
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string(5, 'x')));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string(7, 'x')));
}

RUVIA_TEST(http2_settings_enable_connect_protocol) {
    Http2PeerSettings settings;
    RUVIA_CHECK(!settings.enableConnectProtocol());
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 1).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.enableConnectProtocol());
    // Once enabled it must not be turned off (RFC 8441).
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 0).status ==
                Http2PeerSettingsStatus::kInvalidEnableConnectProtocolTransition);
    // A value other than 0/1 is invalid.
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 2).status ==
                Http2PeerSettingsStatus::kInvalidEnableConnectProtocol);
}
