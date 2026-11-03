#include "ruvia/core/bytes.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "test_harness.h"

RUVIA_TEST(byte_views_preserve_octets_including_embedded_nul) {
    const char raw[] = {'a', '\0', '\xff'};
    const auto text = std::string_view(raw, sizeof(raw));
    const auto bytes_value = ruvia::as_bytes(text);
    RUVIA_CHECK_EQ(bytes_value.size(), std::size_t{3});
    RUVIA_CHECK(bytes_value[0] == std::byte{'a'});
    RUVIA_CHECK(bytes_value[1] == std::byte{0});
    RUVIA_CHECK(bytes_value[2] == std::byte{0xff});
    RUVIA_CHECK(ruvia::as_chars(bytes_value).data() == text.data());
    RUVIA_CHECK_EQ(ruvia::as_chars(bytes_value).size(), text.size());
}

RUVIA_TEST(empty_byte_views_round_trip) {
    RUVIA_CHECK(ruvia::as_bytes({}).empty());
    RUVIA_CHECK(ruvia::as_chars({}).empty());
}

RUVIA_TEST(byte_views_accept_named_owners_and_preserve_storage_addresses) {
    const std::string owned_text("abc", 3);
    const std::pmr::string pmr_text("def", 3);
    const std::vector<std::byte> owned_bytes{std::byte{'x'}, std::byte{0}, std::byte{'z'}};
    const auto text_bytes = ruvia::as_bytes(owned_text);
    const auto pmr_bytes = ruvia::as_bytes(pmr_text);
    const auto chars_value = ruvia::as_chars(owned_bytes);
    RUVIA_CHECK(reinterpret_cast<const char*>(text_bytes.data()) == owned_text.data());
    RUVIA_CHECK(reinterpret_cast<const char*>(pmr_bytes.data()) == pmr_text.data());
    RUVIA_CHECK(chars_value.data() == reinterpret_cast<const char*>(owned_bytes.data()));
    RUVIA_CHECK_EQ(chars_value, std::string_view("x\0z", 3));
}
