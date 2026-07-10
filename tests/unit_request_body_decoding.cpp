#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"

namespace {

using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpTransferCoding;
using ruvia::detail::HttpTransferCodings;
using ruvia::detail::TransferCodingDecoder;
using ruvia::detail::decodeRequestContentEncoding;
using ruvia::detail::kMaxDecodedRequestBodyBytes;
using ruvia::detail::requestContentCoding;

std::string gzipCompress(std::string_view data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buffer[16384];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status == Z_OK);
    (void)deflateEnd(&stream);
    return out;
}

std::string brotliCompress(std::string_view data) {
    std::size_t bound = BrotliEncoderMaxCompressedSize(data.size());
    if (bound == 0) {
        bound = data.size() + 1024;
    }
    std::string out(bound, '\0');
    std::size_t outSize = bound;
    if (BrotliEncoderCompress(
            BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            data.size(), reinterpret_cast<const std::uint8_t*>(data.data()),
            &outSize, reinterpret_cast<std::uint8_t*>(out.data())) != BROTLI_TRUE) {
        return {};
    }
    out.resize(outSize);
    return out;
}

std::string zstdCompress(std::string_view data) {
    const std::size_t bound = ZSTD_compressBound(data.size());
    std::string out(bound, '\0');
    const std::size_t size = ZSTD_compress(out.data(), bound, data.data(), data.size(), 3);
    if (ZSTD_isError(size)) {
        return {};
    }
    out.resize(size);
    return out;
}

std::string decoded(HttpContentCoding coding, std::string_view input, std::size_t maxBytes) {
    std::pmr::string out(std::pmr::get_default_resource());
    if (!decodeRequestContentEncoding(coding, input, out, maxBytes)) {
        return std::string("\x01" "decode-failed");  // sentinel no real body equals
    }
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(request_content_coding_mapping) {
    RUVIA_CHECK(requestContentCoding("gzip") == HttpContentCoding::kGzip);
    RUVIA_CHECK(requestContentCoding("x-gzip") == HttpContentCoding::kGzip);  // alias
    RUVIA_CHECK(requestContentCoding("GZIP") == HttpContentCoding::kGzip);    // case-insensitive
    RUVIA_CHECK(requestContentCoding("  br ") == HttpContentCoding::kBrotli); // OWS trimmed
    RUVIA_CHECK(requestContentCoding("zstd") == HttpContentCoding::kZstd);
    RUVIA_CHECK(requestContentCoding("identity") == HttpContentCoding::kNone);
    RUVIA_CHECK(requestContentCoding("deflate") == HttpContentCoding::kNone);  // unsupported
    RUVIA_CHECK(requestContentCoding("") == HttpContentCoding::kNone);
    RUVIA_CHECK(requestContentCoding("gzip, br") == HttpContentCoding::kNone);  // a list is not honored
}

RUVIA_TEST(request_body_gzip_round_trip) {
    const std::string plain = "The quick brown fox jumps over the lazy dog";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kGzip, gz, kMaxDecodedRequestBodyBytes), plain);
}

RUVIA_TEST(request_body_gzip_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny gzip
    const std::string gz = gzipCompress(big);
    std::pmr::string out(std::pmr::get_default_resource());
    // A small cap must stop the expansion, not decode the whole megabyte.
    RUVIA_CHECK(!decodeRequestContentEncoding(HttpContentCoding::kGzip, gz, out, 1024));
}

RUVIA_TEST(request_body_gzip_truncated_rejected) {
    const std::string plain(4096, 'q');
    std::string gz = gzipCompress(plain);
    RUVIA_CHECK(gz.size() > 6);
    gz.resize(gz.size() - 6);  // cut into the gzip trailer -> incomplete stream
    std::pmr::string out(std::pmr::get_default_resource());
    RUVIA_CHECK(!decodeRequestContentEncoding(HttpContentCoding::kGzip, gz, out, kMaxDecodedRequestBodyBytes));
}

RUVIA_TEST(request_body_brotli_round_trip) {
    const std::string plain = "permessage brotli body content, repeated repeated repeated";
    const std::string br = brotliCompress(plain);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kBrotli, br, kMaxDecodedRequestBodyBytes), plain);
}

RUVIA_TEST(request_body_brotli_bomb_rejected) {
    const std::string big(1u << 20, 'a');
    const std::string br = brotliCompress(big);
    RUVIA_CHECK(!br.empty());
    std::pmr::string out(std::pmr::get_default_resource());
    RUVIA_CHECK(!decodeRequestContentEncoding(HttpContentCoding::kBrotli, br, out, 1024));
}

RUVIA_TEST(request_body_zstd_round_trip) {
    const std::string plain = "zstd request body content, repeated repeated repeated repeated";
    const std::string zz = zstdCompress(plain);
    RUVIA_CHECK(!zz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kZstd, zz, kMaxDecodedRequestBodyBytes), plain);
}

RUVIA_TEST(request_body_zstd_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny zstd frame
    const std::string zz = zstdCompress(big);
    RUVIA_CHECK(!zz.empty());
    std::pmr::string out(std::pmr::get_default_resource());
    // A small cap must stop the expansion mid-stream, not decode the whole megabyte.
    RUVIA_CHECK(!decodeRequestContentEncoding(HttpContentCoding::kZstd, zz, out, 1024));
}

RUVIA_TEST(transfer_coding_decoder_gzip_round_trip) {
    auto* resource = std::pmr::get_default_resource();
    HttpTransferCodings codings{};
    codings.values[0] = HttpTransferCoding::kGzip;
    codings.count = 1;
    TransferCodingDecoder decoder(codings, std::pmr::polymorphic_allocator<char>(resource), 1u << 20);

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzipCompress(plain);
    std::pmr::string output(resource);
    decoder.decodeAppend(gz, output);
    decoder.finish();
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    HttpTransferCodings codings{};
    codings.values[0] = HttpTransferCoding::kGzip;
    codings.count = 1;
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    TransferCodingDecoder decoder(codings, std::pmr::polymorphic_allocator<char>(resource), 1024);

    const std::string big(1u << 20, 'a');
    const std::string gz = gzipCompress(big);
    std::pmr::string output(resource);
    bool threw = false;
    try {
        decoder.decodeAppend(gz, output);
        decoder.finish();
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
