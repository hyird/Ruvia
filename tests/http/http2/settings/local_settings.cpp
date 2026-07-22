#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/settings/Http2LocalSettings.h"
#include "ruvia/http/detail/http2/settings/Http2PeerSettings.h"

namespace {

using ruvia::detail::Http2FrameType;
using ruvia::detail::Http2LocalSettings;
using ruvia::detail::Http2SettingId;

struct ExpectedSetting final {
    Http2SettingId id;
    std::uint32_t value;
};

constexpr std::array<ExpectedSetting, Http2LocalSettings::kEntryCount> kExpectedSettings{{
    {Http2SettingId::kHeaderTableSize, Http2LocalSettings::kHeaderTableSize},
    {Http2SettingId::kEnablePush, Http2LocalSettings::kEnablePush},
    {Http2SettingId::kMaxConcurrentStreams, Http2LocalSettings::kMaxConcurrentStreams},
    {Http2SettingId::kInitialWindowSize, Http2LocalSettings::kInitialWindowSize},
    {Http2SettingId::kMaxFrameSize, Http2LocalSettings::kMaxFrameSize},
    {Http2SettingId::kMaxHeaderListSize, Http2LocalSettings::kMaxHeaderListSize},
    {Http2SettingId::kEnableConnectProtocol, Http2LocalSettings::kEnableConnectProtocol},
}};

}  // namespace

RUVIA_TEST(http2_local_settings_wire_is_the_compile_time_contract) {
    std::array<char, Http2LocalSettings::kFrameBytes> bytes{};
    const auto* end = ruvia::detail::http2WriteLocalSettingsFrame(bytes.data());
    RUVIA_CHECK_EQ(end, bytes.data() + bytes.size());

    const auto wire = std::string_view(bytes.data(), bytes.size());
    const auto header = ruvia::detail::http2ParseFrameHeader(wire.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(header.flags, std::uint8_t{0});
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{0});
    RUVIA_CHECK_EQ(header.length, Http2LocalSettings::kPayloadBytes);

    const auto payload = wire.substr(9);
    for (std::size_t i = 0; i < kExpectedSettings.size(); ++i) {
        const auto setting = ruvia::detail::http2ReadSettingEntry(payload, i * 6);
        RUVIA_CHECK(setting.id == kExpectedSettings[i].id);
        RUVIA_CHECK_EQ(setting.value, kExpectedSettings[i].value);
    }
}
