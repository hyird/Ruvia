#include "test_harness.h"

#include "ruvia/http/detail/http1/Http1ChunkedFraming.h"

#include <cstddef>
#include <array>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

template <typename T>
concept ExposesRvalueHttp1ChunkHeaderView = requires(T&& header) {
    std::move(header).view();
};

static_assert(!ExposesRvalueHttp1ChunkHeaderView<
    ruvia::detail::Http1ChunkHeader>);

RUVIA_TEST(http1_chunk_header_encodes_lowercase_hex_and_crlf) {
    const ruvia::detail::Http1ChunkHeader zero(0);
    const ruvia::detail::Http1ChunkHeader fifteen(15);
    const ruvia::detail::Http1ChunkHeader sixteen(16);
    const ruvia::detail::Http1ChunkHeader abc(0xabc);
    RUVIA_CHECK_EQ(zero.view(), std::string_view("0\r\n"));
    RUVIA_CHECK_EQ(fifteen.view(), std::string_view("f\r\n"));
    RUVIA_CHECK_EQ(sixteen.view(), std::string_view("10\r\n"));
    RUVIA_CHECK_EQ(abc.view(), std::string_view("abc\r\n"));
}

RUVIA_TEST(http1_chunk_header_buffer_covers_size_t_max) {
    const ruvia::detail::Http1ChunkHeader header((std::numeric_limits<std::size_t>::max)());
    const auto encoded = header.view();
    RUVIA_CHECK_EQ(encoded.size(), sizeof(std::size_t) * 2 + 2);
    RUVIA_CHECK(encoded.ends_with("\r\n"));
}

RUVIA_TEST(http1_chunk_trailer_serialization_is_protocol_owned) {
    std::pmr::string trailers(std::pmr::get_default_resource());
    const std::array<ruvia::HttpHeaderView, 2> fields{{
        {"Digest", "sha-256=value"},
        {"X-Trace", "abc"}}};
    const auto result = ruvia::detail::httpResponseTrailerSection(fields);
    RUVIA_CHECK(result.section() != nullptr);
    ruvia::detail::appendHttp1TrailerSection(trailers, *result.section());
    RUVIA_CHECK_EQ(
        std::string_view(trailers),
        std::string_view("Digest: sha-256=value\r\nX-Trace: abc\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::kHttp1LastChunkPrefix, std::string_view("0\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::kHttp1TrailerSectionTerminator, std::string_view("\r\n"));
}

RUVIA_TEST(http1_chunk_trailer_serializer_requires_validated_section) {
    std::pmr::string trailers(std::pmr::get_default_resource());
    const std::array<ruvia::HttpHeaderView, 1> forbidden{{
        {"Content-Length", "5"}}};
    const auto forbiddenResult =
        ruvia::detail::httpResponseTrailerSection(forbidden);
    RUVIA_CHECK(forbiddenResult.section() == nullptr);
    RUVIA_CHECK(forbiddenResult.failure() != nullptr);
    RUVIA_CHECK(trailers.empty());

    const std::array<ruvia::HttpHeaderView, 1> invalid{{
        {"X-Trace", std::string_view("a\r\nb", 4)}}};
    const auto invalidResult =
        ruvia::detail::httpResponseTrailerSection(invalid);
    RUVIA_CHECK(invalidResult.section() == nullptr);
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(trailers.empty());
}
