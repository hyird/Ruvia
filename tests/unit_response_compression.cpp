#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "net/server/HttpResponseCompression.h"
#include "http/HeaderAcceptUtils.h"

namespace {

using ruvia::HttpResponse;
using ruvia::HttpServerOptions;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::compressResponseBodyIfAccepted;

using Compression = HttpServerOptions::Compression;

// A highly compressible payload comfortably above any minBytes used here.
const std::string kCompressibleBody(2048, 'a');

HttpResponse responseWithBody(std::string_view body) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.setBodyCopy(body);
    return response;
}

bool tryCompress(
    HttpResponse& response,
    Compression options,
    HttpContentCoding coding = HttpContentCoding::kGzip,
    bool skipBody = false) {
    std::pmr::string scratch(std::pmr::new_delete_resource());
    return compressResponseBodyIfAccepted(coding, response, options, scratch, skipBody);
}

}  // namespace

RUVIA_TEST(compress_happy_path_sets_encoding_and_vary) {
    auto response = responseWithBody(kCompressibleBody);
    RUVIA_CHECK(tryCompress(response, Compression{true, 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    // Compressing on Accept-Encoding must advertise the variance.
    RUVIA_CHECK(response.header("Vary").find("Accept-Encoding") != std::string_view::npos);
}

RUVIA_TEST(compress_skips_when_disabled_none_or_skipbody) {
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(response, Compression{false, 16}));  // disabled
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(response, Compression{true, 16}, HttpContentCoding::kNone));
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(response, Compression{true, 16}, HttpContentCoding::kGzip, true));  // skipBody
    }
}

RUVIA_TEST(compress_skips_non_compressible_status_codes) {
    // 206/204/205/304 and any 1xx must never carry a compressed representation.
    for (const std::uint16_t status : {std::uint16_t{206}, std::uint16_t{204},
                                       std::uint16_t{205}, std::uint16_t{304}}) {
        auto response = responseWithBody(kCompressibleBody);
        response.status(status);
        RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
    }
}

RUVIA_TEST(compress_respects_below_min_bytes) {
    auto response = responseWithBody("too small to bother");
    RUVIA_CHECK(!tryCompress(response, Compression{true, 1024}));
}

RUVIA_TEST(compress_respects_no_transform) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Cache-Control", "no-transform");
    RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
}

RUVIA_TEST(compress_skips_already_encoded_body) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Encoding", "gzip");  // already encoded upstream
    RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
}

RUVIA_TEST(compress_skips_when_result_would_not_be_smaller) {
    // High-entropy data cannot be shrunk; the response must be left uncompressed
    // rather than emitting a larger body and wasting CPU (as with images, video,
    // or already-compressed payloads). splitmix64 output is effectively random.
    std::string incompressible;
    incompressible.reserve(4096);
    std::uint64_t x = 0;
    for (int i = 0; i < 4096; ++i) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= (z >> 31);
        incompressible.push_back(static_cast<char>(z & 0xFF));
    }
    auto response = responseWithBody(incompressible);
    RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
    RUVIA_CHECK(response.header("Content-Encoding").empty());
}

RUVIA_TEST(compress_skips_content_range_response) {
    // A range/partial representation must not be recompressed: it would invalidate
    // the byte offsets the Content-Range header describes.
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Range", "bytes 0-2047/8192");
    RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
    RUVIA_CHECK(response.header("Content-Encoding").empty());
}
