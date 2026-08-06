#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/response/HttpResponseCompression.h"
#include "ruvia/web/detail/server/response/HttpStreamingResponseCompression.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::detail::applyResponseCompression;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpResponseCodingSelection;
using ruvia::detail::HttpResponseCodingQualities;
using ruvia::detail::responseBody;

using Compression = ruvia::CompressionConfig;

class ToggleMemoryResource final : public std::pmr::memory_resource {
public:
    explicit ToggleMemoryResource(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource()) noexcept
        : upstream_(upstream) {}

    void failAllocations(bool fail) noexcept {
        fail_ = fail;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail_) {
            throw std::bad_alloc();
        }
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    bool fail_{false};
};

[[nodiscard]] HttpResponseCodingSelection responseCoding(HttpContentCoding coding) {
    HttpResponseCodingQualities qualities;
    switch (coding) {
        case HttpContentCoding::kIdentity:
            break;
        case HttpContentCoding::kGzip:
            qualities.update("gzip");
            break;
        case HttpContentCoding::kBrotli:
            qualities.update("br");
            break;
        case HttpContentCoding::kZstd:
            qualities.update("zstd");
            break;
    }
    const auto selection = HttpResponseCodingSelection::select(qualities);
    const auto* selected = selection.selected();
    if (selected == nullptr || selected->coding() != coding) {
        throw std::logic_error("test response coding selection did not match requested coding");
    }
    return *selected;
}

[[nodiscard]] HttpResponseCodingSelection gzipResponseCoding() {
    return responseCoding(HttpContentCoding::kGzip);
}

// Reference decompressors. Each returns "\x01decompress-failed" on error, a
// sentinel no real body equals, so a failure is a visible mismatch not a match.
const std::string kDecompressFailed =
    "\x01"
    "decompress-failed";

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
    const auto result = BrotliDecoderDecompress(data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &outSize, reinterpret_cast<std::uint8_t*>(out.data()));
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

bool tryCompress(HttpResponse& response, Compression options, HttpContentCoding coding = HttpContentCoding::kGzip, HttpKnownMethod method = HttpKnownMethod::kGet) {
    const bool alreadyEncoded = response.header("Content-Encoding").has_value();
    const auto result = applyResponseCompression(responseCoding(coding), method, response, options);
    return !alreadyEncoded && result.compressed() && response.header("Content-Encoding").has_value();
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
        const auto result = applyResponseCompression(responseCoding(HttpContentCoding::kGzip), HttpKnownMethod::kGet, response, Compression{.minBytes = 16});
        RUVIA_CHECK(result.compressed());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
        RUVIA_CHECK(responseBody(response).size() < original.size());  // actually shrank
        RUVIA_CHECK_EQ(gzipDecompress(responseBody(response).bytes()), original);
    }
    {
        auto response = responseWithBody(original);
        const auto result = applyResponseCompression(responseCoding(HttpContentCoding::kBrotli), HttpKnownMethod::kGet, response, Compression{.minBytes = 16});
        RUVIA_CHECK(result.compressed());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
        RUVIA_CHECK_EQ(brotliDecompress(responseBody(response).bytes()), original);
    }
    {
        auto response = responseWithBody(original);
        const auto result = applyResponseCompression(responseCoding(HttpContentCoding::kZstd), HttpKnownMethod::kGet, response, Compression{.minBytes = 16});
        RUVIA_CHECK(result.compressed());
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
    RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") != std::string_view::npos);
}

RUVIA_TEST(streaming_compression_selects_unknown_length_representation) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Length", "2048");
    response.header("ETag", "\"stream-v1\"");

    const auto selection = gzipResponseCoding();
    RUVIA_CHECK(ruvia::detail::prepareStreamingResponseCompression(selection, HttpKnownMethod::kGet, response, ruvia::detail::ResponseStreamKind::kGeneric));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"stream-v1\""));
    RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") != std::string_view::npos);
}

