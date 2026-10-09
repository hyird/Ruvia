#include <concepts>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_types.h"
#include "http2/http2_peer_settings.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_default_initial_window_size;
using ruvia::detail::http2_default_max_frame_size;
using ruvia::detail::http2_error_code;
using ruvia::detail::http2_max_frame_size_limit;
using ruvia::detail::http2_peer_initial_window_change;
using ruvia::detail::http2_peer_setting_applied;
using ruvia::detail::http2_peer_setting_apply_result;
using ruvia::detail::http2_peer_setting_error;
using ruvia::detail::http2_peer_setting_error_code;
using ruvia::detail::http2_peer_setting_error_message;
using ruvia::detail::http2_peer_setting_failure;
using ruvia::detail::http2_peer_settings;
using ruvia::detail::http2_read_setting_entry;
using ruvia::detail::http2_role;
using ruvia::detail::http2_setting_id;
using ruvia::detail::http2_settings_payload_size_valid;
using ruvia::detail::http2_write_settings_entry;

constexpr std::uint32_t int32_max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

}  // namespace

RUVIA_TEST(peer_settings_payload_size_validity) {
    // A SETTINGS payload is a sequence of 6-byte entries (RFC 9113 Section 6.5).
    RUVIA_CHECK(http2_settings_payload_size_valid(std::string_view("", 0)));
    RUVIA_CHECK(http2_settings_payload_size_valid(std::string_view("aaaaaa", 6)));
    RUVIA_CHECK(http2_settings_payload_size_valid(std::string_view("aaaaaaaaaaaa", 12)));
    RUVIA_CHECK(!http2_settings_payload_size_valid(std::string_view("aaaaa", 5)));
    RUVIA_CHECK(!http2_settings_payload_size_valid(std::string_view("aaaaaaa", 7)));
}

RUVIA_TEST(peer_settings_read_entry_at_offset) {
    char buf[12];
    http2_write_settings_entry(buf, http2_setting_id::max_frame_size, 20000);
    http2_write_settings_entry(buf + 6, http2_setting_id::initial_window_size, 12345);
    const std::string_view payload_value(buf, 12);

    const auto first = http2_read_setting_entry(payload_value, 0);
    RUVIA_CHECK(first.id_ == http2_setting_id::max_frame_size);
    RUVIA_CHECK_EQ(first.value_, std::uint32_t{20000});

    const auto second = http2_read_setting_entry(payload_value, 6);
    RUVIA_CHECK(second.id_ == http2_setting_id::initial_window_size);
    RUVIA_CHECK_EQ(second.value_, std::uint32_t{12345});
}

RUVIA_TEST(peer_settings_defaults) {
    http2_peer_settings settings(http2_role::server);
    RUVIA_CHECK_EQ(settings.max_frame_size(), http2_default_max_frame_size);
    RUVIA_CHECK_EQ(settings.initial_window_size(), http2_default_initial_window_size);
    RUVIA_CHECK_EQ(settings.max_concurrent_streams(), (std::numeric_limits<std::uint32_t>::max)());
    RUVIA_CHECK(!settings.enable_connect_protocol());
}

RUVIA_TEST(peer_setting_apply_result_is_discriminated) {
    http2_peer_settings settings(http2_role::server);

    const auto applied = settings.apply(http2_setting_id::header_table_size, 8192);
    RUVIA_CHECK(applied.applied() != nullptr);
    RUVIA_CHECK(applied.initial_window_change() == nullptr);
    RUVIA_CHECK(applied.failure() == nullptr);

    // Re-advertising the same value still requires stream propagation, with delta zero.
    const auto changed = settings.apply(http2_setting_id::initial_window_size,
        static_cast<std::uint32_t>(http2_default_initial_window_size));
    RUVIA_CHECK(changed.applied() == nullptr);
    RUVIA_CHECK(changed.initial_window_change() != nullptr);
    RUVIA_CHECK_EQ(changed.initial_window_change()->delta(), std::int64_t{0});
    RUVIA_CHECK(changed.failure() == nullptr);

    const auto failed = settings.apply(http2_setting_id::max_frame_size, 0);
    RUVIA_CHECK(failed.applied() == nullptr);
    RUVIA_CHECK(failed.initial_window_change() == nullptr);
    RUVIA_CHECK(failed.failure() != nullptr);
    RUVIA_CHECK(failed.failure()->error() == http2_peer_setting_error::invalid_max_frame_size);
}

