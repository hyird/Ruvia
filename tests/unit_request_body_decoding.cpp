#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <brotli/encode.h>
#include <zlib.h>

#include "http/RequestBodyDecoding.h"

namespace {

using ruvia::detail::HttpContentCoding;
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
