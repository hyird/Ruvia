#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_chunked_framing.h"

#include "test_harness.h"

RUVIA_TEST(http1_chunk_header_encodes_lowercase_hex_and_crlf) {
    const ruvia::http1_chunk_header zero(0);
    const ruvia::http1_chunk_header fifteen(15);
    const ruvia::http1_chunk_header sixteen(16);
    const ruvia::http1_chunk_header abc(0xabc);
    RUVIA_CHECK_EQ(zero.view(), std::string_view("0\r\n"));
    RUVIA_CHECK_EQ(fifteen.view(), std::string_view("f\r\n"));
    RUVIA_CHECK_EQ(sixteen.view(), std::string_view("10\r\n"));
    RUVIA_CHECK_EQ(abc.view(), std::string_view("abc\r\n"));
}

RUVIA_TEST(http1_chunk_header_buffer_covers_size_t_max) {
    const ruvia::http1_chunk_header header_value((std::numeric_limits<std::size_t>::max)());
    const auto encoded = header_value.view();
    RUVIA_CHECK_EQ(encoded.size(), sizeof(std::size_t) * 2 + 2);
    RUVIA_CHECK(encoded.ends_with("\r\n"));
}
