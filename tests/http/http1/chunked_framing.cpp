#include <array>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_chunked_framing.h"

#include "http1/http1_chunked_framing.h"
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

RUVIA_TEST(http1_chunk_trailer_serialization_is_protocol_owned) {
    std::pmr::string trailers(std::pmr::get_default_resource());
    const std::array<ruvia::http_header_view, 2> fields_value{
        {{"Digest", "sha-256=value"}, {"X-Trace", "abc"}}};
    const auto result_value = ruvia::detail::check_http_response_trailer_section(fields_value);
    RUVIA_CHECK(result_value.section() != nullptr);
    ruvia::detail::append_http1_trailer_section(trailers, *result_value.section());
    RUVIA_CHECK_EQ(
        std::string_view(trailers), std::string_view("Digest: sha-256=value\r\nX-Trace: abc\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::http1_last_chunk_prefix, std::string_view("0\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::http1_trailer_section_terminator, std::string_view("\r\n"));
}

RUVIA_TEST(http1_chunk_trailer_serializer_requires_validated_section) {
    std::pmr::string trailers(std::pmr::get_default_resource());
    const std::array<ruvia::http_header_view, 1> forbidden{{{"Content-Length", "5"}}};
    const auto forbidden_result = ruvia::detail::check_http_response_trailer_section(forbidden);
    RUVIA_CHECK(forbidden_result.section() == nullptr);
    RUVIA_CHECK(forbidden_result.failure() != nullptr);
    RUVIA_CHECK(trailers.empty());

    const std::array<ruvia::http_header_view, 1> invalid{
        {{"X-Trace", std::string_view("a\r\nb", 4)}}};
    const auto invalid_result = ruvia::detail::check_http_response_trailer_section(invalid);
    RUVIA_CHECK(invalid_result.section() == nullptr);
    RUVIA_CHECK(invalid_result.failure() != nullptr);
    RUVIA_CHECK(trailers.empty());
}