RUVIA_TEST(streaming_compression_owns_one_typed_encoder_lifecycle) {
    auto response = responseWithBody(kCompressibleBody);
    const auto selection = gzipResponseCoding();
    ruvia::detail::HttpStreamingResponseCompression compression(std::pmr::get_default_resource(), selection, ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);

    compression.prepare(HttpKnownMethod::kGet, response, ruvia::detail::ResponseStreamKind::kGeneric);
    RUVIA_CHECK(!compression.active());
    compression.activate(ruvia::detail::httpResponseBodyPlan(HttpKnownMethod::kGet, response.status()));
    RUVIA_CHECK(compression.active());

    std::string encoded;
    RUVIA_CHECK(compression.write(std::string_view(kCompressibleBody).substr(0, 700)) != ruvia::detail::HttpContentEncodeStep::kFailure);
    encoded.append(compression.output());
    RUVIA_CHECK(compression.write(std::string_view(kCompressibleBody).substr(700)) != ruvia::detail::HttpContentEncodeStep::kFailure);
    encoded.append(compression.output());
    RUVIA_CHECK(compression.finish() == ruvia::detail::HttpContentEncodeStep::kFinished);
    encoded.append(compression.output());
    RUVIA_CHECK(compression.write("late") == ruvia::detail::HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(compression.finish() == ruvia::detail::HttpContentEncodeStep::kFinished);
    RUVIA_CHECK(!compression.active());

    const auto decoded = ruvia::detail::decodeHttpContent(ruvia::detail::HttpContentCoding::kGzip, encoded, kCompressibleBody.size(), std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view(kCompressibleBody));
    }
}

