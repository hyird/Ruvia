#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/hpack.h"

#include "http2/http2_hpack_static_table.h"
#include "test_harness.h"

namespace {

using ruvia::detail::hpack_static_fields;
using ruvia::detail::hpack_static_header_at;
using ruvia::detail::hpack_static_table_size;

}  // namespace

RUVIA_TEST(hpack_static_table_known_indices) {
    RUVIA_CHECK_EQ(hpack_static_table_size, std::size_t{61});
    // Spot-check normative entries from RFC 7541 Appendix A (1-indexed).
    RUVIA_CHECK_EQ(hpack_static_header_at(1).name_, std::string_view(":authority"));
    RUVIA_CHECK_EQ(hpack_static_header_at(1).value_, std::string_view(""));
    RUVIA_CHECK_EQ(hpack_static_header_at(2).name_, std::string_view(":method"));
    RUVIA_CHECK_EQ(hpack_static_header_at(2).value_, std::string_view("GET"));
    RUVIA_CHECK_EQ(hpack_static_header_at(3).value_, std::string_view("POST"));
    RUVIA_CHECK_EQ(hpack_static_header_at(8).name_, std::string_view(":status"));
    RUVIA_CHECK_EQ(hpack_static_header_at(8).value_, std::string_view("200"));
    RUVIA_CHECK_EQ(hpack_static_header_at(16).name_, std::string_view("accept-encoding"));
    RUVIA_CHECK_EQ(hpack_static_header_at(16).value_, std::string_view("gzip, deflate"));
    RUVIA_CHECK_EQ(hpack_static_header_at(32).name_, std::string_view("cookie"));
    RUVIA_CHECK_EQ(hpack_static_header_at(61).name_, std::string_view("www-authenticate"));
}

RUVIA_TEST(hpack_find_exact_match) {
    auto method = hpack_static_fields.find(":method", "GET");
    RUVIA_CHECK(method.has_value());
    RUVIA_CHECK_EQ(method.value().exact_index_.value(), std::uint8_t{1});
    RUVIA_CHECK_EQ(method.value().name_index_, std::uint8_t{1});

    // A later value for the same name resolves to its own exact index, but the
    // name index still points at the first occurrence of that name.
    auto post = hpack_static_fields.find(":method", "POST");
    RUVIA_CHECK(post.has_value());
    RUVIA_CHECK_EQ(post.value().exact_index_.value(), std::uint8_t{2});
    RUVIA_CHECK_EQ(post.value().name_index_, std::uint8_t{1});

    auto status404 = hpack_static_fields.find(":status", "404");
    RUVIA_CHECK(status404.has_value());
    RUVIA_CHECK_EQ(status404.value().exact_index_.value(), std::uint8_t{12});
    RUVIA_CHECK_EQ(status404.value().name_index_, std::uint8_t{7});

    auto accept_encoding = hpack_static_fields.find("accept-encoding", "gzip, deflate");
    RUVIA_CHECK(accept_encoding.has_value());
    RUVIA_CHECK_EQ(accept_encoding.value().exact_index_.value(), std::uint8_t{15});
    RUVIA_CHECK_EQ(accept_encoding.value().name_index_, std::uint8_t{15});

    // An empty-value entry.
    auto cookie = hpack_static_fields.find("cookie", "");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(cookie.value().exact_index_.value(), std::uint8_t{31});
}

RUVIA_TEST(hpack_find_name_only_match) {
    // Name present, value not in the table -> name index only, no exact index.
    auto method = hpack_static_fields.find(":method", "PUT");
    RUVIA_CHECK(method.has_value());
    RUVIA_CHECK(!method.value().exact_index_.has_value());
    RUVIA_CHECK_EQ(method.value().name_index_, std::uint8_t{1});

    auto status = hpack_static_fields.find(":status", "201");
    RUVIA_CHECK(status.has_value());
    RUVIA_CHECK(!status.value().exact_index_.has_value());
    RUVIA_CHECK_EQ(status.value().name_index_, std::uint8_t{7});
}

RUVIA_TEST(hpack_find_no_match) {
    auto custom_value = hpack_static_fields.find("x-custom-header", "value");
    RUVIA_CHECK(!custom_value.has_value());
}

RUVIA_TEST(hpack_static_lookup_preserves_first_matches_for_similar_field_names) {
    const auto check = [&](std::string_view name, std::string_view value) {
        std::uint32_t expected_name = 0;
        std::uint32_t expected_exact = 0;
        for (std::uint32_t index = 1; index <= hpack_static_table_size; ++index) {
            const auto& entry_value = hpack_static_header_at(index);
            if (entry_value.name_ != name) {
                continue;
            }
            if (expected_name == 0) {
                expected_name = index;
            }
            if (entry_value.value_ == value) {
                expected_exact = index;
                break;
            }
        }
        const auto actual = hpack_static_fields.find(name, value);
        RUVIA_CHECK_EQ(actual ? static_cast<std::uint32_t>(actual->name_index_) + 1 : 0, expected_name);
        RUVIA_CHECK_EQ(actual && actual->exact_index_ ? static_cast<std::uint32_t>(*actual->exact_index_) + 1 : 0, expected_exact);
    };

    check({}, {});
    check({}, "value");
    for (std::uint32_t index = 1; index <= hpack_static_table_size; ++index) {
        const auto& entry_value = hpack_static_header_at(index);
        check(entry_value.name_, entry_value.value_);
        check(entry_value.name_, "not-a-static-value");
        check(std::string(entry_value.name_) + "-extra", entry_value.value_);
        for (const auto position : {std::size_t{0}, entry_value.name_.size() / 2, entry_value.name_.size() - 1}) {
            std::string name(entry_value.name_);
            for (unsigned byte = 0; byte < 256; ++byte) {
                name[position] = static_cast<char>(byte);
                check(name, entry_value.value_);
            }
        }
    }
}

RUVIA_TEST(hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::hpack_decoder decoder;
    for (std::uint32_t index = 1; index <= hpack_static_table_size; ++index) {
        const auto& entry_value = hpack_static_header_at(index);
        std::pmr::string encoded(std::pmr::get_default_resource());
        ruvia::hpack_encoder::encode_header(encoded, entry_value.name_, entry_value.value_);
        RUVIA_CHECK_EQ(encoded.size(), std::size_t{1});
        if (!encoded.empty()) {
            RUVIA_CHECK_EQ(static_cast<unsigned char>(encoded.front()), 0x80U | index);
        }
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(encoded, [&](std::string_view name, std::string_view value) {
            ++fields_value;
            RUVIA_CHECK_EQ(name, entry_value.name_);
            RUVIA_CHECK_EQ(value, entry_value.value_);
            return true;
        });
        RUVIA_CHECK(decoded.decoded());
        RUVIA_CHECK_EQ(fields_value, std::size_t{1});
    }
}
