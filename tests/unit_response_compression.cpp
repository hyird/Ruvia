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
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/server/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/HttpResponseCompression.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace {

using ruvia::HttpResponse;
using ruvia::HttpKnownMethod;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::applyResponseCompression;
using ruvia::detail::responseBody;

using Compression = ruvia::CompressionConfig;

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
    response.body(body);
    return response;
}

bool tryCompress(
    HttpResponse& response,
    Compression options,
    HttpContentCoding coding = HttpContentCoding::kGzip,
    HttpKnownMethod method = HttpKnownMethod::kGet) {
    const bool alreadyEncoded =
        response.header("Content-Encoding").has_value();
    applyResponseCompression(coding, method, response, options);
    return !alreadyEncoded &&
        response.header("Content-Encoding").has_value();
}

}  // namespace

RUVIA_TEST(compress_output_round_trips_for_each_coding) {
    // The Content-Encoding label tests do not prove the emitted bytes are a valid
    // stream. Decompress the produced body with the reference library and confirm it
    // equals the original -- catching a corrupt stream (wrong gzip window bits,
    // truncation, bad framing) that a header-only assertion would silently miss.
    // Compression installs owned response bytes, so the representation remains
    // valid without an external scratch lifetime protocol.
    const std::string original =
        "Ruvia response compression round-trip payload. "
        "The quick brown fox jumps over the lazy dog. 0123456789. "
        "Repeated content compresses well; repeated content compresses well.";

    {
        auto response = responseWithBody(original);
        applyResponseCompression(
            HttpContentCoding::kGzip,
            HttpKnownMethod::kGet,
            response,
            Compression{.minBytes = 16});
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
        RUVIA_CHECK(responseBody(response).size() < original.size());  // actually shrank
        RUVIA_CHECK_EQ(gzipDecompress(responseBody(response).bytes()), original);
    }
    {
        auto response = responseWithBody(original);
        applyResponseCompression(
            HttpContentCoding::kBrotli,
            HttpKnownMethod::kGet,
            response,
            Compression{.minBytes = 16});
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
        RUVIA_CHECK_EQ(brotliDecompress(responseBody(response).bytes()), original);
    }
    {
        auto response = responseWithBody(original);
        applyResponseCompression(
            HttpContentCoding::kZstd,
            HttpKnownMethod::kGet,
            response,
            Compression{.minBytes = 16});
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
        RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
        RUVIA_CHECK_EQ(zstdDecompress(responseBody(response).bytes()), original);
    }
}

RUVIA_TEST(compress_happy_path_sets_encoding_and_vary) {
    auto response = responseWithBody(kCompressibleBody);
    RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    // Compressing on Accept-Encoding must advertise the variance.
    RUVIA_CHECK(
        response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
        std::string_view::npos);
}

RUVIA_TEST(compress_weakens_strong_etag_but_leaves_weak_and_absent) {
    // A strong ETag identifies the identity representation byte-for-byte. After
    // compression the body is a different representation (RFC 9110 8.8.1), so the
    // strong validator must be weakened to "W/..." -- otherwise a client could
    // strong-compare it (e.g. If-Range) against the compressed bytes.
    {
        auto response = responseWithBody(kCompressibleBody);
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // An already-weak ETag is a semantic (not byte-exact) validator, so it stays
    // valid across encodings and must not be double-weakened to W/W/"...".
    {
        auto response = responseWithBody(kCompressibleBody);
        response.header("ETag", "W/\"v1\"");
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // No ETag stays no ETag -- weakening never fabricates a validator.
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
        RUVIA_CHECK(!response.header("ETag").has_value());
    }
    // When nothing is compressed (body below minBytes), the strong ETag is left
    // intact -- the response still is the identity representation.
    {
        auto response = responseWithBody("tiny");
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 4096}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
    }
}

RUVIA_TEST(compress_brotli_and_zstd_emit_their_content_encoding) {
    // The gzip path is covered above; brotli and zstd are equally supported
    // codings and must set their own Content-Encoding token after compressing.
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}, HttpContentCoding::kBrotli));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(
            response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
            std::string_view::npos);
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}, HttpContentCoding::kZstd));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
    }
}

RUVIA_TEST(buffered_response_absent_policies_skip_cors_and_compression) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Accept-Encoding: gzip\r\n\r\n");
    auto response = responseWithBody(kCompressibleBody);
    ruvia::detail::HttpServerOptions options;
    options.compression.reset();
    RUVIA_CHECK(!options.cors.has_value());

    const auto writePlan = ruvia::detail::prepareBufferedHttpResponse(
        parsed.request,
        HttpContentCoding::kGzip,
        response,
        options);
    RUVIA_CHECK(writePlan.matchesResponse(response));
    RUVIA_CHECK(
        writePlan.requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK(!response.header("Access-Control-Allow-Origin").has_value());
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    RUVIA_CHECK(!response.header("Vary").has_value());
}

