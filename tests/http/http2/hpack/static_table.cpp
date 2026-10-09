#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/Hpack.h"

#include "http2/Http2HpackStaticTable.h"
#include "test_harness.h"

namespace {

using ruvia::detail::hpack_static_fields;
using ruvia::detail::hpackStaticHeaderAt;
using ruvia::detail::kHpackStaticTableSize;

}  // namespace

RUVIA_TEST(hpack_static_table_known_indices) {
    RUVIA_CHECK_EQ(kHpackStaticTableSize, std::size_t{61});
    // Spot-check normative entries from RFC 7541 Appendix A (1-indexed).
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(1).name, std::string_view(":authority"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(1).value, std::string_view(""));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(2).name, std::string_view(":method"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(2).value, std::string_view("GET"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(3).value, std::string_view("POST"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(8).name, std::string_view(":status"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(8).value, std::string_view("200"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(16).name, std::string_view("accept-encoding"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(16).value, std::string_view("gzip, deflate"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(32).name, std::string_view("cookie"));
    RUVIA_CHECK_EQ(hpackStaticHeaderAt(61).name, std::string_view("www-authenticate"));
}

RUVIA_TEST(hpack_find_exact_match) {
    auto method = hpack_static_fields.find(":method", "GET");
    RUVIA_CHECK(method.has_value());
    RUVIA_CHECK_EQ(method.value().exact_index.value(), std::uint8_t{1});
    RUVIA_CHECK_EQ(method.value().name_index, std::uint8_t{1});

    // A later value for the same name resolves to its own exact index, but the
    // name index still points at the first occurrence of that name.
    auto post = hpack_static_fields.find(":method", "POST");
    RUVIA_CHECK(post.has_value());
    RUVIA_CHECK_EQ(post.value().exact_index.value(), std::uint8_t{2});
    RUVIA_CHECK_EQ(post.value().name_index, std::uint8_t{1});

    auto status404 = hpack_static_fields.find(":status", "404");
    RUVIA_CHECK(status404.has_value());
    RUVIA_CHECK_EQ(status404.value().exact_index.value(), std::uint8_t{12});
    RUVIA_CHECK_EQ(status404.value().name_index, std::uint8_t{7});

    auto acceptEncoding = hpack_static_fields.find("accept-encoding", "gzip, deflate");
    RUVIA_CHECK(acceptEncoding.has_value());
    RUVIA_CHECK_EQ(acceptEncoding.value().exact_index.value(), std::uint8_t{15});
    RUVIA_CHECK_EQ(acceptEncoding.value().name_index, std::uint8_t{15});

    // An empty-value entry.
    auto cookie = hpack_static_fields.find("cookie", "");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(cookie.value().exact_index.value(), std::uint8_t{31});
}

RUVIA_TEST(hpack_find_name_only_match) {
    // Name present, value not in the table -> name index only, no exact index.
    auto method = hpack_static_fields.find(":method", "PUT");
    RUVIA_CHECK(method.has_value());
    RUVIA_CHECK(!method.value().exact_index.has_value());
    RUVIA_CHECK_EQ(method.value().name_index, std::uint8_t{1});

    auto status = hpack_static_fields.find(":status", "201");
    RUVIA_CHECK(status.has_value());
    RUVIA_CHECK(!status.value().exact_index.has_value());
    RUVIA_CHECK_EQ(status.value().name_index, std::uint8_t{7});
}

RUVIA_TEST(hpack_find_no_match) {
    auto custom = hpack_static_fields.find("x-custom-header", "value");
    RUVIA_CHECK(!custom.has_value());
}

RUVIA_TEST(hpack_static_lookup_preserves_first_matches_for_similar_field_names) {
    const auto check = [&](std::string_view name, std::string_view value) {
        std::uint32_t expected_name = 0;
        std::uint32_t expected_exact = 0;
        for (std::uint32_t index = 1; index <= kHpackStaticTableSize; ++index) {
            const auto& entry = hpackStaticHeaderAt(index);
            if (entry.name != name) {
                continue;
            }
            if (expected_name == 0) {
                expected_name = index;
            }
            if (entry.value == value) {
                expected_exact = index;
                break;
            }
        }
        const auto actual = hpack_static_fields.find(name, value);
        RUVIA_CHECK_EQ(actual ? static_cast<std::uint32_t>(actual->name_index) + 1 : 0, expected_name);
        RUVIA_CHECK_EQ(actual && actual->exact_index ? static_cast<std::uint32_t>(*actual->exact_index) + 1 : 0, expected_exact);
    };

    check({}, {});
    check({}, "value");
    for (std::uint32_t index = 1; index <= kHpackStaticTableSize; ++index) {
        const auto& entry = hpackStaticHeaderAt(index);
        check(entry.name, entry.value);
        check(entry.name, "not-a-static-value");
        check(std::string(entry.name) + "-extra", entry.value);
        for (const auto position : {std::size_t{0}, entry.name.size() / 2, entry.name.size() - 1}) {
            std::string name(entry.name);
            for (unsigned byte = 0; byte < 256; ++byte) {
                name[position] = static_cast<char>(byte);
                check(name, entry.value);
            }
        }
    }
}

RUVIA_TEST(hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::HpackDecoder decoder;
    for (std::uint32_t index = 1; index <= kHpackStaticTableSize; ++index) {
        const auto& entry = hpackStaticHeaderAt(index);
        std::pmr::string encoded(std::pmr::get_default_resource());
        ruvia::HpackEncoder::encodeHeader(encoded, entry.name, entry.value);
        RUVIA_CHECK_EQ(encoded.size(), std::size_t{1});
        if (!encoded.empty()) {
            RUVIA_CHECK_EQ(static_cast<unsigned char>(encoded.front()), 0x80U | index);
        }
        std::size_t fields = 0;
        const auto decoded = decoder.decode(encoded, [&](std::string_view name, std::string_view value) {
            ++fields;
            RUVIA_CHECK_EQ(name, entry.name);
            RUVIA_CHECK_EQ(value, entry.value);
            return true;
        });
        RUVIA_CHECK(decoded.decoded());
        RUVIA_CHECK_EQ(fields, std::size_t{1});
    }
}