RUVIA_TEST(streaming_compression_failure_is_terminal) {
    ToggleMemoryResource resource;
    auto response = responseWithBody(kCompressibleBody);
    ruvia::detail::HttpStreamingResponseCompression compression(
        &resource,
        gzipResponseCoding(),
        ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);
    compression.prepare(HttpKnownMethod::kGet, response, ruvia::detail::ResponseStreamKind::kGeneric);
    compression.activate(ruvia::detail::httpResponseBodyPlan(HttpKnownMethod::kGet, response.status()));
    RUVIA_CHECK(compression.active());

    resource.failAllocations(true);
    const std::string chunk(4096, 'x');
    RUVIA_CHECK(compression.write(chunk) == ruvia::detail::HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(compression.write("retry") == ruvia::detail::HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(compression.finish() == ruvia::detail::HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(!compression.active());
}

RUVIA_TEST(streaming_compression_precommit_abort_is_terminal) {
    auto response = responseWithBody(kCompressibleBody);
    ruvia::detail::HttpStreamingResponseCompression compression(
        std::pmr::get_default_resource(),
        gzipResponseCoding(),
        ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);
    compression.prepare(HttpKnownMethod::kGet, response, ruvia::detail::ResponseStreamKind::kGeneric);
    compression.abort();

    RUVIA_CHECK(!compression.active());
    RUVIA_CHECK(compression.write("retry") == ruvia::detail::HttpContentEncodeStep::kFailure);
    RUVIA_CHECK(compression.finish() == ruvia::detail::HttpContentEncodeStep::kFailure);
}

RUVIA_TEST(streaming_compression_respects_encoder_availability_at_representation_boundary) {
    const auto selection = [] {
        HttpResponseCodingQualities qualities;
        qualities.update("gzip, identity;q=0");
        const auto selected = HttpResponseCodingSelection::select(qualities);
        if (selected.selected() == nullptr) {
            throw std::logic_error("test response coding selection was empty");
        }
        return *selected.selected();
    }();

    auto response = responseWithBody(kCompressibleBody);
    ruvia::detail::HttpStreamingResponseCompression compression(
        std::pmr::get_default_resource(),
        selection,
        ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);

    bool rejected = false;
    try {
        compression.prepare(HttpKnownMethod::kGet, response, ruvia::detail::ResponseStreamKind::kGeneric);
    } catch (const ruvia::HttpError& error) {
        rejected = error.info().status() == ruvia::http_status::kNotAcceptable;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());

    HttpResponse identityAllowed = responseWithBody(kCompressibleBody);
    HttpResponseCodingQualities allowedQualities;
    allowedQualities.update("gzip");
    const auto allowedSelection = HttpResponseCodingSelection::select(allowedQualities);
    RUVIA_CHECK(allowedSelection.selected() != nullptr);
    if (const auto* selected = allowedSelection.selected()) {
        ruvia::detail::HttpStreamingResponseCompression identityFallback(
            std::pmr::get_default_resource(),
            *selected,
            ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly);
        identityFallback.prepare(HttpKnownMethod::kGet, identityAllowed, ruvia::detail::ResponseStreamKind::kGeneric);
        identityFallback.activate(ruvia::detail::httpResponseBodyPlan(HttpKnownMethod::kGet, identityAllowed.status()));
        RUVIA_CHECK(!identityFallback.active());
        RUVIA_CHECK(!identityAllowed.header("Content-Encoding").has_value());
    }
}

RUVIA_TEST(response_compression_preflight_rejects_non_transformable_metadata) {
    const auto selection = gzipResponseCoding();
    const auto eligible = [](const HttpResponse& response) {
        return ruvia::detail::httpResponseCompressionEligibility(
                   gzipResponseCoding(),
                   HttpKnownMethod::kGet,
                   response,
                   ruvia::detail::ResponseStreamKind::kGeneric) == ruvia::detail::HttpResponseCompressionEligibility::kEligible;
    };

    RUVIA_CHECK(eligible(responseWithBody(kCompressibleBody)));

    auto noTransform = responseWithBody(kCompressibleBody);
    noTransform.header("Cache-Control", "no-transform");
    RUVIA_CHECK(!eligible(noTransform));

    auto media = responseWithBody(kCompressibleBody);
    media.header("Content-Type", "image/png");
    RUVIA_CHECK(!eligible(media));

    auto partial = responseWithBody(kCompressibleBody);
    partial.status(ruvia::http_status::kPartialContent);
    RUVIA_CHECK(!eligible(partial));

    auto encoded = responseWithBody(kCompressibleBody);
    encoded.header("Content-Encoding", "gzip");
    RUVIA_CHECK(!eligible(encoded));

    RUVIA_CHECK(ruvia::detail::httpResponseCompressionEligibility(
                    selection,
                    HttpKnownMethod::kGet,
                    responseWithBody(kCompressibleBody),
                    ruvia::detail::ResponseStreamKind::kGeneric) == ruvia::detail::HttpResponseCompressionEligibility::kEligible);
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
        RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") != std::string_view::npos);
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

    const auto negotiation = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        const auto preparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, response, options);
        const auto writePlan = preparation.writePlan();
        RUVIA_CHECK(writePlan.matchesResponse(response));
        RUVIA_CHECK(writePlan.requestMethod() == ruvia::HttpKnownMethod::kGet);
        RUVIA_CHECK(!response.header("Access-Control-Allow-Origin").has_value());
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
}

RUVIA_TEST(buffered_response_coding_folds_repeated_accept_encoding_fields) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);
    const auto selected = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == HttpContentCoding::kBrotli);
    }
}

RUVIA_TEST(buffered_response_coding_is_independent_of_server_encoder_availability) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);

    const auto selection = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(selection.selected() != nullptr);
    if (const auto* selected = selection.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kGzip);
        RUVIA_CHECK(!selected->identityAccepted());

        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        ruvia::detail::HttpServerOptions options;
        options.compression.reset();
        auto response = responseWithBody(kCompressibleBody);
        static_cast<void>(ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, response, options));
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
        RUVIA_CHECK(ruvia::detail::httpResponseNeedsNotAcceptable(policy, parsed.request, response));
    }
}

