#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <variant>

#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/Http3VarInt.h"

#include "test_harness.h"

RUVIA_TEST(http3_settings_round_trip_known_values_and_disable_dynamic_table) {
    const ruvia::Http3Settings settings{
        .qpackMaxTableCapacity = 0,
        .maxFieldSectionSize = ruvia::kHttp3VarIntMax,
        .qpackBlockedStreams = (std::uint64_t{1} << 30),
        .enableConnectProtocol = true,
    };
    std::array<char, 32> wire{};
    const auto written = ruvia::encodeHttp3Settings(wire, settings);
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    std::pmr::monotonic_buffer_resource resource;
    const auto decoded = ruvia::decodeHttp3Settings(std::span<const char>(wire).first(std::get<0>(written)), &resource);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(decoded).qpackMaxTableCapacity, std::uint64_t{0});
        RUVIA_CHECK_EQ(std::get<0>(decoded).maxFieldSectionSize, settings.maxFieldSectionSize);
        RUVIA_CHECK_EQ(std::get<0>(decoded).qpackBlockedStreams, settings.qpackBlockedStreams);
        RUVIA_CHECK(std::get<0>(decoded).enableConnectProtocol);
    }
}

RUVIA_TEST(http3_settings_skips_unknown_ids_but_rejects_duplicates_everywhere) {
    constexpr std::array<char, 8> unknownAndKnown{0x20, 0x2a, 0x01, 0x05, 0x21, 0x00, 0x01, 0x09};
    const auto duplicate = ruvia::decodeHttp3Settings(unknownAndKnown);
    RUVIA_CHECK(!(duplicate.index() == 0));
    if ((duplicate.index() != 0)) {
        RUVIA_CHECK(std::get<1>(duplicate) == ruvia::Http3SettingsError::kDuplicateIdentifier);
    }

    constexpr std::array<char, 6> duplicateUnknown{0x20, 0x2a, 0x21, 0x01, 0x20, 0x09};
    const auto repeatedUnknown = ruvia::decodeHttp3Settings(duplicateUnknown);
    RUVIA_CHECK(!(repeatedUnknown.index() == 0));
    if ((repeatedUnknown.index() != 0)) {
        RUVIA_CHECK(std::get<1>(repeatedUnknown) == ruvia::Http3SettingsError::kDuplicateIdentifier);
    }

    constexpr std::array<char, 6> unknownOnly{0x20, 0x2a, 0x21, 0x01, 0x22, 0x00};
    const auto skipped = ruvia::decodeHttp3Settings(unknownOnly);
    RUVIA_CHECK((skipped.index() == 0));
    if ((skipped.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(skipped).qpackMaxTableCapacity, std::uint64_t{0});
        RUVIA_CHECK(!std::get<0>(skipped).maxFieldSectionSize.has_value());
        RUVIA_CHECK_EQ(std::get<0>(skipped).qpackBlockedStreams, std::uint64_t{0});
    }
}

RUVIA_TEST(http3_settings_rejects_forbidden_ids_and_truncated_varints) {
    for (std::uint64_t identifier = 0; identifier <= 0x5; ++identifier) {
        if (identifier == 0x1) {
            continue;
        }
        std::array<char, 16> wire{};
        const auto idSize = ruvia::encodeHttp3VarInt(wire, identifier);
        const auto valueSize = ruvia::encodeHttp3VarInt(std::span<char>(wire).subspan(std::get<0>(idSize)), 0);
        const auto decoded = ruvia::decodeHttp3Settings(
            std::span<const char>(wire).first(std::get<0>(idSize) + std::get<0>(valueSize)));
        RUVIA_CHECK(!(decoded.index() == 0));
        if ((decoded.index() != 0)) {
            RUVIA_CHECK(std::get<1>(decoded) == ruvia::Http3SettingsError::kForbiddenIdentifier);
        }
    }

    constexpr std::array<char, 1> truncatedIdentifier{static_cast<char>(0x40)};
    constexpr std::array<char, 2> truncatedValue{0x01, static_cast<char>(0x40)};
    RUVIA_CHECK(std::get<1>(ruvia::decodeHttp3Settings(truncatedIdentifier)) ==
                ruvia::Http3SettingsError::kNeedMoreData);
    RUVIA_CHECK(std::get<1>(ruvia::decodeHttp3Settings(truncatedValue)) ==
                ruvia::Http3SettingsError::kNeedMoreData);
}