RUVIA_TEST(peer_settings_enable_push_is_directional) {
    http2_peer_settings server(http2_role::server);
    const auto server_disabled = server.apply(http2_setting_id::enable_push, 0);
    RUVIA_CHECK(server_disabled.applied() != nullptr);
    const auto server_enabled = server.apply(http2_setting_id::enable_push, 1);
    RUVIA_CHECK(server_enabled.applied() != nullptr);
    const auto invalid_server_value = server.apply(http2_setting_id::enable_push, 2);
    RUVIA_CHECK(invalid_server_value.failure() != nullptr);
    RUVIA_CHECK(invalid_server_value.failure()->error() == http2_peer_setting_error::invalid_enable_push);

    http2_peer_settings client(http2_role::client);
    const auto client_disabled = client.apply(http2_setting_id::enable_push, 0);
    RUVIA_CHECK(client_disabled.applied() != nullptr);
    const auto invalid_from_server = client.apply(http2_setting_id::enable_push, 1);
    RUVIA_CHECK(invalid_from_server.failure() != nullptr);
    RUVIA_CHECK(invalid_from_server.failure()->error() == http2_peer_setting_error::invalid_enable_push);
}

RUVIA_TEST(peer_settings_initial_window_size_and_delta) {
    http2_peer_settings settings(http2_role::server);
    const auto result_value = settings.apply(http2_setting_id::initial_window_size, 100000);
    RUVIA_CHECK(result_value.initial_window_change() != nullptr);
    RUVIA_CHECK_EQ(result_value.initial_window_change()->delta(),
        std::int64_t{100000} - http2_default_initial_window_size);
    RUVIA_CHECK_EQ(settings.initial_window_size(), std::int32_t{100000});

    // Exactly 2^31-1 is allowed and reports the signed difference.
    http2_peer_settings at_max(http2_role::server);
    const auto max_value = at_max.apply(http2_setting_id::initial_window_size, int32_max);
    RUVIA_CHECK(max_value.initial_window_change() != nullptr);
    RUVIA_CHECK_EQ(max_value.initial_window_change()->delta(),
        static_cast<std::int64_t>(int32_max) - http2_default_initial_window_size);
    // One above is a flow-control error (RFC 9113 Section 6.5.2).
    http2_peer_settings too_big(http2_role::server);
    const auto invalid = too_big.apply(http2_setting_id::initial_window_size, int32_max + 1);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http2_peer_setting_error::invalid_initial_window);
}

RUVIA_TEST(peer_settings_max_frame_size_bounds) {
    http2_peer_settings settings(http2_role::server);
    const auto minimum = settings.apply(http2_setting_id::max_frame_size, http2_default_max_frame_size);
    RUVIA_CHECK(minimum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.max_frame_size(), http2_default_max_frame_size);
    const auto maximum = settings.apply(http2_setting_id::max_frame_size, http2_max_frame_size_limit);
    RUVIA_CHECK(maximum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.max_frame_size(), http2_max_frame_size_limit);
    // Below the 2^14 minimum and above the 2^24-1 maximum are rejected.
    const auto below = settings.apply(http2_setting_id::max_frame_size, http2_default_max_frame_size - 1);
    RUVIA_CHECK(below.failure() != nullptr);
    RUVIA_CHECK(below.failure()->error() == http2_peer_setting_error::invalid_max_frame_size);
    const auto above = settings.apply(http2_setting_id::max_frame_size, http2_max_frame_size_limit + 1);
    RUVIA_CHECK(above.failure() != nullptr);
    RUVIA_CHECK(above.failure()->error() == http2_peer_setting_error::invalid_max_frame_size);
}