RUVIA_TEST(buffered_response_compression_failure_is_not_negotiation_miss) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);

    ToggleMemoryResource resource;
    auto response = HttpResponse(&resource);
    response.body(kCompressibleBody);
    // Keep the already-owned identity body valid, then make the encoder's
    // process-resource allocation fail. This reaches the typed encoder failure
    // branch without making response construction itself fail.
    resource.failAllocations(true);

    const auto negotiation = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        ruvia::detail::HttpServerOptions options;
        const auto preparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, response, options);
        RUVIA_CHECK(preparation.compressionResult().failed());

        const auto error = ruvia::detail::httpBufferedResponsePreparationError(policy, parsed.request, response, preparation.compressionResult());
        RUVIA_CHECK(error.has_value());
        if (error.has_value()) {
            RUVIA_CHECK_EQ(error->status(), ruvia::http_status::kInternalServerError);
            RUVIA_CHECK_EQ(error->code(), std::string_view("response_compression_failed"));
        }
    }
}

RUVIA_TEST(encoded_response_commit_is_transactional_on_header_allocation_failure) {
    // The encoder result is already owned by the response resource. A failure
    // while staging Content-Length must not publish Content-Encoding first:
    // otherwise the identity body would be emitted as a gzip representation.
    ToggleMemoryResource resource;
    auto response = HttpResponse(&resource);
    response.body("identity");
    response.header("Content-Length", "8");
    std::pmr::string encoded("compressed", &resource);

    resource.failAllocations(true);
    bool rejected = false;
    try {
        ruvia::detail::replaceResponseBodyWithContentEncoding(response, std::move(encoded), "gzip");
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("identity"));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("8"));

    // Strong-validator weakening is staged before the body and the encoding
    // field as well. A failure there leaves all identity metadata untouched.
    resource.failAllocations(false);
    auto withEtag = HttpResponse(&resource);
    withEtag.body("identity");
    withEtag.header("ETag", "\"v1\"");
    std::pmr::string encodedWithEtag("compressed", &resource);
    resource.failAllocations(true);
    rejected = false;
    try {
        ruvia::detail::replaceResponseBodyWithContentEncoding(withEtag, std::move(encodedWithEtag), "gzip");
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(responseBody(withEtag).bytes(), std::string_view("identity"));
    RUVIA_CHECK(!withEtag.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(withEtag.header("ETag"), std::string_view("\"v1\""));
}

RUVIA_TEST(buffered_response_rejects_forbidden_identity_when_policy_skips_compression) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");

    auto noTransform = responseWithBody(kCompressibleBody);
    noTransform.header("Cache-Control", "no-transform");
    auto options = ruvia::detail::HttpServerOptions{};
    const auto coding = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(coding.selected() != nullptr);
    if (const auto* selected = coding.selected()) {
        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        const auto preparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, noTransform, options);
        const auto writePlan = preparation.writePlan();
        RUVIA_CHECK(writePlan.matchesResponse(noTransform));
        RUVIA_CHECK(ruvia::detail::httpResponseNeedsNotAcceptable(policy, parsed.request, noTransform));

        // The replacement 406 representation gets the same negotiated coding
        // opportunity. Identity is only permitted below when even that
        // terminal error cannot be represented acceptably.
        auto error = responseWithBody(kCompressibleBody);
        error.status(ruvia::http_status::kNotAcceptable);
        static_cast<void>(ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, error, options));
        RUVIA_CHECK_EQ(error.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(!ruvia::detail::httpResponseNeedsNotAcceptable(policy, parsed.request, error));
    }

    auto bodyless = responseWithBody(kCompressibleBody);
    bodyless.status(ruvia::http_status::kNoContent);
    if (const auto* selected = coding.selected()) {
        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        static_cast<void>(ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, bodyless, options));
        RUVIA_CHECK(!ruvia::detail::httpResponseNeedsNotAcceptable(policy, parsed.request, bodyless));
    }

    auto terminalError = responseWithBody(kCompressibleBody);
    static_cast<void>(ruvia::detail::prepareBufferedHttpResponse(parsed.request, ruvia::detail::HttpResponseCodingPolicy::disabled(), terminalError, options));
    RUVIA_CHECK(!terminalError.header("Content-Encoding").has_value());
}

