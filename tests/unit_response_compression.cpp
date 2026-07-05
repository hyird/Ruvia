#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/http/HttpResponse.h"
#include "net/server/HttpResponseCompression.h"
#include "http/HeaderAcceptUtils.h"
#include "http/HttpResponseBodyAccess.h"

namespace {

using ruvia::HttpResponse;
using ruvia::HttpServerOptions;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::compressResponseBodyIfAccepted;
using ruvia::detail::responseBodyBytes;

using Compression = HttpServerOptions::Compression;

// Reference decompressors. Each returns "\x01decompress-failed" on error, a
// sentinel no real body equals, so a failure is a visible mismatch not a match.
const std::string kDecompressFailed = "\x01" "decompress-failed";

std::string gzipDecompress(std::string_view data) {
    z_stream stream{};
    // 15 + 32 auto-detects the gzip (or zlib) wrapper on the stream.
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return kDecompressFailed;
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buffer[16384];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            (void)inflateEnd(&stream);
            return kDecompressFailed;
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status != Z_STREAM_END);
    (void)inflateEnd(&stream);
    return out;
}

std::string brotliDecompress(std::string_view data) {
    std::string out(64 * 1024, '\0');
    std::size_t outSize = out.size();
    const auto result = BrotliDecoderDecompress(
        data.size(), reinterpret_cast<const std::uint8_t*>(data.data()),
        &outSize, reinterpret_cast<std::uint8_t*>(out.data()));
    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        return kDecompressFailed;
    }
    out.resize(outSize);
    return out;
}

std::string zstdDecompress(std::string_view data) {
    std::string out(64 * 1024, '\0');
    const auto size = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(size)) {
        return kDecompressFailed;
    }
    out.resize(size);
    return out;
}

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

RUVIA_TEST(compress_output_round_trips_for_each_coding) {
    // The Content-Encoding label tests do not prove the emitted bytes are a valid
    // stream. Decompress the produced body with the reference library and confirm it
    // equals the original -- catching a corrupt stream (wrong gzip window bits,
    // truncation, bad framing) that a header-only assertion would silently miss.
    // The compressor writes into `scratch` and points the response body view at it
    // (zero-copy), so scratch must outlive the body read -- the shared tryCompress
    // helper's local scratch would dangle, hence the explicit local here.
    const std::string original =
        "Ruvia response compression round-trip payload. "
        "The quick brown fox jumps over the lazy dog. 0123456789. "
        "Repeated content compresses well; repeated content compresses well.";

    {
        auto response = responseWithBody(original);
        std::pmr::string scratch(std::pmr::new_delete_resource());
        RUVIA_CHECK(compressResponseBodyIfAccepted(
            HttpContentCoding::kGzip, response, Compression{true, 16}, scratch));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(responseBodyBytes(response).size() < original.size());  // actually shrank
        RUVIA_CHECK_EQ(gzipDecompress(responseBodyBytes(response)), original);
    }
    {
        auto response = responseWithBody(original);
        std::pmr::string scratch(std::pmr::new_delete_resource());
        RUVIA_CHECK(compressResponseBodyIfAccepted(
            HttpContentCoding::kBrotli, response, Compression{true, 16}, scratch));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK_EQ(brotliDecompress(responseBodyBytes(response)), original);
    }
    {
        auto response = responseWithBody(original);
        std::pmr::string scratch(std::pmr::new_delete_resource());
        RUVIA_CHECK(compressResponseBodyIfAccepted(
            HttpContentCoding::kZstd, response, Compression{true, 16}, scratch));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
        RUVIA_CHECK_EQ(zstdDecompress(responseBodyBytes(response)), original);
    }
}

