#include "ruvia/core/Bytes.h"

#include <cstddef>
#include <string_view>

#include "test_harness.h"

RUVIA_TEST(byte_views_preserve_octets_including_embedded_nul) {
    const char raw[] = {'a', '\0', '\xff'};
    const auto text = std::string_view(raw, sizeof(raw));
    const auto bytes = ruvia::asBytes(text);
    RUVIA_CHECK_EQ(bytes.size(), std::size_t{3});
    RUVIA_CHECK(bytes[0] == std::byte{'a'});
    RUVIA_CHECK(bytes[1] == std::byte{0});
    RUVIA_CHECK(bytes[2] == std::byte{0xff});
    RUVIA_CHECK(ruvia::asChars(bytes).data() == text.data());
    RUVIA_CHECK_EQ(ruvia::asChars(bytes).size(), text.size());
}

RUVIA_TEST(empty_byte_views_round_trip) {
    RUVIA_CHECK(ruvia::asBytes({}).empty());
    RUVIA_CHECK(ruvia::asChars({}).empty());
}