RUVIA_TEST(buffered_response_defers_empty_coding_set_until_status_is_known) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET /empty HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);

    const auto policy = ruvia::detail::HttpResponseCodingPolicy::noAcceptableCoding();
    ruvia::detail::HttpServerOptions options;

    auto bodyless = responseWithBody("this body is suppressed by 204");
    bodyless.status(ruvia::http_status::kNoContent);
    const auto bodylessPreparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, bodyless, options);
    RUVIA_CHECK(!ruvia::detail::httpBufferedResponsePreparationError(policy, parsed.request, bodyless, bodylessPreparation.compressionResult()).has_value());

    auto bodyful = responseWithBody("this representation cannot be identity");
    const auto bodyfulPreparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, bodyful, options);
    const auto bodyfulError = ruvia::detail::httpBufferedResponsePreparationError(policy, parsed.request, bodyful, bodyfulPreparation.compressionResult());
    RUVIA_CHECK(bodyfulError.has_value());
    if (bodyfulError.has_value()) {
        RUVIA_CHECK_EQ(bodyfulError->status(), ruvia::http_status::kNotAcceptable);
    }
}

RUVIA_TEST(compress_skips_when_no_coding_but_preserves_head_metadata) {
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}, HttpContentCoding::kIdentity));
    }
    {
        auto response = responseWithBody(kCompressibleBody);
        RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}, HttpContentCoding::kGzip, HttpKnownMethod::kHead));
        const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(HttpKnownMethod::kHead, response);
        RUVIA_CHECK(writePlan.bodySuppressed());
        RUVIA_CHECK(!writePlan.sendBody());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") != std::string_view::npos);
    }
}

RUVIA_TEST(compress_skips_non_compressible_status_codes) {
    // 206/204/205/304 and any 1xx must never carry a compressed representation.
    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kPartialContent, ruvia::http_status::kNoContent, ruvia::http_status::kResetContent, ruvia::http_status::kNotModified}) {
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

RUVIA_TEST(compress_respects_no_transform_in_later_cache_control_field) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Cache-Control", "public");
    response.header("Cache-Control", "no-transform", HttpResponse::HeaderOptions{.append = true});
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
}

