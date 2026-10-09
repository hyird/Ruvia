#include <algorithm>
#include <array>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Qpack.h"
#include "ruvia/http/Http3QpackConnection.h"

#include "test_harness.h"

namespace {

struct CapturedFields final {
    std::vector<std::string> names;
    std::vector<std::string> values;
    std::vector<bool> neverIndexed;
};

bool capture(void* opaque, ruvia::Http3FieldSectionFieldView field) {
    auto& captured = *static_cast<CapturedFields*>(opaque);
    captured.names.emplace_back(field.name);
    captured.values.emplace_back(field.value);
    captured.neverIndexed.push_back(field.neverIndexed);
    return true;
}

}  // namespace

RUVIA_TEST(http3_field_section_decodes_rfc_9204_appendix_b1) {
    constexpr std::array<char, 15> wire{static_cast<char>(0x00), static_cast<char>(0x00),
        static_cast<char>(0x51), static_cast<char>(0x0b), '/', 'i', 'n', 'd', 'e', 'x', '.', 'h', 't', 'm', 'l'};
    CapturedFields captured;
    const auto decoded = ruvia::decodeHttp3FieldSection(wire, capture, &captured);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(*decoded, 1U);
    }
    RUVIA_CHECK_EQ(captured.names.size(), 1U);
    if (!captured.names.empty()) {
        RUVIA_CHECK_EQ(captured.names[0], ":path");
        RUVIA_CHECK_EQ(captured.values[0], "/index.html");
        RUVIA_CHECK(!captured.neverIndexed[0]);
    }
}

RUVIA_TEST(http3_field_section_round_trips_all_static_zero_capacity_representations) {
    const std::array<ruvia::Http3FieldSectionFieldView, 3> fields{{
        {":method", "GET", false},
        {":path", "/custom", true},
        {"custom-name", "", false},
    }};
    std::pmr::monotonic_buffer_resource resource;
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource);
    RUVIA_CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    RUVIA_CHECK_EQ((*encoded)[0], '\0');
    RUVIA_CHECK_EQ((*encoded)[1], '\0');
    CapturedFields captured;
    const auto decoded = ruvia::decodeHttp3FieldSection(*encoded, capture, &captured);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(captured.names.size(), fields.size());
    if (captured.names.size() == fields.size()) {
        for (std::size_t index = 0; index < fields.size(); ++index) {
            RUVIA_CHECK_EQ(captured.names[index], fields[index].name);
            RUVIA_CHECK_EQ(captured.values[index], fields[index].value);
            RUVIA_CHECK_EQ(captured.neverIndexed[index], fields[index].neverIndexed);
        }
    }
}

RUVIA_TEST(http3_qpack_encoders_preserve_static_indexes_and_never_indexed_fields) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 4096, .maxBlockedStreams = 1});
    for (std::size_t index = 0; index < 99; ++index) {
        const auto entry = ruvia::http3QpackStaticEntry(index);
        RUVIA_CHECK(entry.has_value());
        if (!entry) {
            continue;
        }
        std::size_t first_name = index;
        std::size_t first_exact = index;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto other = ruvia::http3QpackStaticEntry(earlier);
            if (other->name == entry->name) {
                first_name = std::min(first_name, earlier);
                if (other->value == entry->value) {
                    first_exact = std::min(first_exact, earlier);
                }
            }
        }
        for (const bool never_indexed : {false, true}) {
            const std::array fields{ruvia::Http3FieldSectionFieldView{entry->name, entry->value, never_indexed}};
            const auto static_section = ruvia::encodeHttp3FieldSection(fields, std::pmr::get_default_resource());
            const auto connection_section = encoder.encode(0, fields);
            RUVIA_CHECK(static_section.has_value());
            RUVIA_CHECK(connection_section.has_value());
            if (!static_section || !connection_section) {
                continue;
            }
            RUVIA_CHECK(*static_section == *connection_section);
            const auto encoded = std::span<const char>(*static_section).subspan(2);
            const auto reference = ruvia::decodeHttp3QpackInteger(encoded, never_indexed ? 4 : 6);
            RUVIA_CHECK(reference.has_value());
            if (reference) {
                RUVIA_CHECK_EQ(reference->value, never_indexed ? first_name : first_exact);
            }
            RUVIA_CHECK_EQ(static_cast<unsigned char>(encoded[0]) & (never_indexed ? 0xf0 : 0xc0),
                never_indexed ? 0x70 : 0xc0);
            CapturedFields captured;
            const auto decoded = ruvia::decodeHttp3FieldSection(*static_section, capture, &captured);
            RUVIA_CHECK(decoded.has_value());
            RUVIA_CHECK_EQ(captured.names.size(), 1U);
            if (captured.names.size() == 1) {
                RUVIA_CHECK_EQ(captured.names.front(), entry->name);
                RUVIA_CHECK_EQ(captured.values.front(), entry->value);
                RUVIA_CHECK_EQ(captured.neverIndexed.front(), never_indexed);
            }
        }
    }
    RUVIA_CHECK_EQ(encoder.insertCount(), 0U);
}

