#include <algorithm>
#include <array>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_qpack.h"
#include "ruvia/http/http3_qpack_connection.h"

#include "test_harness.h"

namespace {

struct captured_fields final {
    std::vector<std::string> names_;
    std::vector<std::string> values_;
    std::vector<bool> never_indexed_;
};

bool capture(void* opaque, ruvia::http3_field_section_field_view field) {
    auto& captured_value = *static_cast<captured_fields*>(opaque);
    captured_value.names_.emplace_back(field.name_);
    captured_value.values_.emplace_back(field.value_);
    captured_value.never_indexed_.push_back(field.never_indexed_);
    return true;
}

}  // namespace

RUVIA_TEST(http3_field_section_decodes_rfc_9204_appendix_b1) {
    constexpr std::array<char, 15> wire{static_cast<char>(0x00), static_cast<char>(0x00),
        static_cast<char>(0x51), static_cast<char>(0x0b), '/', 'i', 'n', 'd', 'e', 'x', '.', 'h', 't', 'm', 'l'};
    captured_fields captured;
    const auto decoded = ruvia::decode_http3_field_section(wire, capture, &captured);
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(decoded), 1U);
    }
    RUVIA_CHECK_EQ(captured.names_.size(), 1U);
    if (!captured.names_.empty()) {
        RUVIA_CHECK_EQ(captured.names_[0], ":path");
        RUVIA_CHECK_EQ(captured.values_[0], "/index.html");
        RUVIA_CHECK(!captured.never_indexed_[0]);
    }
}

RUVIA_TEST(http3_field_section_round_trips_all_static_zero_capacity_representations) {
    const std::array<ruvia::http3_field_section_field_view, 3> fields_value{{
        {":method", "GET", false},
        {":path", "/custom", true},
        {"custom-name", "", false},
    }};
    std::pmr::monotonic_buffer_resource resource;
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ((std::get<0>(encoded))[0], '\0');
    RUVIA_CHECK_EQ((std::get<0>(encoded))[1], '\0');
    captured_fields captured;
    const auto decoded = ruvia::decode_http3_field_section(std::get<0>(encoded), capture, &captured);
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK_EQ(captured.names_.size(), fields_value.size());
    if (captured.names_.size() == fields_value.size()) {
        for (std::size_t index = 0; index < fields_value.size(); ++index) {
            RUVIA_CHECK_EQ(captured.names_[index], fields_value[index].name_);
            RUVIA_CHECK_EQ(captured.values_[index], fields_value[index].value_);
            RUVIA_CHECK_EQ(captured.never_indexed_[index], fields_value[index].never_indexed_);
        }
    }
}

RUVIA_TEST(http3_qpack_encoders_preserve_static_indexes_and_never_indexed_fields) {
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 4096, .max_blocked_streams_ = 1});
    for (std::size_t index = 0; index < 99; ++index) {
        const auto entry_value = ruvia::get_http3_qpack_static_entry(index);
        RUVIA_CHECK((entry_value.index() == 0));
        if ((entry_value.index() != 0)) {
            continue;
        }
        std::size_t first_name = index;
        std::size_t first_exact = index;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto other = ruvia::get_http3_qpack_static_entry(earlier);
            if (std::get<0>(other).name_ == std::get<0>(entry_value).name_) {
                first_name = std::min(first_name, earlier);
                if (std::get<0>(other).value_ == std::get<0>(entry_value).value_) {
                    first_exact = std::min(first_exact, earlier);
                }
            }
        }
        for (const bool never_indexed : {false, true}) {
            const std::array fields_value{ruvia::http3_field_section_field_view{std::get<0>(entry_value).name_, std::get<0>(entry_value).value_, never_indexed}};
            const auto static_section = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
            const auto connection_section = encoder.encode(0, fields_value);
            RUVIA_CHECK((static_section.index() == 0));
            RUVIA_CHECK((connection_section.index() == 0));
            if ((static_section.index() != 0) || (connection_section.index() != 0)) {
                continue;
            }
            RUVIA_CHECK(std::get<0>(static_section) == std::get<0>(connection_section));
            const auto encoded = std::span<const char>(std::get<0>(static_section)).subspan(2);
            const auto reference = ruvia::decode_http3_qpack_integer(encoded, never_indexed ? 4 : 6);
            RUVIA_CHECK((reference.index() == 0));
            if (reference.index() == 0) {
                RUVIA_CHECK_EQ(std::get<0>(reference).value_, never_indexed ? first_name : first_exact);
            }
            RUVIA_CHECK_EQ(static_cast<unsigned char>(encoded[0]) & (never_indexed ? 0xf0 : 0xc0),
                never_indexed ? 0x70 : 0xc0);
            captured_fields captured;
            const auto decoded = ruvia::decode_http3_field_section(std::get<0>(static_section), capture, &captured);
            RUVIA_CHECK((decoded.index() == 0));
            RUVIA_CHECK_EQ(captured.names_.size(), 1U);
            if (captured.names_.size() == 1) {
                RUVIA_CHECK_EQ(captured.names_.front(), std::get<0>(entry_value).name_);
                RUVIA_CHECK_EQ(captured.values_.front(), std::get<0>(entry_value).value_);
                RUVIA_CHECK_EQ(captured.never_indexed_.front(), never_indexed);
            }
        }
    }
    RUVIA_CHECK_EQ(encoder.insert_count(), 0U);
}

