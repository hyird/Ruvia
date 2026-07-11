#include "test_harness.h"

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"

namespace {

using ruvia::detail::HttpContentCoding;
using ruvia::detail::Http1ChunkedBodyDecoder;
using ruvia::detail::Http1RequestBodyPlan;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::HttpServerExpectationAction;
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

std::string chunked(std::string_view body) {
    char size[2 * sizeof(std::size_t)];
    const auto [end, ec] = std::to_chars(
        size,
        size + sizeof(size),
        body.size(),
        16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(size, end);
    wire.append("\r\n");
    wire.append(body);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

template <typename T>
concept HasRequestBodyMode = requires(const T& value) {
    value.mode();
};

template <typename T>
concept HasRequestContentLength = requires(const T& value) {
    { value.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasRequestTransferCodings = requires(const T& value) {
    value.transferCodings();
};

template <typename T>
concept HasPublicRequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(HttpTransferCodings{});
};

static_assert(!std::default_initializable<Http1RequestBodyPlan>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1KnownLengthRequestBody,
    std::size_t>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1ChunkedRequestBody,
    HttpTransferCodings>);
static_assert(!HasPublicRequestBodyPlanFactories<Http1RequestBodyPlan>);
static_assert(!HasRequestBodyMode<Http1RequestBodyPlan>);
static_assert(!HasRequestContentLength<Http1RequestBodyPlan>);
static_assert(!HasRequestTransferCodings<Http1RequestBodyPlan>);
static_assert(HasRequestContentLength<ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasRequestContentLength<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasRequestTransferCodings<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!HasRequestTransferCodings<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().withoutBody()),
    const ruvia::detail::Http1RequestWithoutBody*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().knownLength()),
    const ruvia::detail::Http1KnownLengthRequestBody*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().chunked()),
    const ruvia::detail::Http1ChunkedRequestBody*>);

}  // namespace

RUVIA_TEST(http1_request_body_plan_has_one_framing_truth) {
    Http1ServerRequestParser parser;
    const auto noneState = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto& none = noneState.bodyPlan;
    RUVIA_CHECK(none.withoutBody() != nullptr);
    RUVIA_CHECK(none.knownLength() == nullptr);
    RUVIA_CHECK(none.chunked() == nullptr);
    RUVIA_CHECK(!none.requiresConsumption());

    const auto emptyLengthState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    const auto& emptyLength = emptyLengthState.bodyPlan;
    const auto* knownLength = emptyLength.knownLength();
    RUVIA_CHECK(knownLength != nullptr);
    RUVIA_CHECK(emptyLength.withoutBody() == nullptr);
    RUVIA_CHECK(emptyLength.chunked() == nullptr);
    if (knownLength != nullptr) {
        RUVIA_CHECK_EQ(knownLength->contentLength(), std::size_t{0});
    }
    RUVIA_CHECK(!emptyLength.requiresConsumption());
    RUVIA_CHECK(emptyLength.expectations().has100Continue());
    RUVIA_CHECK(
        emptyLength.expectationAction() ==
        HttpServerExpectationAction::kNone);

    const auto compressedChunkedState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    const auto& compressedChunked = compressedChunkedState.bodyPlan;
    const auto* chunkedBody = compressedChunked.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    RUVIA_CHECK(compressedChunked.withoutBody() == nullptr);
    RUVIA_CHECK(compressedChunked.knownLength() == nullptr);
    RUVIA_CHECK(compressedChunked.requiresConsumption());
    RUVIA_CHECK(
        compressedChunked.expectationAction() ==
        HttpServerExpectationAction::kSend100Continue);
    if (chunkedBody != nullptr) {
        RUVIA_CHECK_EQ(
            chunkedBody->transferCodings().count,
            std::size_t{1});
    }
}

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

RUVIA_TEST(transfer_coded_chunked_request_plan_drives_decode_order) {
    const std::string plain =
        "RFC 9112 transfer coding followed by final chunked framing";
    const std::string gz = gzipCompress(plain);
    const std::string wireBody = chunked(gz);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK(!wireBody.empty());

    Http1ServerRequestParser parser;
    const std::string rawRequest =
        std::string(
            "POST / HTTP/1.1\r\nHost: x\r\n"
            "Transfer-Encoding: gzip, chunked\r\n\r\n") +
        wireBody;
    const auto parsed = parser.parseMessage(rawRequest);
    RUVIA_CHECK(parsed.messageReady());
    const auto* chunkedBody = parsed.bodyPlan.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    if (chunkedBody == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(chunkedBody->transferCodings().count, std::size_t{1});

    auto* resource = std::pmr::get_default_resource();
    Http1ChunkedBodyDecoder chunks(1u << 20);
    TransferCodingDecoder transfer(
        chunkedBody->transferCodings(),
        std::pmr::polymorphic_allocator<char>(resource),
        1u << 20);
    std::pmr::string output(resource);
    std::string_view pending(wireBody);
    bool complete = false;
    while (!complete) {
        const auto result = chunks.decode(pending);
        RUVIA_CHECK(result.needMore() == nullptr);
        if (result.needMore() != nullptr) {
            break;
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            transfer.decodeAppend(bodyChunk->bytes(), output);
        } else if (result.complete() != nullptr) {
            complete = true;
        }
        pending.remove_prefix(result.consumedBytes());
    }
    RUVIA_CHECK(complete);
    transfer.finish();
    RUVIA_CHECK_EQ(
        std::string_view(output.data(), output.size()),
        std::string_view(plain));
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