RUVIA_TEST(http3_qpack_encoders_distinguish_literal_names_and_static_name_only_matches) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 0});
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{":method", "CUSTOM", false},
        ruvia::Http3FieldSectionFieldView{"accept", "application/ruvia", false},
        ruvia::Http3FieldSectionFieldView{"Accept", "*/*", false},
        ruvia::Http3FieldSectionFieldView{"x-custom", "value", true},
        ruvia::Http3FieldSectionFieldView{"accept-longer", "*/*", false},
        ruvia::Http3FieldSectionFieldView{"access-control-allow-credentials-extra", "value", false},
        ruvia::Http3FieldSectionFieldView{"", "empty-name-codec-input", false},
    };
    const auto static_section = ruvia::encodeHttp3FieldSection(fields, std::pmr::get_default_resource());
    const auto connection_section = encoder.encode(0, fields);
    RUVIA_CHECK(static_section.has_value());
    RUVIA_CHECK(connection_section.has_value());
    if (!static_section || !connection_section) {
        return;
    }
    RUVIA_CHECK(*static_section == *connection_section);
    CapturedFields captured;
    const auto decoded = ruvia::decodeHttp3FieldSection(*static_section, capture, &captured);
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(captured.names.size(), fields.size());
    if (captured.names.size() == fields.size()) {
        for (std::size_t index = 0; index < fields.size(); ++index) {
            RUVIA_CHECK_EQ(captured.names[index], fields[index].name);
            RUVIA_CHECK_EQ(captured.values[index], fields[index].value);
            RUVIA_CHECK_EQ(captured.neverIndexed[index], fields[index].neverIndexed);
        }
    }
}

RUVIA_TEST(http3_qpack_encoders_compare_complete_names_before_using_static_references) {
    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 0});
    for (std::size_t index = 0; index < 99; ++index) {
        const auto entry = ruvia::http3QpackStaticEntry(index);
        if (!entry || entry->name.size() < 3) {
            continue;
        }
        // A different interior byte must not alias a known name with the same
        // length, first byte and last byte. QPACK treats names as opaque bytes.
        std::string name(entry->name);
        name[1] = '~';
        const std::array fields{ruvia::Http3FieldSectionFieldView{name, entry->value}};
        const auto static_section = ruvia::encodeHttp3FieldSection(fields, std::pmr::get_default_resource());
        const auto connection_section = encoder.encode(0, fields);
        RUVIA_CHECK(static_section.has_value());
        RUVIA_CHECK(connection_section.has_value());
        if (!static_section || !connection_section) {
            continue;
        }
        RUVIA_CHECK(*static_section == *connection_section);
        RUVIA_CHECK_EQ(static_cast<unsigned char>((*static_section)[2]) & 0xe0, 0x20);
        CapturedFields captured;
        const auto decoded = ruvia::decodeHttp3FieldSection(*static_section, capture, &captured);
        RUVIA_CHECK(decoded.has_value());
        RUVIA_CHECK_EQ(captured.names.size(), 1U);
        if (captured.names.size() == 1) {
            RUVIA_CHECK_EQ(captured.names.front(), name);
            RUVIA_CHECK_EQ(captured.values.front(), entry->value);
        }
    }
}

RUVIA_TEST(http3_field_section_rejects_bad_prefix_dynamic_references_and_truncation) {
    constexpr std::array<char, 2> shortPrefix{0, 0};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(std::span<const char>(shortPrefix).first(1), nullptr,
                    nullptr)
                    .error() == ruvia::Http3FieldSectionError::kNeedMoreData);

    constexpr std::array<char, 3> nonzeroRic{1, 0, 0};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(nonzeroRic, nullptr, nullptr).error() ==
                ruvia::Http3FieldSectionError::kNonzeroRequiredInsertCount);
    constexpr std::array<char, 3> invalidBase{0, 1, 0};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(invalidBase, nullptr, nullptr).error() ==
                ruvia::Http3FieldSectionError::kInvalidBase);
    constexpr std::array<char, 3> dynamicIndexed{0, 0, static_cast<char>(0x80)};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(dynamicIndexed, nullptr, nullptr).error() ==
                ruvia::Http3FieldSectionError::kDynamicReference);
    constexpr std::array<char, 3> dynamicName{0, 0, static_cast<char>(0x40)};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(dynamicName, nullptr, nullptr).error() ==
                ruvia::Http3FieldSectionError::kDynamicReference);
    constexpr std::array<char, 3> truncated{0, 0, static_cast<char>(0x51)};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(truncated, nullptr, nullptr).error() ==
                ruvia::Http3FieldSectionError::kNeedMoreData);
}

RUVIA_TEST(http3_field_section_enforces_field_limits_and_accepts_empty_values) {
    constexpr std::array<char, 4> emptyValue{0, 0, static_cast<char>(0x50), 0};
    CapturedFields captured;
    const auto decoded = ruvia::decodeHttp3FieldSection(emptyValue, capture, &captured);
    RUVIA_CHECK(decoded.has_value());
    if (!captured.values.empty()) {
        RUVIA_CHECK(captured.values[0].empty());
    }

    constexpr std::array<char, 3> oneField{0, 0, static_cast<char>(0xc1)};
    const ruvia::Http3FieldSectionLimits noFields{64, 64, 0};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(oneField, nullptr, nullptr, noFields).error() ==
                ruvia::Http3FieldSectionError::kTooManyFields);
    const ruvia::Http3FieldSectionLimits smallWire{2, 64, 10};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(oneField, nullptr, nullptr, smallWire).error() ==
                ruvia::Http3FieldSectionError::kFieldSectionTooLarge);
    const ruvia::Http3FieldSectionLimits smallList{64, 37, 10};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(oneField, nullptr, nullptr, smallList).error() ==
                ruvia::Http3FieldSectionError::kFieldListTooLarge);
    const ruvia::Http3FieldSectionLimits exactList{64, 38, 10};
    RUVIA_CHECK(ruvia::decodeHttp3FieldSection(oneField, nullptr, nullptr, exactList).has_value());
}