RUVIA_TEST(peer_settings_enable_connect_protocol_cannot_be_disabled) {
    http2_peer_settings settings(http2_role::server);
    const auto enabled = settings.apply(http2_setting_id::enable_connect_protocol, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    RUVIA_CHECK(settings.enable_connect_protocol());
    // Once enabled it must never be turned off (RFC 8441).
    const auto disabled = settings.apply(http2_setting_id::enable_connect_protocol, 0);
    RUVIA_CHECK(disabled.failure() != nullptr);
    RUVIA_CHECK(disabled.failure()->error() ==
                http2_peer_setting_error::invalid_enable_connect_protocol_transition);
    // A non-boolean value is invalid.
    const auto invalid = settings.apply(http2_setting_id::enable_connect_protocol, 5);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http2_peer_setting_error::invalid_enable_connect_protocol);

    // Setting 0 while already disabled is fine.
    http2_peer_settings fresh(http2_role::server);
    const auto remains_disabled = fresh.apply(http2_setting_id::enable_connect_protocol, 0);
    RUVIA_CHECK(remains_disabled.applied() != nullptr);
    RUVIA_CHECK(!fresh.enable_connect_protocol());
}

RUVIA_TEST(peer_settings_stored_values_and_unknown_ignored) {
    http2_peer_settings settings(http2_role::server);
    const auto max_concurrent = settings.apply(http2_setting_id::max_concurrent_streams, 250);
    RUVIA_CHECK(max_concurrent.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.max_concurrent_streams(), std::uint32_t{250});
    const auto header_table = settings.apply(http2_setting_id::header_table_size, 8192);
    RUVIA_CHECK(header_table.applied() != nullptr);
    const auto header_list = settings.apply(http2_setting_id::max_header_list_size, 1000);
    RUVIA_CHECK(header_list.applied() != nullptr);
    // An unregistered setting id is ignored (RFC 9113 Section 6.5.2).
    const auto unknown = settings.apply(static_cast<http2_setting_id>(0x63), 999);
    RUVIA_CHECK(unknown.applied() != nullptr);
}

RUVIA_TEST(peer_settings_error_code_and_message_mapping) {
    // Only an invalid initial window is a flow-control error; the rest are protocol errors.
    RUVIA_CHECK(http2_peer_setting_error_code(http2_peer_setting_error::invalid_initial_window) ==
                http2_error_code::flow_control_error);
    RUVIA_CHECK(http2_peer_setting_error_code(http2_peer_setting_error::invalid_enable_push) ==
                http2_error_code::protocol_error);
    RUVIA_CHECK(http2_peer_setting_error_code(http2_peer_setting_error::invalid_max_frame_size) ==
                http2_error_code::protocol_error);

    RUVIA_CHECK_EQ(http2_peer_setting_error_message(http2_peer_setting_error::invalid_enable_push),
        std::string_view("invalid ENABLE_PUSH"));
    RUVIA_CHECK_EQ(http2_peer_setting_error_message(http2_peer_setting_error::invalid_initial_window),
        std::string_view("invalid initial window"));
    RUVIA_CHECK_EQ(http2_peer_setting_error_message(http2_peer_setting_error::invalid_max_frame_size),
        std::string_view("invalid max frame size"));
    RUVIA_CHECK_EQ(
        http2_peer_setting_error_message(http2_peer_setting_error::invalid_enable_connect_protocol),
        std::string_view("invalid ENABLE_CONNECT_PROTOCOL"));
    RUVIA_CHECK_EQ(http2_peer_setting_error_message(
                       http2_peer_setting_error::invalid_enable_connect_protocol_transition),
        std::string_view("invalid ENABLE_CONNECT_PROTOCOL transition"));
}
