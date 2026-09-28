#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>

#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/Http3VarInt.h"

#include "test_harness.h"

RUVIA_TEST(http3_settings_round_trip_known_values_and_disable_dynamic_table) {
    const ruvia::Http3Settings settings{
        .qpackMaxTableCapacity = 0,
        .maxFieldSectionSize = ruvia::kHttp3VarIntMax,
        .qpackBlockedStreams = (std::uint64_t{1} << 30),
    };
    std::array<char, 32> wire{};
    const auto written = ruvia::encodeHttp3Settings(wire, settings);
    RUVIA_CHECK(written.has_value());
    if (!written) {
        return;
    }
    std::pmr::monotonic_buffer_resource resource;
    const auto decoded = ruvia::decodeHttp3Settings(std::span<const char>(wire).first(*written), &resource);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(decoded->qpackMaxTableCapacity, std::uint64_t{0});
        RUVIA_CHECK_EQ(decoded->maxFieldSectionSize, settings.maxFieldSectionSize);
        RUVIA_CHECK_EQ(decoded->qpackBlockedStreams, settings.qpackBlockedStreams);
    }
}

RUVIA_TEST(http3_settings_skips_unknown_ids_but_rejects_duplicates_everywhere) {
    constexpr std::array<char, 8> unknownAndKnown{0x20, 0x2a, 0x01, 0x05, 0x21, 0x00, 0x01, 0x09};
    const auto duplicate = ruvia::decodeHttp3Settings(unknownAndKnown);
    RUVIA_CHECK(!duplicate.has_value());
    if (!duplicate) {
        RUVIA_CHECK(duplicate.error() == ruvia::Http3SettingsError::kDuplicateIdentifier);
    }

    constexpr std::array<char, 6> duplicateUnknown{0x20, 0x2a, 0x21, 0x01, 0x20, 0x09};
    const auto repeatedUnknown = ruvia::decodeHttp3Settings(duplicateUnknown);
    RUVIA_CHECK(!repeatedUnknown.has_value());
    if (!repeatedUnknown) {
        RUVIA_CHECK(repeatedUnknown.error() == ruvia::Http3SettingsError::kDuplicateIdentifier);
    }

    constexpr std::array<char, 6> unknownOnly{0x20, 0x2a, 0x21, 0x01, 0x22, 0x00};
    const auto skipped = ruvia::decodeHttp3Settings(unknownOnly);
    RUVIA_CHECK(skipped.has_value());
    if (skipped) {
        RUVIA_CHECK_EQ(skipped->qpackMaxTableCapacity, std::uint64_t{0});
        RUVIA_CHECK(!skipped->maxFieldSectionSize.has_value());
        RUVIA_CHECK_EQ(skipped->qpackBlockedStreams, std::uint64_t{0});
    }
}

RUVIA_TEST(http3_settings_rejects_forbidden_ids_and_truncated_varints) {
    for (std::uint64_t identifier = 0; identifier <= 0x5; ++identifier) {
        if (identifier == 0x1) {
            continue;
        }
        std::array<char, 16> wire{};
        const auto idSize = ruvia::encodeHttp3VarInt(wire, identifier);
        const auto valueSize = ruvia::encodeHttp3VarInt(std::span<char>(wire).subspan(*idSize), 0);
        const auto decoded = ruvia::decodeHttp3Settings(
            std::span<const char>(wire).first(*idSize + *valueSize));
        RUVIA_CHECK(!decoded.has_value());
        if (!decoded) {
            RUVIA_CHECK(decoded.error() == ruvia::Http3SettingsError::kForbiddenIdentifier);
        }
    }

    constexpr std::array<char, 1> truncatedIdentifier{static_cast<char>(0x40)};
    constexpr std::array<char, 2> truncatedValue{0x01, static_cast<char>(0x40)};
    RUVIA_CHECK(ruvia::decodeHttp3Settings(truncatedIdentifier).error() ==
                ruvia::Http3SettingsError::kNeedMoreData);
    RUVIA_CHECK(ruvia::decodeHttp3Settings(truncatedValue).error() ==
                ruvia::Http3SettingsError::kNeedMoreData);
}

RUVIA_TEST(http3_settings_omits_absent_field_section_limit_but_preserves_explicit_zero) {
    std::array<char, 32> output{};
    const auto defaultSize = ruvia::encodeHttp3Settings(output, {});
    RUVIA_CHECK(defaultSize.has_value());
    if (!defaultSize) {
        return;
    }
    const auto defaultDecoded = ruvia::decodeHttp3Settings(std::span(output).first(*defaultSize));
    RUVIA_CHECK(defaultDecoded.has_value());
    if (defaultDecoded) {
        RUVIA_CHECK(!defaultDecoded->maxFieldSectionSize.has_value());
    }
    const auto explicitSize = ruvia::encodeHttp3Settings(output, {.maxFieldSectionSize = 0});
    RUVIA_CHECK(explicitSize.has_value());
    if (explicitSize) {
        const auto explicitDecoded = ruvia::decodeHttp3Settings(std::span(output).first(*explicitSize));
        RUVIA_CHECK(explicitDecoded.has_value());
        if (explicitDecoded) {
            RUVIA_CHECK(explicitDecoded->maxFieldSectionSize == std::uint64_t{0});
        }
    }
}

RUVIA_TEST(http3_settings_encoder_rejects_values_out_of_range_and_short_output) {
    std::array<char, 32> output{};
    const ruvia::Http3Settings tooLarge{
        .qpackMaxTableCapacity = ruvia::kHttp3VarIntMax + 1,
    };
    RUVIA_CHECK(ruvia::encodeHttp3Settings(output, tooLarge).error() ==
                ruvia::Http3SettingsError::kValueOutOfRange);

    RUVIA_CHECK(ruvia::encodeHttp3Settings(std::span<char>(output).first(1), {}).error() ==
                ruvia::Http3SettingsError::kOutputTooSmall);
}
