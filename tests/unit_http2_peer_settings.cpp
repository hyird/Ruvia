#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <string_view>

#include "net/http2/Http2PeerSettings.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2FrameTypes.h"

namespace {

using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2PeerSettings;
using ruvia::detail::Http2PeerSettingsStatus;
using ruvia::detail::Http2SettingId;
using ruvia::detail::http2PeerSettingsErrorCode;
using ruvia::detail::http2PeerSettingsErrorMessage;
using ruvia::detail::http2ReadSettingEntry;
using ruvia::detail::http2SettingsPayloadSizeValid;
using ruvia::detail::http2WriteSettingsEntry;
using ruvia::detail::kHttp2DefaultInitialWindowSize;
using ruvia::detail::kHttp2DefaultMaxFrameSize;
using ruvia::detail::kHttp2MaxFrameSizeLimit;

constexpr std::uint32_t kInt32Max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

}  // namespace

RUVIA_TEST(peer_settings_payload_size_validity) {
    // A SETTINGS payload is a sequence of 6-byte entries (RFC 7540 6.5).
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("", 0)));
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("aaaaaa", 6)));
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("aaaaaaaaaaaa", 12)));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string_view("aaaaa", 5)));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string_view("aaaaaaa", 7)));
}

RUVIA_TEST(peer_settings_read_entry_at_offset) {
    char buf[12];
    http2WriteSettingsEntry(buf, Http2SettingId::kMaxFrameSize, 20000);
    http2WriteSettingsEntry(buf + 6, Http2SettingId::kInitialWindowSize, 12345);
    const std::string_view payload(buf, 12);

    const auto first = http2ReadSettingEntry(payload, 0);
    RUVIA_CHECK(first.id == Http2SettingId::kMaxFrameSize);
    RUVIA_CHECK_EQ(first.value, std::uint32_t{20000});

    const auto second = http2ReadSettingEntry(payload, 6);
    RUVIA_CHECK(second.id == Http2SettingId::kInitialWindowSize);
    RUVIA_CHECK_EQ(second.value, std::uint32_t{12345});
}

RUVIA_TEST(peer_settings_defaults) {
    Http2PeerSettings settings;
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2DefaultMaxFrameSize);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), kHttp2DefaultInitialWindowSize);
    RUVIA_CHECK_EQ(settings.maxConcurrentStreams(), (std::numeric_limits<std::uint32_t>::max)());
    RUVIA_CHECK(!settings.enableConnectProtocol());
}

RUVIA_TEST(peer_settings_enable_push_must_be_boolean) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 0).status == Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 1).status == Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnablePush, 2).status ==
                Http2PeerSettingsStatus::kInvalidEnablePush);
}

RUVIA_TEST(peer_settings_initial_window_size_and_delta) {
    Http2PeerSettings settings;
    const auto result = settings.apply(Http2SettingId::kInitialWindowSize, 100000);
    RUVIA_CHECK(result.status == Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(result.initialWindowChanged);
    RUVIA_CHECK_EQ(result.initialWindowDelta,
                   std::int64_t{100000} - kHttp2DefaultInitialWindowSize);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), std::int32_t{100000});

    // Exactly 2^31-1 is allowed.
    Http2PeerSettings atMax;
    RUVIA_CHECK(atMax.apply(Http2SettingId::kInitialWindowSize, kInt32Max).status ==
                Http2PeerSettingsStatus::kOk);
    // One above is a flow-control error (RFC 7540 6.5.2).
    Http2PeerSettings tooBig;
    RUVIA_CHECK(tooBig.apply(Http2SettingId::kInitialWindowSize, kInt32Max + 1).status ==
                Http2PeerSettingsStatus::kInvalidInitialWindow);
}

RUVIA_TEST(peer_settings_max_frame_size_bounds) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, kHttp2DefaultMaxFrameSize).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2DefaultMaxFrameSize);
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, kHttp2MaxFrameSizeLimit).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2MaxFrameSizeLimit);
    // Below the 2^14 minimum and above the 2^24-1 maximum are rejected.
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, kHttp2DefaultMaxFrameSize - 1).status ==
                Http2PeerSettingsStatus::kInvalidMaxFrameSize);
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxFrameSize, kHttp2MaxFrameSizeLimit + 1).status ==
                Http2PeerSettingsStatus::kInvalidMaxFrameSize);
}

RUVIA_TEST(peer_settings_enable_connect_protocol_cannot_be_disabled) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 1).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.enableConnectProtocol());
    // Once enabled it must never be turned off (RFC 8441).
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 0).status ==
                Http2PeerSettingsStatus::kInvalidEnableConnectProtocolTransition);
    // A non-boolean value is invalid.
    RUVIA_CHECK(settings.apply(Http2SettingId::kEnableConnectProtocol, 5).status ==
                Http2PeerSettingsStatus::kInvalidEnableConnectProtocol);

    // Setting 0 while already disabled is fine.
    Http2PeerSettings fresh;
    RUVIA_CHECK(fresh.apply(Http2SettingId::kEnableConnectProtocol, 0).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(!fresh.enableConnectProtocol());
}

RUVIA_TEST(peer_settings_stored_values_and_unknown_ignored) {
    Http2PeerSettings settings;
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxConcurrentStreams, 250).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK_EQ(settings.maxConcurrentStreams(), std::uint32_t{250});
    RUVIA_CHECK(settings.apply(Http2SettingId::kHeaderTableSize, 8192).status ==
                Http2PeerSettingsStatus::kOk);
    RUVIA_CHECK(settings.apply(Http2SettingId::kMaxHeaderListSize, 1000).status ==
                Http2PeerSettingsStatus::kOk);
    // An unregistered setting id is ignored (RFC 7540 6.5.2).
    RUVIA_CHECK(settings.apply(static_cast<Http2SettingId>(0x63), 999).status ==
                Http2PeerSettingsStatus::kOk);
}

RUVIA_TEST(peer_settings_error_code_and_message_mapping) {
    // Only an invalid initial window is a flow-control error; the rest are protocol errors.
    RUVIA_CHECK(http2PeerSettingsErrorCode(Http2PeerSettingsStatus::kInvalidInitialWindow) ==
                Http2ErrorCode::kFlowControlError);
    RUVIA_CHECK(http2PeerSettingsErrorCode(Http2PeerSettingsStatus::kInvalidEnablePush) ==
                Http2ErrorCode::kProtocolError);
    RUVIA_CHECK(http2PeerSettingsErrorCode(Http2PeerSettingsStatus::kInvalidMaxFrameSize) ==
                Http2ErrorCode::kProtocolError);

    RUVIA_CHECK_EQ(http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kInvalidEnablePush),
                   std::string_view("invalid ENABLE_PUSH"));
    RUVIA_CHECK_EQ(http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kInvalidInitialWindow),
                   std::string_view("invalid initial window"));
    RUVIA_CHECK_EQ(http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kInvalidMaxFrameSize),
                   std::string_view("invalid max frame size"));
    RUVIA_CHECK_EQ(http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kInvalidEnableConnectProtocol),
                   std::string_view("invalid ENABLE_CONNECT_PROTOCOL"));
    RUVIA_CHECK_EQ(
        http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kInvalidEnableConnectProtocolTransition),
        std::string_view("invalid ENABLE_CONNECT_PROTOCOL transition"));
    RUVIA_CHECK(http2PeerSettingsErrorMessage(Http2PeerSettingsStatus::kOk).empty());
}