RUVIA_TEST(buffered_response_coding_folds_repeated_accept_encoding_fields) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);
    RUVIA_CHECK(
        ruvia::detail::httpResponseCodingFor(parsed.request) ==
        HttpContentCoding::kBrotli);
}

RUVIA_TEST(compress_skips_when_no_coding_but_preserves_head_metadata) {
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}, HttpContentCoding::kIdentity));
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(
            response,
            Compression{.minBytes = 16},
            HttpContentCoding::kGzip,
            HttpKnownMethod::kHead));
        const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
            HttpKnownMethod::kHead, response);
        RUVIA_CHECK(writePlan.bodySuppressed());
        RUVIA_CHECK(!writePlan.sendBody());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(
            response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
            std::string_view::npos);
    }
}

RUVIA_TEST(compress_skips_non_compressible_status_codes) {
    // 206/204/205/304 and any 1xx must never carry a compressed representation.
    for (const std::uint16_t status : {std::uint16_t{206}, std::uint16_t{204},
                                       std::uint16_t{205}, std::uint16_t{304}}) {
        auto response = responseWithBody(kCompressibleBody);
        response.status(status);
        RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
    }
}

RUVIA_TEST(compress_respects_below_min_bytes) {
    auto response = responseWithBody("too small to bother");
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 1024}));
}

RUVIA_TEST(compress_respects_no_transform) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Cache-Control", "no-transform");
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
}

RUVIA_TEST(compress_ignores_no_transform_inside_quoted_extension) {
    auto response = responseWithBody(kCompressibleBody);
    response.header(
        "Cache-Control",
        R"(extension="a, no-transform, b")");
    RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(compress_skips_already_encoded_body) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Encoding", "gzip");  // already encoded upstream
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
}

RUVIA_TEST(compress_declares_vary_for_negotiated_but_uncompressed_responses) {
    const auto varies = [](HttpResponse& r) {
        return r.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
            std::string_view::npos;
    };

    // A compressible representation is selected by Accept-Encoding, so it must carry
    // Vary even when THIS response is left identity: below the size threshold, or the
    // client accepted no coding we support. Otherwise a shared cache serves this
    // identity body to a client that would get the compressed one (RFC 9110 12.5.5).
    {
        auto r = responseWithBody("small");
        RUVIA_CHECK(!tryCompress(r, Compression{.minBytes = 4096}));  // below minBytes
        RUVIA_CHECK(varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(r, Compression{.minBytes = 16}, HttpContentCoding::kIdentity));
        RUVIA_CHECK(varies(r));
    }

    // Responses that never vary by Accept-Encoding must NOT over-declare Vary
    // (RFC 9110 12.5.5 SHOULD NOT): incompressible media type, no-transform,
    // and an already-chosen encoding.
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Content-Type", "image/png");
        RUVIA_CHECK(!tryCompress(r, Compression{.minBytes = 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Cache-Control", "no-transform");
        RUVIA_CHECK(!tryCompress(r, Compression{.minBytes = 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = responseWithBody(kCompressibleBody);
        r.header("Content-Encoding", "gzip");
        RUVIA_CHECK(!tryCompress(r, Compression{.minBytes = 16}));
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
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(compress_skips_content_range_response) {
    // A range/partial representation must not be recompressed: it would invalidate
    // the byte offsets the Content-Range header describes.
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Range", "bytes 0-2047/8192");
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(compress_skips_incompressible_media_types) {
    auto png = responseWithBody(kCompressibleBody);
    png.header("Content-Type", "image/png");
    RUVIA_CHECK(!tryCompress(png, Compression{.minBytes = 16}));
    RUVIA_CHECK(!png.header("Content-Encoding").has_value());

    auto svg = responseWithBody(kCompressibleBody);
    svg.header("Content-Type", "image/svg+xml");
    RUVIA_CHECK(tryCompress(svg, Compression{.minBytes = 16}));
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
        RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    }

    // A parameterised incompressible type still matches once its parameters are
    // stripped, so it is not compressed either.
    auto png = responseWithBody(kCompressibleBody);
    png.header("Content-Type", "image/png; name=photo");
    RUVIA_CHECK(!tryCompress(png, Compression{.minBytes = 16}));
    RUVIA_CHECK(!png.header("Content-Encoding").has_value());
}
