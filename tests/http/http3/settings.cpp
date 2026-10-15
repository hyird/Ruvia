#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <variant>

#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http3_var_int.h"

#include "test_harness.h"

RUVIA_TEST(http3_settings_round_trip_known_values_and_disable_dynamic_table) {
    const ruvia::http3_settings settings{
        .qpack_max_table_capacity_ = 0,
        .max_field_section_size_ = ruvia::http3_var_int_max,
        .qpack_blocked_streams_ = (std::uint64_t{1} << 30),
        .enable_connect_protocol_ = true,
    };
    std::array<char, 32> wire{};
    const auto written = ruvia::encode_http3_settings(wire, settings);
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    std::pmr::monotonic_buffer_resource resource;
    const auto decoded = ruvia::decode_http3_settings(std::span<const char>(wire).first(std::get<0>(written)), &resource);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(decoded).qpack_max_table_capacity_, std::uint64_t{0});
        RUVIA_CHECK_EQ(std::get<0>(decoded).max_field_section_size_, settings.max_field_section_size_);
        RUVIA_CHECK_EQ(std::get<0>(decoded).qpack_blocked_streams_, settings.qpack_blocked_streams_);
        RUVIA_CHECK(std::get<0>(decoded).enable_connect_protocol_);
    }
}

RUVIA_TEST(http3_settings_skips_unknown_ids_but_rejects_duplicates_everywhere) {
    constexpr std::array<char, 8> unknown_and_known{0x20, 0x2a, 0x01, 0x05, 0x21, 0x00, 0x01, 0x09};
    const auto duplicate = ruvia::decode_http3_settings(unknown_and_known);
    RUVIA_CHECK(!(duplicate.index() == 0));
    if ((duplicate.index() != 0)) {
        RUVIA_CHECK(std::get<1>(duplicate) == ruvia::http3_settings_error::duplicate_identifier);
    }

    constexpr std::array<char, 6> duplicate_unknown{0x20, 0x2a, 0x21, 0x01, 0x20, 0x09};
    const auto repeated_unknown = ruvia::decode_http3_settings(duplicate_unknown);
    RUVIA_CHECK(!(repeated_unknown.index() == 0));
    if ((repeated_unknown.index() != 0)) {
        RUVIA_CHECK(std::get<1>(repeated_unknown) == ruvia::http3_settings_error::duplicate_identifier);
    }

    constexpr std::array<char, 6> unknown_only{0x20, 0x2a, 0x21, 0x01, 0x22, 0x00};
    const auto skipped = ruvia::decode_http3_settings(unknown_only);
    RUVIA_CHECK((skipped.index() == 0));
    if ((skipped.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(skipped).qpack_max_table_capacity_, std::uint64_t{0});
        RUVIA_CHECK(!std::get<0>(skipped).max_field_section_size_.has_value());
        RUVIA_CHECK_EQ(std::get<0>(skipped).qpack_blocked_streams_, std::uint64_t{0});
    }
}

RUVIA_TEST(http3_settings_rejects_forbidden_ids_and_truncated_varints) {
    for (std::uint64_t identifier = 0; identifier <= 0x5; ++identifier) {
        if (identifier == 0x1) {
            continue;
        }
        std::array<char, 16> wire{};
        const auto id_size = ruvia::encode_http3_var_int(wire, identifier);
        const auto value_size = ruvia::encode_http3_var_int(std::span<char>(wire).subspan(std::get<0>(id_size)), 0);
        const auto decoded = ruvia::decode_http3_settings(
            std::span<const char>(wire).first(std::get<0>(id_size) + std::get<0>(value_size)));
        RUVIA_CHECK(!(decoded.index() == 0));
        if ((decoded.index() != 0)) {
            RUVIA_CHECK(std::get<1>(decoded) == ruvia::http3_settings_error::forbidden_identifier);
        }
    }

    constexpr std::array<char, 1> truncated_identifier{static_cast<char>(0x40)};
    constexpr std::array<char, 2> truncated_value{0x01, static_cast<char>(0x40)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_settings(truncated_identifier)) ==
                ruvia::http3_settings_error::need_more_data);
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_settings(truncated_value)) ==
                ruvia::http3_settings_error::need_more_data);
}

RUVIA_TEST(http3_settings_omits_absent_field_section_limit_but_preserves_explicit_zero) {
    std::array<char, 32> output{};
    const auto default_size = ruvia::encode_http3_settings(output, {});
    RUVIA_CHECK((default_size.index() == 0));
    if ((default_size.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(default_size), std::size_t{4});
    const auto default_decoded = ruvia::decode_http3_settings(std::span(output).first(std::get<0>(default_size)));
    RUVIA_CHECK((default_decoded.index() == 0));
    if ((default_decoded.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(default_decoded).max_field_section_size_.has_value());
        RUVIA_CHECK(!std::get<0>(default_decoded).enable_connect_protocol_);
    }
    const auto explicit_size = ruvia::encode_http3_settings(output, {.max_field_section_size_ = 0});
    RUVIA_CHECK((explicit_size.index() == 0));
    if ((explicit_size.index() == 0)) {
        const auto explicit_decoded = ruvia::decode_http3_settings(std::span(output).first(std::get<0>(explicit_size)));
        RUVIA_CHECK((explicit_decoded.index() == 0));
        if ((explicit_decoded.index() == 0)) {
            RUVIA_CHECK(std::get<0>(explicit_decoded).max_field_section_size_ == std::uint64_t{0});
        }
    }
}

RUVIA_TEST(http3_settings_enable_connect_protocol_uses_canonical_boolean_encoding) {
    constexpr std::array<char, 6> expected{0x01, 0x00, 0x07, 0x00, 0x08, 0x01};
    std::array<char, 16> output{};
    const auto written = ruvia::encode_http3_settings(output, {.enable_connect_protocol_ = true});
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(written), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK_EQ(output[i], expected[i]);
    }
    const auto decoded = ruvia::decode_http3_settings(std::span<const char>(output).first(std::get<0>(written)));
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK(std::get<0>(decoded).enable_connect_protocol_);
    }

    constexpr std::array<char, 2> disabled{0x08, 0x00};
    const auto decoded_disabled = ruvia::decode_http3_settings(disabled);
    RUVIA_CHECK((decoded_disabled.index() == 0));
    if ((decoded_disabled.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(decoded_disabled).enable_connect_protocol_);
    }
    constexpr std::array<char, 2> invalid{0x08, 0x02};
    const auto decoded_invalid = ruvia::decode_http3_settings(invalid);
    RUVIA_CHECK(!(decoded_invalid.index() == 0));
    if ((decoded_invalid.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decoded_invalid) == ruvia::http3_settings_error::value_out_of_range);
    }
    constexpr std::array<char, 4> duplicate{0x08, 0x01, 0x08, 0x00};
    const auto decoded_duplicate = ruvia::decode_http3_settings(duplicate);
    RUVIA_CHECK(!(decoded_duplicate.index() == 0));
    if ((decoded_duplicate.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decoded_duplicate) == ruvia::http3_settings_error::duplicate_identifier);
    }
}

RUVIA_TEST(http3_settings_encoder_rejects_values_out_of_range_and_short_output) {
    std::array<char, 32> output{};
    const ruvia::http3_settings too_large{
        .qpack_max_table_capacity_ = ruvia::http3_var_int_max + 1,
    };
    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_settings(output, too_large)) ==
                ruvia::http3_settings_error::value_out_of_range);

    RUVIA_CHECK(std::get<1>(ruvia::encode_http3_settings(std::span<char>(output).first(1), {})) ==
                ruvia::http3_settings_error::output_too_small);
}
