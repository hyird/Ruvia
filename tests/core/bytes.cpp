#include "ruvia/core/bytes.h"

#include <cstddef>
#include <string_view>

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
