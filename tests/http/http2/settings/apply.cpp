#include <cstdint>
#include <string>
#include <string_view>

#include "http2/http2_peer_settings.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_error_code;
using ruvia::detail::http2_peer_setting_error;
using ruvia::detail::http2_peer_setting_error_code;
using ruvia::detail::http2_peer_settings;
using ruvia::detail::http2_role;
using ruvia::detail::http2_setting_id;
using ruvia::detail::http2_settings_payload_size_valid;

}  // namespace

RUVIA_TEST(http2_settings_enable_push) {
    http2_peer_settings settings(http2_role::server);
    const auto disabled = settings.apply(http2_setting_id::enable_push, 0);
    RUVIA_CHECK(disabled.applied() != nullptr);
    const auto enabled = settings.apply(http2_setting_id::enable_push, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    // Only 0 or 1 are legal (RFC 9113 Section 6.5.2).
    const auto invalid = settings.apply(http2_setting_id::enable_push, 2);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http2_peer_setting_error::invalid_enable_push);
}

RUVIA_TEST(http2_settings_initial_window_size) {
    http2_peer_settings settings(http2_role::server);
    // Exactly 2^31-1 is valid; the delta from the default is reported.
    const auto ok = settings.apply(http2_setting_id::initial_window_size, 0x7fffffffU);
    RUVIA_CHECK(ok.initial_window_change() != nullptr);
    RUVIA_CHECK_EQ(settings.initial_window_size(), std::int32_t{0x7fffffff});
    // Above 2^31-1 is a FLOW_CONTROL_ERROR, not a protocol error.
    const auto invalid = settings.apply(http2_setting_id::initial_window_size, 0x80000000U);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http2_peer_setting_error::invalid_initial_window);
    RUVIA_CHECK(http2_peer_setting_error_code(http2_peer_setting_error::invalid_initial_window) ==
                http2_error_code::flow_control_error);
    // A non-flow error maps to PROTOCOL_ERROR.
    RUVIA_CHECK(http2_peer_setting_error_code(http2_peer_setting_error::invalid_enable_push) ==
                http2_error_code::protocol_error);
}

RUVIA_TEST(http2_settings_max_frame_size_bounds) {
    http2_peer_settings settings(http2_role::server);
    const auto minimum = settings.apply(http2_setting_id::max_frame_size, 16384);
    RUVIA_CHECK(minimum.applied() != nullptr);
    const auto maximum = settings.apply(http2_setting_id::max_frame_size, 16777215);
    RUVIA_CHECK(maximum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.max_frame_size(), std::uint32_t{16777215});
    const auto below = settings.apply(http2_setting_id::max_frame_size, 16383);
    RUVIA_CHECK(below.failure() != nullptr);  // below the range
    RUVIA_CHECK(below.failure()->error() == http2_peer_setting_error::invalid_max_frame_size);
    const auto above = settings.apply(http2_setting_id::max_frame_size, 16777216);
    RUVIA_CHECK(above.failure() != nullptr);  // above the range
    RUVIA_CHECK(above.failure()->error() == http2_peer_setting_error::invalid_max_frame_size);
}

RUVIA_TEST(http2_settings_unknown_ignored_and_payload_size) {
    http2_peer_settings settings(http2_role::server);
    // An unknown setting identifier must be ignored (RFC 9113 Section 6.5.2).
    const auto unknown = settings.apply(static_cast<http2_setting_id>(0xABCD), 999);
    RUVIA_CHECK(unknown.applied() != nullptr);
    // A SETTINGS payload length must be a multiple of six.
    RUVIA_CHECK(http2_settings_payload_size_valid(std::string_view()));    // empty
    RUVIA_CHECK(http2_settings_payload_size_valid(std::string(12, 'x')));  // two entries
    RUVIA_CHECK(!http2_settings_payload_size_valid(std::string(5, 'x')));
    RUVIA_CHECK(!http2_settings_payload_size_valid(std::string(7, 'x')));
}

RUVIA_TEST(http2_settings_enable_connect_protocol) {
    http2_peer_settings settings(http2_role::server);
    RUVIA_CHECK(!settings.enable_connect_protocol());
    const auto enabled = settings.apply(http2_setting_id::enable_connect_protocol, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    RUVIA_CHECK(settings.enable_connect_protocol());
    // Once enabled it must not be turned off (RFC 8441).
    const auto disabled = settings.apply(http2_setting_id::enable_connect_protocol, 0);
    RUVIA_CHECK(disabled.failure() != nullptr);
    RUVIA_CHECK(disabled.failure()->error() ==
                http2_peer_setting_error::invalid_enable_connect_protocol_transition);
    // A value other than 0/1 is invalid.
    const auto invalid = settings.apply(http2_setting_id::enable_connect_protocol, 2);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http2_peer_setting_error::invalid_enable_connect_protocol);
}
