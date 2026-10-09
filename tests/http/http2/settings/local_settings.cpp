#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "http2/http2_frame_codec.h"
#include "http2/http2_local_settings.h"
#include "http2/http2_peer_settings.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_local_settings;
using ruvia::detail::http2_setting_id;

struct expected_setting final {
    http2_setting_id id_;
    std::uint32_t value_;
};

constexpr std::array<expected_setting, http2_local_settings::entry_count> expected_settings{{
    {http2_setting_id::header_table_size, http2_local_settings::header_table_size},
    {http2_setting_id::enable_push, http2_local_settings::enable_push},
    {http2_setting_id::max_concurrent_streams, http2_local_settings::max_concurrent_streams},
    {http2_setting_id::initial_window_size, http2_local_settings::initial_window_size},
    {http2_setting_id::max_frame_size, http2_local_settings::max_frame_size},
    {http2_setting_id::max_header_list_size, http2_local_settings::max_header_list_size},
    {http2_setting_id::enable_connect_protocol, http2_local_settings::enable_connect_protocol},
    {http2_setting_id::no_rfc7540_priorities, 1},
}};

}  // namespace

RUVIA_TEST(http2_local_settings_wire_is_the_compile_time_contract) {
    std::array<char, http2_local_settings::frame_bytes> bytes_value{};
    const auto* end = ruvia::detail::http2_write_local_settings_frame(bytes_value.data());
    RUVIA_CHECK_EQ(end, bytes_value.data() + bytes_value.size());

    const auto wire = std::string_view(bytes_value.data(), bytes_value.size());
    const auto header_value = ruvia::detail::http2_parse_frame_header(wire.substr(0, 9));
    RUVIA_CHECK_EQ(header_value.type_, static_cast<std::uint8_t>(http2_frame_type::settings));
    RUVIA_CHECK_EQ(header_value.flags_, std::uint8_t{0});
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{0});
    RUVIA_CHECK_EQ(header_value.length_, http2_local_settings::payload_bytes);

    const auto payload_value = wire.substr(9);
    for (std::size_t i = 0; i < expected_settings.size(); ++i) {
        const auto setting = ruvia::detail::http2_read_setting_entry(payload_value, i * 6);
        RUVIA_CHECK(setting.id_ == expected_settings[i].id_);
        RUVIA_CHECK_EQ(setting.value_, expected_settings[i].value_);
    }
}