RUVIA_TEST(compress_ignores_no_transform_inside_quoted_extension) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Cache-Control", R"(extension="a, no-transform, b")");
    RUVIA_CHECK(tryCompress(response, Compression{.minBytes = 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(compress_skips_already_encoded_body) {
    auto response = responseWithBody(kCompressibleBody);
    response.header("Content-Encoding", "gzip");  // already encoded upstream
    RUVIA_CHECK(!tryCompress(response, Compression{.minBytes = 16}));
}

RUVIA_TEST(preencoded_response_must_be_acceptable_to_client) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET /encoded HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: br, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);

    const auto negotiation = ruvia::detail::httpResponseCodingFor(parsed.request);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        RUVIA_CHECK(selected->coding() == HttpContentCoding::kBrotli);

        auto buffered = responseWithBody(kCompressibleBody);
        buffered.header("Content-Encoding", "gzip");
        const auto policy = ruvia::detail::HttpResponseCodingPolicy::selected(*selected);
        ruvia::detail::HttpServerOptions options;
        const auto preparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, buffered, options);
        const auto error = ruvia::detail::httpBufferedResponsePreparationError(policy, parsed.request, buffered, preparation.compressionResult());
        RUVIA_CHECK(error.has_value());
        if (error.has_value()) {
            RUVIA_CHECK_EQ(error->status(), ruvia::http_status::kNotAcceptable);
        }

        // A valid stack made only from Ruvia-known codings must be checked
        // member by member. Treating every multi-coding field as opaque would
        // let `gzip, br` through even though this request explicitly rejects
        // gzip.
        auto stacked = responseWithBody(kCompressibleBody);
        stacked.header("Content-Encoding", "gzip, br");
        const auto stackedPreparation = ruvia::detail::prepareBufferedHttpResponse(parsed.request, policy, stacked, options);
        const auto stackedError = ruvia::detail::httpBufferedResponsePreparationError(policy, parsed.request, stacked, stackedPreparation.compressionResult());
        RUVIA_CHECK(stackedError.has_value());
        if (stackedError.has_value()) {
            RUVIA_CHECK_EQ(stackedError->status(), ruvia::http_status::kNotAcceptable);
        }

        auto streaming = responseWithBody(kCompressibleBody);
        streaming.header("Content-Encoding", "gzip");
        ruvia::detail::HttpStreamingResponseCompression streamCompression(
            std::pmr::get_default_resource(),
            *selected,
            ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);
        bool rejected = false;
        try {
            streamCompression.prepare(HttpKnownMethod::kGet, streaming, ruvia::detail::ResponseStreamKind::kGeneric);
        } catch (const ruvia::HttpError& streamError) {
            rejected = streamError.info().status() == ruvia::http_status::kNotAcceptable;
        }
        RUVIA_CHECK(rejected);

        auto stackedStreaming = responseWithBody(kCompressibleBody);
        stackedStreaming.header("Content-Encoding", "gzip, br");
        ruvia::detail::HttpStreamingResponseCompression stackedCompression(
            std::pmr::get_default_resource(),
            *selected,
            ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);
        bool stackedRejected = false;
        try {
            stackedCompression.prepare(HttpKnownMethod::kGet, stackedStreaming, ruvia::detail::ResponseStreamKind::kGeneric);
        } catch (const ruvia::HttpError& stackedStreamError) {
            stackedRejected = stackedStreamError.info().status() == ruvia::http_status::kNotAcceptable;
        }
        RUVIA_CHECK(stackedRejected);

        ruvia::detail::Http1ServerRequestParser identityParser;
        const auto identityParsed = identityParser.parseMessage(
            "GET /encoded HTTP/1.1\r\nHost: x\r\n"
            "Accept-Encoding: identity, gzip;q=0\r\n\r\n");
        RUVIA_CHECK(identityParsed.messageReady() != nullptr);
        const auto identityNegotiation = ruvia::detail::httpResponseCodingFor(identityParsed.request);
        RUVIA_CHECK(identityNegotiation.selected() != nullptr);
        if (const auto* identitySelection = identityNegotiation.selected()) {
            RUVIA_CHECK(identitySelection->coding() == HttpContentCoding::kIdentity);
            auto identityStreaming = responseWithBody(kCompressibleBody);
            identityStreaming.header("Content-Encoding", "gzip");
            ruvia::detail::HttpStreamingResponseCompression identityCompression(
                std::pmr::get_default_resource(),
                *identitySelection,
                ruvia::detail::HttpResponseCodingAvailability::kIdentityAndCompression);
            bool identityRejected = false;
            try {
                identityCompression.prepare(HttpKnownMethod::kGet, identityStreaming, ruvia::detail::ResponseStreamKind::kGeneric);
            } catch (const ruvia::HttpError& identityError) {
                identityRejected = identityError.info().status() == ruvia::http_status::kNotAcceptable;
            }
            RUVIA_CHECK(identityRejected);
        }
    }
}

RUVIA_TEST(compress_declares_vary_for_negotiated_but_uncompressed_responses) {
    const auto varies = [](HttpResponse& r) { return r.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") != std::string_view::npos; };

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
    for (const char* type : {"video/mp4", "audio/mpeg", "application/gzip", "application/x-gzip", "application/zip", "application/zstd", "application/pdf", "application/octet-stream"}) {
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
