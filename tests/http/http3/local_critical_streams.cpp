#include <array>
#include <memory_resource>
#include <variant>

#include "ruvia/http/http3_control_stream.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_peer_streams.h"

#include "test_harness.h"

RUVIA_TEST(http3_local_critical_streams_emit_independent_fragmentable_prefixes) {
    const auto created = ruvia::http3_local_critical_streams::create(
        {.max_field_section_size_ = 0});
    RUVIA_CHECK((created.index() == 0));
    if ((created.index() != 0)) {
        return;
    }
    const auto& streams = std::get<0>(created);
    const auto control = streams.control_prefix();
    const auto encoder = streams.qpack_encoder_prefix();
    const auto decoder = streams.qpack_decoder_prefix();
    RUVIA_CHECK_EQ(control.size(), std::size_t{9});
    RUVIA_CHECK_EQ(encoder.size(), std::size_t{1});
    RUVIA_CHECK_EQ(decoder.size(), std::size_t{1});
    RUVIA_CHECK(control[0] == 0);  // Control stream type.

    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_peer_streams peer_streams(ruvia::http3_peer_role::client, &resource);
    const std::array<std::span<const char>, 3> prefixes{control, encoder, decoder};
    const std::array<std::uint64_t, 3> ids{3, 7, 11};
    const std::array<ruvia::http3_peer_stream_kind, 3> kinds{
        ruvia::http3_peer_stream_kind::control,
        ruvia::http3_peer_stream_kind::qpack_encoder,
        ruvia::http3_peer_stream_kind::qpack_decoder};
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        auto result_value = peer_streams.feed(ids[i], prefixes[i].first(1));
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            RUVIA_CHECK(std::get<0>(result_value).kind_ == kinds[i]);
            RUVIA_CHECK_EQ(std::get<0>(result_value).consumed_, std::size_t{1});
        }
        for (const char byte : prefixes[i].subspan(1)) {
            result_value = peer_streams.feed(ids[i], std::span<const char>(&byte, 1));
            RUVIA_CHECK((result_value.index() == 0));
            if ((result_value.index() == 0)) {
                RUVIA_CHECK(std::get<0>(result_value).kind_ == kinds[i]);
            }
        }
    }

    ruvia::http3_control_stream control_parser(ruvia::http3_control_role::client, &resource);
    for (const char byte : control.subspan(1)) {
        RUVIA_CHECK(control_parser.feed(std::span<const char>(&byte, 1), false) ==
                    ruvia::http3_control_stream_status::need_more_data);
    }
    RUVIA_CHECK(control_parser.peer_settings().has_value());
    if (control_parser.peer_settings()) {
        RUVIA_CHECK_EQ(control_parser.peer_settings()->qpack_max_table_capacity_, std::uint64_t{0});
        RUVIA_CHECK_EQ(control_parser.peer_settings()->qpack_blocked_streams_, std::uint64_t{0});
        RUVIA_CHECK(control_parser.peer_settings()->max_field_section_size_ == std::uint64_t{0});
    }
}

RUVIA_TEST(http3_local_critical_streams_encode_absent_and_explicit_zero_limits) {
    const auto defaults = ruvia::http3_local_critical_streams::create();
    const auto explicit_zero = ruvia::http3_local_critical_streams::create(
        {.max_field_section_size_ = 0});
    const auto field_section_limit = ruvia::http3_local_critical_streams::create(
        {.max_field_section_size_ = 4096});
    const auto connect_enabled = ruvia::http3_local_critical_streams::create(
        {.enable_connect_protocol_ = true});
    RUVIA_CHECK((defaults.index() == 0));
    RUVIA_CHECK((explicit_zero.index() == 0));
    RUVIA_CHECK((field_section_limit.index() == 0));
    RUVIA_CHECK((connect_enabled.index() == 0));
    if ((defaults.index() == 0) && (explicit_zero.index() == 0) && (field_section_limit.index() == 0) && (connect_enabled.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(defaults).control_prefix().size(), std::size_t{7});
        RUVIA_CHECK_EQ(std::get<0>(explicit_zero).control_prefix().size(), std::size_t{9});
        RUVIA_CHECK_EQ(std::get<0>(field_section_limit).control_prefix().size(), std::size_t{10});
        RUVIA_CHECK_EQ(std::get<0>(connect_enabled).control_prefix().size(), std::size_t{9});
        RUVIA_CHECK(std::get<0>(defaults).control_prefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(explicit_zero).control_prefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(field_section_limit).control_prefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(connect_enabled).control_prefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK_EQ(std::get<0>(defaults).qpack_encoder_prefix().size(), std::size_t{1});
        RUVIA_CHECK_EQ(std::get<0>(defaults).qpack_decoder_prefix().size(), std::size_t{1});

        std::pmr::monotonic_buffer_resource resource;
        ruvia::http3_control_stream parser(ruvia::http3_control_role::client, &resource);
        const auto prefix = std::get<0>(field_section_limit).control_prefix();
        RUVIA_CHECK(parser.feed(prefix.subspan(1), false) ==
                    ruvia::http3_control_stream_status::need_more_data);
        RUVIA_CHECK(parser.peer_settings().has_value());
        if (parser.peer_settings()) {
            RUVIA_CHECK(parser.peer_settings()->max_field_section_size_ == std::uint64_t{4096});
        }

        ruvia::http3_control_stream connect_parser(ruvia::http3_control_role::client, &resource);
        const auto connect_prefix = std::get<0>(connect_enabled).control_prefix();
        RUVIA_CHECK(connect_parser.feed(connect_prefix.subspan(1), false) ==
                    ruvia::http3_control_stream_status::need_more_data);
        RUVIA_CHECK(connect_parser.peer_settings().has_value());
        if (connect_parser.peer_settings()) {
            RUVIA_CHECK(connect_parser.peer_settings()->enable_connect_protocol_);
        }
    }
}

RUVIA_TEST(http3_local_critical_streams_advertise_dynamic_qpack_and_datagrams) {
    const auto prefixes = ruvia::http3_local_critical_streams::create(
        {.qpack_max_table_capacity_ = 4096, .qpack_blocked_streams_ = 16, .enable_connect_protocol_ = true, .h3_datagram_ = true});
    RUVIA_CHECK((prefixes.index() == 0));
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http3_control_stream control(ruvia::http3_control_role::client, &resource);
    RUVIA_CHECK(control.feed(std::get<0>(prefixes).control_prefix().subspan(1), false) == ruvia::http3_control_stream_status::need_more_data);
    RUVIA_CHECK(control.peer_settings().has_value());
    RUVIA_CHECK_EQ(control.peer_settings()->qpack_max_table_capacity_, 4096u);
    RUVIA_CHECK_EQ(control.peer_settings()->qpack_blocked_streams_, 16u);
    RUVIA_CHECK(control.peer_settings()->h3_datagram_);
}
