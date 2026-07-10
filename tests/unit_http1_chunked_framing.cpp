#include "test_harness.h"

#include "ruvia/http/detail/http1/Http1ChunkedFraming.h"

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

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
    ruvia::detail::appendHttp1TrailerField(trailers, "Digest", "sha-256=value");
    ruvia::detail::appendHttp1TrailerField(trailers, "X-Trace", "abc");
    RUVIA_CHECK_EQ(
        std::string_view(trailers),
        std::string_view("Digest: sha-256=value\r\nX-Trace: abc\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::kHttp1LastChunkPrefix, std::string_view("0\r\n"));
    RUVIA_CHECK_EQ(ruvia::detail::kHttp1TrailerSectionTerminator, std::string_view("\r\n"));
}

RUVIA_TEST(http1_chunk_trailer_serializer_owns_protocol_validation) {
    std::pmr::string trailers(std::pmr::get_default_resource());
    bool forbiddenRejected = false;
    try {
        ruvia::detail::appendHttp1TrailerField(trailers, "Content-Length", "5");
    } catch (const std::invalid_argument&) {
        forbiddenRejected = true;
    }
    RUVIA_CHECK(forbiddenRejected);
    RUVIA_CHECK(trailers.empty());

    bool invalidValueRejected = false;
    try {
        ruvia::detail::appendHttp1TrailerField(
            trailers, "X-Trace", std::string_view("a\r\nb", 4));
    } catch (const std::invalid_argument&) {
        invalidValueRejected = true;
    }
    RUVIA_CHECK(invalidValueRejected);
    RUVIA_CHECK(trailers.empty());
}