RUVIA_TEST(compress_happy_path_sets_encoding_and_vary) {
    auto response = responseWithBody(kCompressibleBody);
    RUVIA_CHECK(tryCompress(response, Compression{true, 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    // Compressing on Accept-Encoding must advertise the variance.
    RUVIA_CHECK(response.header("Vary").find("Accept-Encoding") != std::string_view::npos);
}

RUVIA_TEST(compress_weakens_strong_etag_but_leaves_weak_and_absent) {
    // A strong ETag identifies the identity representation byte-for-byte. After
    // compression the body is a different representation (RFC 9110 8.8.1), so the
    // strong validator must be weakened to "W/..." -- otherwise a client could
    // strong-compare it (e.g. If-Range) against the compressed bytes.
    {
        auto response = responseWithBody(kCompressibleBody);
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(tryCompress(response, Compression{true, 16}));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // An already-weak ETag is a semantic (not byte-exact) validator, so it stays
    // valid across encodings and must not be double-weakened to W/W/"...".
    {
        auto response = responseWithBody(kCompressibleBody);
        response.header("ETag", "W/\"v1\"");
        RUVIA_CHECK(tryCompress(response, Compression{true, 16}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // No ETag stays no ETag -- weakening never fabricates a validator.
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{true, 16}));
        RUVIA_CHECK(response.header("ETag").empty());
    }
    // When nothing is compressed (body below minBytes), the strong ETag is left
    // intact -- the response still is the identity representation.
    {
        auto response = responseWithBody("tiny");
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(!tryCompress(response, Compression{true, 4096}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
    }
}

RUVIA_TEST(compress_brotli_and_zstd_emit_their_content_encoding) {
    // The gzip path is covered above; brotli and zstd are equally supported
    // codings and must set their own Content-Encoding token after compressing.
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{true, 16}, HttpContentCoding::kBrotli));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(response.header("Vary").find("Accept-Encoding") != std::string_view::npos);
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{true, 16}, HttpContentCoding::kZstd));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
    }
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

RUVIA_TEST(compress_declares_vary_for_negotiated_but_uncompressed_responses) {
    const auto varies = [](HttpResponse& r) {
        return r.header("Vary").find("Accept-Encoding") != std::string_view::npos;
    };

    // A compressible representation is selected by Accept-Encoding, so it must carry
    // Vary even when THIS response is left identity: below the size threshold, or the
    // client accepted no coding we support. Otherwise a shared cache serves this
    // identity body to a client that would get the compressed one (RFC 9110 12.5.5).
    {
        auto r = responseWithBody("small");
        RUVIA_CHECK(!tryCompress(r, Compression{true, 4096}));  // below minBytes
        RUVIA_CHECK(varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(r, Compression{true, 16}, HttpContentCoding::kNone));
        RUVIA_CHECK(varies(r));
    }

    // Responses that never vary by Accept-Encoding must NOT over-declare Vary
    // (RFC 9110 12.5.5 SHOULD NOT): incompressible media type, no-transform,
    // an already-chosen encoding, and (compression disabled) no negotiation at all.
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Content-Type", "image/png");
        RUVIA_CHECK(!tryCompress(r, Compression{true, 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Cache-Control", "no-transform");
        RUVIA_CHECK(!tryCompress(r, Compression{true, 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Content-Encoding", "gzip");
        RUVIA_CHECK(!tryCompress(r, Compression{true, 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(r, Compression{false, 16}));  // disabled
        RUVIA_CHECK(!varies(r));
    }
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

RUVIA_TEST(compress_skips_incompressible_media_types) {
    auto png = responseWithBody(kCompressibleBody);
    png.header("Content-Type", "image/png");
    RUVIA_CHECK(!tryCompress(png, Compression{true, 16}));
    RUVIA_CHECK(png.header("Content-Encoding").empty());

    auto svg = responseWithBody(kCompressibleBody);
    svg.header("Content-Type", "image/svg+xml");
    RUVIA_CHECK(tryCompress(svg, Compression{true, 16}));
    RUVIA_CHECK_EQ(svg.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(compress_skips_video_audio_and_container_media_types) {
    // Beyond image/*, the full already-compressed set is video/*, audio/*, and the
    // specific container application types. Compressing these wastes CPU for no size
    // win, so each family and each exact container type must be left uncompressed.
    for (const char* type : {"video/mp4", "audio/mpeg", "application/gzip",
                             "application/x-gzip", "application/zip", "application/zstd",
                             "application/pdf", "application/octet-stream"}) {
        auto response = responseWithBody(kCompressibleBody);
        response.header("Content-Type", type);
        RUVIA_CHECK(!tryCompress(response, Compression{true, 16}));
        RUVIA_CHECK(response.header("Content-Encoding").empty());
    }

    // A parameterised incompressible type still matches once its parameters are
    // stripped, so it is not compressed either.
    auto png = responseWithBody(kCompressibleBody);
    png.header("Content-Type", "image/png; name=photo");
    RUVIA_CHECK(!tryCompress(png, Compression{true, 16}));
    RUVIA_CHECK(png.header("Content-Encoding").empty());
}