RUVIA_TEST(http3_settings_omits_absent_field_section_limit_but_preserves_explicit_zero) {
    std::array<char, 32> output{};
    const auto defaultSize = ruvia::encodeHttp3Settings(output, {});
    RUVIA_CHECK((defaultSize.index() == 0));
    if ((defaultSize.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(defaultSize), std::size_t{4});
    const auto defaultDecoded = ruvia::decodeHttp3Settings(std::span(output).first(std::get<0>(defaultSize)));
    RUVIA_CHECK((defaultDecoded.index() == 0));
    if ((defaultDecoded.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(defaultDecoded).maxFieldSectionSize.has_value());
        RUVIA_CHECK(!std::get<0>(defaultDecoded).enableConnectProtocol);
    }
    const auto explicitSize = ruvia::encodeHttp3Settings(output, {.maxFieldSectionSize = 0});
    RUVIA_CHECK((explicitSize.index() == 0));
    if ((explicitSize.index() == 0)) {
        const auto explicitDecoded = ruvia::decodeHttp3Settings(std::span(output).first(std::get<0>(explicitSize)));
        RUVIA_CHECK((explicitDecoded.index() == 0));
        if ((explicitDecoded.index() == 0)) {
            RUVIA_CHECK(std::get<0>(explicitDecoded).maxFieldSectionSize == std::uint64_t{0});
        }
    }
}

RUVIA_TEST(http3_settings_enable_connect_protocol_uses_canonical_boolean_encoding) {
    constexpr std::array<char, 6> expected{0x01, 0x00, 0x07, 0x00, 0x08, 0x01};
    std::array<char, 16> output{};
    const auto written = ruvia::encodeHttp3Settings(output, {.enableConnectProtocol = true});
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(written), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK_EQ(output[i], expected[i]);
    }
    const auto decoded = ruvia::decodeHttp3Settings(std::span<const char>(output).first(std::get<0>(written)));
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK(std::get<0>(decoded).enableConnectProtocol);
    }

    constexpr std::array<char, 2> disabled{0x08, 0x00};
    const auto decodedDisabled = ruvia::decodeHttp3Settings(disabled);
    RUVIA_CHECK((decodedDisabled.index() == 0));
    if ((decodedDisabled.index() == 0)) {
        RUVIA_CHECK(!std::get<0>(decodedDisabled).enableConnectProtocol);
    }
    constexpr std::array<char, 2> invalid{0x08, 0x02};
    const auto decodedInvalid = ruvia::decodeHttp3Settings(invalid);
    RUVIA_CHECK(!(decodedInvalid.index() == 0));
    if ((decodedInvalid.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decodedInvalid) == ruvia::Http3SettingsError::kValueOutOfRange);
    }
    constexpr std::array<char, 4> duplicate{0x08, 0x01, 0x08, 0x00};
    const auto decodedDuplicate = ruvia::decodeHttp3Settings(duplicate);
    RUVIA_CHECK(!(decodedDuplicate.index() == 0));
    if ((decodedDuplicate.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decodedDuplicate) == ruvia::Http3SettingsError::kDuplicateIdentifier);
    }
}

RUVIA_TEST(http3_settings_encoder_rejects_values_out_of_range_and_short_output) {
    std::array<char, 32> output{};
    const ruvia::Http3Settings tooLarge{
        .qpackMaxTableCapacity = ruvia::kHttp3VarIntMax + 1,
    };
    RUVIA_CHECK(std::get<1>(ruvia::encodeHttp3Settings(output, tooLarge)) ==
                ruvia::Http3SettingsError::kValueOutOfRange);

    RUVIA_CHECK(std::get<1>(ruvia::encodeHttp3Settings(std::span<char>(output).first(1), {})) ==
                ruvia::Http3SettingsError::kOutputTooSmall);
}