RUVIA_TEST(http3_qpack_encoders_distinguish_literal_names_and_static_name_only_matches) {
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 0});
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CUSTOM", false},
        ruvia::http3_field_section_field_view{"accept", "application/ruvia", false},
        ruvia::http3_field_section_field_view{"Accept", "*/*", false},
        ruvia::http3_field_section_field_view{"x-custom", "value", true},
        ruvia::http3_field_section_field_view{"accept-longer", "*/*", false},
        ruvia::http3_field_section_field_view{"access-control-allow-credentials-extra", "value", false},
        ruvia::http3_field_section_field_view{"", "empty-name-codec-input", false},
    };
    const auto static_section = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
    const auto connection_section = encoder.encode(0, fields_value);
    RUVIA_CHECK((static_section.index() == 0));
    RUVIA_CHECK((connection_section.index() == 0));
    if ((static_section.index() != 0) || (connection_section.index() != 0)) {
        return;
    }
    RUVIA_CHECK(std::get<0>(static_section) == std::get<0>(connection_section));
    captured_fields captured;
    const auto decoded = ruvia::decode_http3_field_section(std::get<0>(static_section), capture, &captured);
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK_EQ(captured.names_.size(), fields_value.size());
    if (captured.names_.size() == fields_value.size()) {
        for (std::size_t index = 0; index < fields_value.size(); ++index) {
            RUVIA_CHECK_EQ(captured.names_[index], fields_value[index].name_);
            RUVIA_CHECK_EQ(captured.values_[index], fields_value[index].value_);
            RUVIA_CHECK_EQ(captured.never_indexed_[index], fields_value[index].never_indexed_);
        }
    }
}

RUVIA_TEST(http3_qpack_encoders_compare_complete_names_before_using_static_references) {
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 0});
    for (std::size_t index = 0; index < 99; ++index) {
        const auto entry_value = ruvia::get_http3_qpack_static_entry(index);
        if ((entry_value.index() != 0) || std::get<0>(entry_value).name_.size() < 3) {
            continue;
        }
        // A different interior byte must not alias a known name with the same
        // length, first byte and last byte. QPACK treats names as opaque bytes.
        std::string name(std::get<0>(entry_value).name_);
        name[1] = '~';
        const std::array fields_value{ruvia::http3_field_section_field_view{name, std::get<0>(entry_value).value_}};
        const auto static_section = ruvia::encode_http3_field_section(fields_value, std::pmr::get_default_resource());
        const auto connection_section = encoder.encode(0, fields_value);
        RUVIA_CHECK((static_section.index() == 0));
        RUVIA_CHECK((connection_section.index() == 0));
        if ((static_section.index() != 0) || (connection_section.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(static_section) == std::get<0>(connection_section));
        RUVIA_CHECK_EQ(static_cast<unsigned char>((std::get<0>(static_section))[2]) & 0xe0, 0x20);
        captured_fields captured;
        const auto decoded = ruvia::decode_http3_field_section(std::get<0>(static_section), capture, &captured);
        RUVIA_CHECK((decoded.index() == 0));
        RUVIA_CHECK_EQ(captured.names_.size(), 1U);
        if (captured.names_.size() == 1) {
            RUVIA_CHECK_EQ(captured.names_.front(), name);
            RUVIA_CHECK_EQ(captured.values_.front(), std::get<0>(entry_value).value_);
        }
    }
}

RUVIA_TEST(http3_field_section_rejects_bad_prefix_dynamic_references_and_truncation) {
    constexpr std::array<char, 2> short_prefix{0, 0};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(std::span<const char>(short_prefix).first(1), nullptr,
                    nullptr)) == ruvia::http3_field_section_error::need_more_data);

    constexpr std::array<char, 3> nonzero_ric{1, 0, 0};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(nonzero_ric, nullptr, nullptr)) ==
                ruvia::http3_field_section_error::nonzero_required_insert_count);
    constexpr std::array<char, 3> invalid_base{0, 1, 0};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(invalid_base, nullptr, nullptr)) ==
                ruvia::http3_field_section_error::invalid_base);
    constexpr std::array<char, 3> dynamic_indexed{0, 0, static_cast<char>(0x80)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(dynamic_indexed, nullptr, nullptr)) ==
                ruvia::http3_field_section_error::dynamic_reference);
    constexpr std::array<char, 3> dynamic_name{0, 0, static_cast<char>(0x40)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(dynamic_name, nullptr, nullptr)) ==
                ruvia::http3_field_section_error::dynamic_reference);
    constexpr std::array<char, 3> truncated{0, 0, static_cast<char>(0x51)};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(truncated, nullptr, nullptr)) ==
                ruvia::http3_field_section_error::need_more_data);
}

RUVIA_TEST(http3_field_section_enforces_field_limits_and_accepts_empty_values) {
    constexpr std::array<char, 4> empty_value{0, 0, static_cast<char>(0x50), 0};
    captured_fields captured;
    const auto decoded = ruvia::decode_http3_field_section(empty_value, capture, &captured);
    RUVIA_CHECK((decoded.index() == 0));
    if (!captured.values_.empty()) {
        RUVIA_CHECK(captured.values_[0].empty());
    }

    constexpr std::array<char, 3> one_field{0, 0, static_cast<char>(0xc1)};
    const ruvia::http3_field_section_limits no_fields{64, 64, 0};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(one_field, nullptr, nullptr, no_fields)) ==
                ruvia::http3_field_section_error::too_many_fields);
    const ruvia::http3_field_section_limits small_wire{2, 64, 10};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(one_field, nullptr, nullptr, small_wire)) ==
                ruvia::http3_field_section_error::field_section_too_large);
    const ruvia::http3_field_section_limits small_list{64, 37, 10};
    RUVIA_CHECK(std::get<1>(ruvia::decode_http3_field_section(one_field, nullptr, nullptr, small_list)) ==
                ruvia::http3_field_section_error::field_list_too_large);
    const ruvia::http3_field_section_limits exact_list{64, 38, 10};
    RUVIA_CHECK((ruvia::decode_http3_field_section(one_field, nullptr, nullptr, exact_list).index() == 0));
}
