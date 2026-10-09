#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

#include "failing_memory_resource.h"
#include "server/http_buffered_response.h"
#include "server/http_response_compression.h"
#include "server/http_streaming_response_compression.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::http_content_coding;
using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::http_response_coding_qualities;
using ruvia::http_response_coding_selection;
using ruvia::detail::apply_response_compression;
using compression_type = ruvia::compression_config;

[[nodiscard]] http_response_coding_selection response_coding(http_content_coding coding) {
    http_response_coding_qualities qualities;
    switch (coding) {
        case http_content_coding::identity:
            break;
        case http_content_coding::gzip:
            qualities.update("gzip");
            break;
        case http_content_coding::deflate:
            qualities.update("deflate");
            break;
        case http_content_coding::brotli:
            qualities.update("br");
            break;
        case http_content_coding::zstd:
            qualities.update("zstd");
            break;
    }
    const auto selection = http_response_coding_selection::select(qualities);
    const auto* selected = selection.selected();
    if (selected == nullptr || selected->coding() != coding) {
        throw std::logic_error("test response coding selection did not match requested coding");
    }
    return *selected;
}

[[nodiscard]] http_response_coding_selection gzip_response_coding() {
    return response_coding(http_content_coding::gzip);
}

// Reference decompressors. Each returns "\x01decompress-failed" on error, a
// sentinel no real body equals, so a failure is a visible mismatch not a match.
const std::string decompress_failed =
    "\x01"
    "decompress-failed";

std::string gzip_decompress(std::string_view data) {
    z_stream stream{};
    // 15 + 32 auto-detects the gzip (or zlib) wrapper on the stream.
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return decompress_failed;
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
            return decompress_failed;
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status != Z_STREAM_END);
    (void)inflateEnd(&stream);
    return out;
}

std::string brotli_decompress(std::string_view data) {
    std::string out(64 * 1024, '\0');
    std::size_t out_size = out.size();
    const auto result_value =
        BrotliDecoderDecompress(data.size(), reinterpret_cast<const std::uint8_t*>(data.data()),
            &out_size, reinterpret_cast<std::uint8_t*>(out.data()));
    if (result_value != BROTLI_DECODER_RESULT_SUCCESS) {
        return decompress_failed;
    }
    out.resize(out_size);
    return out;
}

std::string zstd_decompress(std::string_view data) {
    std::string out(64 * 1024, '\0');
    const auto size = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(size)) {
        return decompress_failed;
    }
    out.resize(size);
    return out;
}

// A highly compressible payload comfortably above any min_bytes used here.
const std::string compressible_body(2048, 'a');

http_response response_with_body(std::string_view body) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.body(body);
    return response;
}

bool try_compress(http_response& response, compression_type options,
    http_content_coding coding = http_content_coding::gzip,
    http_known_method method = http_known_method::get) {
    const bool already_encoded = response.header("Content-Encoding").has_value();
    const auto result_value = apply_response_compression(response_coding(coding), method, response, options);
    return !already_encoded && result_value.compressed() &&
           response.header("Content-Encoding").has_value();
}

template <typename result_type>
[[nodiscard]] result_type run_compression_task(
    ruvia::event_loop_attachment& attachment, ruvia::task<result_type> task_value) {
    std::optional<result_type> result;
    std::exception_ptr exception;
    auto loop = attachment.loop();
    auto& context_value = loop.io_context();
    asio::co_spawn(
        context_value,
        [task_value = std::move(task_value), &context_value, &result, &exception]() mutable -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::as_awaitable(std::move(task_value)));
            } catch (...) {
                exception = std::current_exception();
            }
            context_value.stop();
        },
        asio::detached);
    attachment.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("compression task produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(buffered_response_compression_uses_sync_and_bounded_offload_thresholds) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::blocking_pool pool(ruvia::blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 2});
    const auto coding = gzip_response_coding();
    const auto options =
        compression_type{.min_bytes_ = 1024, .sync_bytes_ = 64 * 1024, .max_bytes_ = 64 * 1024 * 1024};

    auto small = response_with_body(std::string(32 * 1024, 's'));
    const auto before_small = pool.stats();
    auto small_result =
        run_compression_task(attachment, ruvia::detail::apply_response_compression_async(
                                             coding, http_known_method::get, small, options, &pool, worker_value));
    RUVIA_CHECK(small_result.compressed());
    RUVIA_CHECK_EQ(pool.stats().completed_, before_small.completed_);
    RUVIA_CHECK_EQ(gzip_decompress(small.body_bytes()), std::string(32 * 1024, 's'));

    io.restart();
    const std::string large_plain(128 * 1024, 'l');
    auto large = response_with_body(large_plain);
    const auto before_large = pool.stats();
    auto large_result =
        run_compression_task(attachment, ruvia::detail::apply_response_compression_async(
                                             coding, http_known_method::get, large, options, &pool, worker_value));
    RUVIA_CHECK(large_result.compressed());
    RUVIA_CHECK_EQ(pool.stats().completed_, before_large.completed_ + 1);
    RUVIA_CHECK_EQ(gzip_decompress(large.body_bytes()), large_plain);

    io.restart();
    const std::string synchronous_fallback_plain(128 * 1024, 'f');
    auto synchronous_fallback = response_with_body(synchronous_fallback_plain);
    auto synchronous_fallback_result = run_compression_task(
        attachment, ruvia::detail::apply_response_compression_async(
                        coding, http_known_method::get, synchronous_fallback, options, nullptr, worker_value));
    RUVIA_CHECK(synchronous_fallback_result.compressed());
    RUVIA_CHECK_EQ(
        gzip_decompress(synchronous_fallback.body_bytes()), synchronous_fallback_plain);

    pool.stop();
    pool.join();
}

RUVIA_TEST(
    buffered_response_compression_falls_back_to_identity_when_pool_rejects_or_body_is_too_large) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::blocking_pool pool(ruvia::blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 1});
    pool.stop();
    pool.join();
    const auto coding = gzip_response_coding();
    const auto options =
        compression_type{.min_bytes_ = 1024, .sync_bytes_ = 64 * 1024, .max_bytes_ = 64 * 1024 * 1024};

    const std::string large_plain(128 * 1024, 'q');
    auto unavailable = response_with_body(large_plain);
    auto unavailable_result =
        run_compression_task(attachment, ruvia::detail::apply_response_compression_async(coding,
                                             http_known_method::get, unavailable, options, &pool, worker_value));
    RUVIA_CHECK(unavailable_result.not_applicable());
    RUVIA_CHECK(!unavailable.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(unavailable.body_bytes(), std::string_view(large_plain));

    io.restart();
    const std::string oversized_plain(65 * 1024, 'x');
    auto oversized = response_with_body(oversized_plain);
    const auto capped =
        compression_type{.min_bytes_ = 1024, .sync_bytes_ = 32 * 1024, .max_bytes_ = 64 * 1024};
    auto oversized_result =
        run_compression_task(attachment, ruvia::detail::apply_response_compression_async(coding,
                                             http_known_method::get, oversized, capped, nullptr, worker_value));
    RUVIA_CHECK(oversized_result.not_applicable());
    RUVIA_CHECK(!oversized.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(oversized.body_bytes(), std::string_view(oversized_plain));
}

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
        auto response = response_with_body(original);
        const auto result_value = apply_response_compression(response_coding(http_content_coding::gzip),
            http_known_method::get, response, compression_type{.min_bytes_ = 16});
        RUVIA_CHECK(result_value.compressed());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(!response.body_bytes().empty());
        RUVIA_CHECK(response.body_bytes().size() < original.size());  // actually shrank
        RUVIA_CHECK_EQ(gzip_decompress(response.body_bytes()), original);
    }
    {
        auto response = response_with_body(original);
        const auto result_value = apply_response_compression(response_coding(http_content_coding::brotli),
            http_known_method::get, response, compression_type{.min_bytes_ = 16});
        RUVIA_CHECK(result_value.compressed());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(!response.body_bytes().empty());
        RUVIA_CHECK_EQ(brotli_decompress(response.body_bytes()), original);
    }
    {
        auto response = response_with_body(original);
        const auto result_value = apply_response_compression(response_coding(http_content_coding::zstd),
            http_known_method::get, response, compression_type{.min_bytes_ = 16});
        RUVIA_CHECK(result_value.compressed());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
        RUVIA_CHECK(!response.body_bytes().empty());
        RUVIA_CHECK_EQ(zstd_decompress(response.body_bytes()), original);
    }
}

RUVIA_TEST(compress_happy_path_sets_encoding_and_vary) {
    auto response = response_with_body(compressible_body);
    RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    // Compressing on Accept-Encoding must advertise the variance.
    RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
                std::string_view::npos);
}

RUVIA_TEST(streaming_compression_selects_unknown_length_representation) {
    auto response = response_with_body(compressible_body);
    response.header("Content-Length", "2048");
    response.header("ETag", "\"stream-v1\"");

    const auto selection = gzip_response_coding();
    ruvia::detail::http_streaming_response_compression compression(
        std::pmr::get_default_resource(), selection,
        ruvia::detail::http_response_coding_availability::identity_and_compression);
    compression.prepare(
        http_known_method::get, response, ruvia::http_response_stream_kind::generic);
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"stream-v1\""));
    RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
                std::string_view::npos);
}

RUVIA_TEST(streaming_identity_representation_preserves_negotiated_variance) {
    for (const auto method : {http_known_method::get, http_known_method::head}) {
        for (const auto kind : {ruvia::http_response_stream_kind::generic,
                 ruvia::http_response_stream_kind::sse}) {
            auto response = response_with_body(compressible_body);
            response.header("Vary", "Origin");
            response.header("ETag", "\"identity-v1\"");
            ruvia::detail::http_streaming_response_compression compression(
                std::pmr::get_default_resource(), response_coding(http_content_coding::identity),
                ruvia::detail::http_response_coding_availability::identity_and_compression);
            compression.prepare(method, response, kind);
            RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, Accept-Encoding"));
            RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"identity-v1\""));
            RUVIA_CHECK(!response.header("Content-Encoding").has_value());
            compression.activate(ruvia::plan_http_response_body(method, response.status()));
            RUVIA_CHECK(!compression.active());
        }
    }
}

RUVIA_TEST(response_coding_preparation_shares_variance_without_transforming_fixed_sources) {
    using source_type = ruvia::detail::http_response_compression_source;
    using decision_type = ruvia::detail::http_response_compression_decision;
    using availability_type = ruvia::detail::http_response_coding_availability;
    const auto identity = response_coding(http_content_coding::identity);
    for (const auto source : {source_type::buffered, source_type::stream, source_type::sse}) {
        auto response = response_with_body(compressible_body);
        response.header("Vary", "Origin, accept-encoding");
        const auto selected = ruvia::detail::prepare_response_compression(identity,
            http_known_method::get, response, source, availability_type::identity_and_compression);
        RUVIA_CHECK(selected == decision_type::negotiated_identity);
        RUVIA_CHECK_EQ(response.header("Vary"), std::string_view("Origin, accept-encoding"));
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());

        auto disabled = response_with_body(compressible_body);
        RUVIA_CHECK(ruvia::detail::prepare_response_compression(gzip_response_coding(),
                        http_known_method::get, disabled, source, availability_type::identity_only) ==
                    decision_type::fixed_representation);
        RUVIA_CHECK(!disabled.header("Vary").has_value());

        for (const auto status : {ruvia::http_status::no_content, ruvia::http_status::not_modified,
                 ruvia::http_status::reset_content, ruvia::http_status::partial_content}) {
            auto fixed = response_with_body(compressible_body);
            fixed.status(status);
            RUVIA_CHECK(ruvia::detail::prepare_response_compression(identity,
                            http_known_method::get, fixed, source, availability_type::identity_and_compression) ==
                        decision_type::fixed_representation);
            RUVIA_CHECK(!fixed.header("Vary").has_value());
        }
        for (const auto field : {"Content-Encoding", "Content-Range", "Cache-Control"}) {
            auto fixed = response_with_body(compressible_body);
            fixed.header(field, std::string_view(field) == "Content-Encoding" ? "gzip" : std::string_view(field) == "Content-Range" ? "bytes 0-9/20"
                                                                                                                                    : "no-transform");
            RUVIA_CHECK(ruvia::detail::prepare_response_compression(identity,
                            http_known_method::get, fixed, source, availability_type::identity_and_compression) ==
                        decision_type::fixed_representation);
            RUVIA_CHECK(!fixed.header("Vary").has_value());
        }
        auto binary = response_with_body(compressible_body);
        binary.header("Content-Type", "application/octet-stream");
        const auto binary_decision = ruvia::detail::prepare_response_compression(identity,
            http_known_method::get, binary, source, availability_type::identity_and_compression);
        RUVIA_CHECK(binary_decision == (source == source_type::sse ? decision_type::negotiated_identity : decision_type::fixed_representation));
        RUVIA_CHECK(binary.header("Vary").has_value() == (source == source_type::sse));
    }
}

RUVIA_TEST(streaming_compression_owns_one_typed_encoder_lifecycle) {
    auto response = response_with_body(compressible_body);
    const auto selection = gzip_response_coding();
    ruvia::detail::http_streaming_response_compression compression(std::pmr::get_default_resource(),
        selection, ruvia::detail::http_response_coding_availability::identity_and_compression);

    compression.prepare(
        http_known_method::get, response, ruvia::http_response_stream_kind::generic);
    RUVIA_CHECK(!compression.active());
    compression.activate(
        ruvia::plan_http_response_body(http_known_method::get, response.status()));
    RUVIA_CHECK(compression.active());

    std::string encoded;
    compression.write(std::string_view(compressible_body).substr(0, 700));
    encoded.append(compression.output());
    compression.write(std::string_view(compressible_body).substr(700));
    encoded.append(compression.output());
    compression.finish();
    encoded.append(compression.output());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { compression.write("late"); }));
    compression.finish();
    RUVIA_CHECK(!compression.active());

    const auto decoded = ruvia::decode_http_content(ruvia::http_content_coding::gzip, encoded,
        {.max_decoded_bytes_ = compressible_body.size(),
            .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view(compressible_body));
    }
}

RUVIA_TEST(streaming_compression_failure_is_terminal) {
    failing_memory_resource resource;
    auto response = response_with_body(compressible_body);
    ruvia::detail::http_streaming_response_compression compression(&resource, gzip_response_coding(),
        ruvia::detail::http_response_coding_availability::identity_and_compression);
    compression.prepare(
        http_known_method::get, response, ruvia::http_response_stream_kind::generic);
    compression.activate(
        ruvia::plan_http_response_body(http_known_method::get, response.status()));
    RUVIA_CHECK(compression.active());

    resource.fail_after(0);
    const std::string chunk(4096, 'x');
    bool allocation_failed = false;
    try {
        compression.write(chunk);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    // Allocation is available again; retries must still reject a failed encoder.
    RUVIA_CHECK(ruvia::testing::throws_on([&] { compression.write("retry"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { compression.finish(); }));
    RUVIA_CHECK(compression.output().empty());
    RUVIA_CHECK(!compression.active());
}

RUVIA_TEST(streaming_compression_precommit_abort_is_terminal) {
    auto response = response_with_body(compressible_body);
    ruvia::detail::http_streaming_response_compression compression(std::pmr::get_default_resource(),
        gzip_response_coding(),
        ruvia::detail::http_response_coding_availability::identity_and_compression);
    compression.prepare(
        http_known_method::get, response, ruvia::http_response_stream_kind::generic);
    compression.abort();

    RUVIA_CHECK(!compression.active());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { compression.write("retry"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { compression.finish(); }));
}

RUVIA_TEST(streaming_compression_respects_encoder_availability_at_representation_boundary) {
    const auto selection = [] {
        http_response_coding_qualities qualities;
        qualities.update("gzip, identity;q=0");
        const auto selected = http_response_coding_selection::select(qualities);
        if (selected.selected() == nullptr) {
            throw std::logic_error("test response coding selection was empty");
        }
        return *selected.selected();
    }();

    auto response = response_with_body(compressible_body);
    ruvia::detail::http_streaming_response_compression compression(std::pmr::get_default_resource(),
        selection, ruvia::detail::http_response_coding_availability::identity_only);

    bool rejected = false;
    try {
        compression.prepare(
            http_known_method::get, response, ruvia::http_response_stream_kind::generic);
    } catch (const ruvia::http_error& error) {
        rejected = error.info().status() == ruvia::http_status::not_acceptable;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());

    http_response identity_allowed = response_with_body(compressible_body);
    http_response_coding_qualities allowed_qualities;
    allowed_qualities.update("gzip");
    const auto allowed_selection = http_response_coding_selection::select(allowed_qualities);
    RUVIA_CHECK(allowed_selection.selected() != nullptr);
    if (const auto* selected = allowed_selection.selected()) {
        ruvia::detail::http_streaming_response_compression identity_fallback(
            std::pmr::get_default_resource(), *selected,
            ruvia::detail::http_response_coding_availability::identity_only);
        identity_fallback.prepare(
            http_known_method::get, identity_allowed, ruvia::http_response_stream_kind::generic);
        identity_fallback.activate(
            ruvia::plan_http_response_body(http_known_method::get, identity_allowed.status()));
        RUVIA_CHECK(!identity_fallback.active());
        RUVIA_CHECK(!identity_allowed.header("Content-Encoding").has_value());
    }
}

RUVIA_TEST(response_compression_preflight_rejects_non_transformable_metadata) {
    const auto selection = gzip_response_coding();
    const auto eligible = [](http_response response) {
        return ruvia::detail::prepare_response_compression(gzip_response_coding(),
                   http_known_method::get, response,
                   ruvia::detail::http_response_compression_source::buffered,
                   ruvia::detail::http_response_coding_availability::identity_and_compression) ==
               ruvia::detail::http_response_compression_decision::encode;
    };

    RUVIA_CHECK(eligible(response_with_body(compressible_body)));

    auto no_transform = response_with_body(compressible_body);
    no_transform.header("Cache-Control", "no-transform");
    RUVIA_CHECK(!eligible(std::move(no_transform)));

    auto media = response_with_body(compressible_body);
    media.header("Content-Type", "image/png");
    RUVIA_CHECK(!eligible(std::move(media)));

    auto partial = response_with_body(compressible_body);
    partial.status(ruvia::http_status::partial_content);
    RUVIA_CHECK(!eligible(std::move(partial)));

    auto encoded = response_with_body(compressible_body);
    encoded.header("Content-Encoding", "gzip");
    RUVIA_CHECK(!eligible(std::move(encoded)));

    auto response = response_with_body(compressible_body);
    RUVIA_CHECK(ruvia::detail::prepare_response_compression(selection, http_known_method::get,
                    response, ruvia::detail::http_response_compression_source::stream,
                    ruvia::detail::http_response_coding_availability::identity_and_compression) ==
                ruvia::detail::http_response_compression_decision::encode);
}

RUVIA_TEST(compress_weakens_strong_etag_but_leaves_weak_and_absent) {
    // A strong ETag identifies the identity representation byte-for-byte. After
    // compression the body is a different representation (RFC 9110 8.8.1), so the
    // strong validator must be weakened to "W/..." -- otherwise a client could
    // strong-compare it (e.g. If-Range) against the compressed bytes.
    {
        auto response = response_with_body(compressible_body);
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // An already-weak ETag is a semantic (not byte-exact) validator, so it stays
    // valid across encodings and must not be double-weakened to W/W/"...".
    {
        auto response = response_with_body(compressible_body);
        response.header("ETag", "W/\"v1\"");
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    }
    // No ETag stays no ETag -- weakening never fabricates a validator.
    {
        auto response = response_with_body(compressible_body);
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK(!response.header("ETag").has_value());
    }
    // When nothing is compressed (body below min_bytes), the strong ETag is left
    // intact -- the response still is the identity representation.
    {
        auto response = response_with_body("tiny");
        response.header("ETag", "\"v1\"");
        RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 4096}));
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
    }
}

RUVIA_TEST(compress_brotli_and_zstd_emit_their_content_encoding) {
    // The gzip path is covered above; brotli and zstd are equally supported
    // codings and must set their own Content-Encoding token after compressing.
    {
        auto response = response_with_body(compressible_body);
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}, http_content_coding::brotli));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("br"));
        RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
                    std::string_view::npos);
    }
    {
        auto response = response_with_body(compressible_body);
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}, http_content_coding::zstd));
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("zstd"));
    }
}

RUVIA_TEST(buffered_response_absent_policies_skip_cors_and_compression) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Accept-Encoding: gzip\r\n\r\n");
    auto response = response_with_body(compressible_body);
    ruvia::detail::http_server_options options;
    options.compression_.reset();
    RUVIA_CHECK(!options.cors_.has_value());

    const auto negotiation = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        const auto preparation =
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        const auto write_plan = preparation.write_plan();
        RUVIA_CHECK(write_plan.matches_response(response));
        RUVIA_CHECK(write_plan.request_method() == ruvia::http_known_method::get);
        RUVIA_CHECK(!response.header("Access-Control-Allow-Origin").has_value());
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
}

RUVIA_TEST(buffered_response_coding_folds_repeated_accept_encoding_fields) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);
    const auto selected = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(selected.selected() != nullptr);
    if (const auto* coding = selected.selected()) {
        RUVIA_CHECK(coding->coding() == http_content_coding::brotli);
    }
}

RUVIA_TEST(buffered_response_coding_is_independent_of_server_encoder_availability) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);

    const auto selection = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(selection.selected() != nullptr);
    if (const auto* selected = selection.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::gzip);
        RUVIA_CHECK(!selected->identity_accepted());

        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        ruvia::detail::http_server_options options;
        options.compression_.reset();
        auto response = response_with_body(compressible_body);
        static_cast<void>(
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options));
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
        RUVIA_CHECK(
            ruvia::detail::http_response_needs_not_acceptable(policy, parsed_value.request_, response));
    }
}

RUVIA_TEST(buffered_response_compression_failure_is_not_negotiation_miss) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);

    failing_memory_resource resource;
    auto response = http_response({.resource_ = &resource});
    response.body(compressible_body);
    // Keep the already-owned identity body valid, then fail the next encoder
    // allocation. This reaches the typed encoder failure
    // branch without making response construction itself fail.
    resource.fail_after(0);

    const auto negotiation = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        ruvia::detail::http_server_options options;
        options.compression_.emplace();
        const auto preparation =
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        RUVIA_CHECK(preparation.compression_result().failed());

        const auto error = ruvia::detail::http_buffered_response_preparation_error(
            policy, parsed_value.request_, response, preparation.compression_result());
        RUVIA_CHECK(error.has_value());
        if (error.has_value()) {
            RUVIA_CHECK_EQ(error->status(), ruvia::http_status::internal_server_error);
            RUVIA_CHECK_EQ(error->code(), std::string_view("response_compression_failed"));
        }
    }
}

RUVIA_TEST(encoded_response_commit_is_transactional_on_header_allocation_failure) {
    // The encoder result is already owned by the response resource. A failure
    // while staging Content-Length must not publish Content-Encoding first:
    // otherwise the identity body would be emitted as a gzip representation.
    failing_memory_resource resource;
    auto response = http_response({.resource_ = &resource});
    response.body("identity");
    response.header("Content-Length", "8");
    std::pmr::string encoded("compressed", &resource);

    resource.fail_after(0);
    bool rejected = false;
    try {
        response.replace_body_with_content_encoding(std::move(encoded), "gzip");
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("identity"));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("8"));

    // Strong-validator weakening is staged before the body and the encoding
    // field as well. A failure there leaves all identity metadata untouched.
    resource.allow_allocations();
    auto with_etag = http_response({.resource_ = &resource});
    with_etag.body("identity");
    with_etag.header("ETag", "\"v1\"");
    std::pmr::string encoded_with_etag("compressed", &resource);
    resource.fail_after(0);
    rejected = false;
    try {
        with_etag.replace_body_with_content_encoding(std::move(encoded_with_etag), "gzip");
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(with_etag.body_bytes(), std::string_view("identity"));
    RUVIA_CHECK(!with_etag.header("Content-Encoding").has_value());
    RUVIA_CHECK_EQ(with_etag.header("ETag"), std::string_view("\"v1\""));
}
RUVIA_TEST(buffered_response_rejects_forbidden_identity_when_policy_skips_compression) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: gzip, identity;q=0\r\n\r\n");

    auto no_transform = response_with_body(compressible_body);
    no_transform.header("Cache-Control", "no-transform");
    auto options = ruvia::detail::http_server_options{};
    options.compression_.emplace();
    const auto coding = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(coding.selected() != nullptr);
    if (const auto* selected = coding.selected()) {
        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        const auto preparation = ruvia::detail::prepare_buffered_http_response(
            parsed_value.request_, policy, no_transform, options);
        const auto write_plan = preparation.write_plan();
        RUVIA_CHECK(write_plan.matches_response(no_transform));
        RUVIA_CHECK(
            ruvia::detail::http_response_needs_not_acceptable(policy, parsed_value.request_, no_transform));

        // The replacement 406 representation gets the same negotiated coding
        // opportunity. Identity is only permitted below when even that
        // terminal error cannot be represented acceptably.
        auto error = response_with_body(compressible_body);
        error.status(ruvia::http_status::not_acceptable);
        static_cast<void>(
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, error, options));
        RUVIA_CHECK_EQ(error.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(!ruvia::detail::http_response_needs_not_acceptable(policy, parsed_value.request_, error));
    }

    auto bodyless = response_with_body(compressible_body);
    bodyless.status(ruvia::http_status::no_content);
    if (const auto* selected = coding.selected()) {
        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        static_cast<void>(
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, bodyless, options));
        RUVIA_CHECK(
            !ruvia::detail::http_response_needs_not_acceptable(policy, parsed_value.request_, bodyless));
    }

    auto terminal_error = response_with_body(compressible_body);
    static_cast<void>(ruvia::detail::prepare_buffered_http_response(parsed_value.request_,
        ruvia::detail::http_response_coding_policy::disabled(), terminal_error, options));
    RUVIA_CHECK(!terminal_error.header("Content-Encoding").has_value());
}

RUVIA_TEST(buffered_response_defers_empty_coding_set_until_status_is_known) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET /empty HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);

    const auto policy = ruvia::detail::http_response_coding_policy::no_acceptable_coding();
    ruvia::detail::http_server_options options;

    auto bodyless = response_with_body("this body is suppressed by 204");
    bodyless.status(ruvia::http_status::no_content);
    const auto bodyless_preparation =
        ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, bodyless, options);
    RUVIA_CHECK(!ruvia::detail::http_buffered_response_preparation_error(
        policy, parsed_value.request_, bodyless, bodyless_preparation.compression_result())
            .has_value());

    auto bodyful = response_with_body("this representation cannot be identity");
    const auto bodyful_preparation =
        ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, bodyful, options);
    const auto bodyful_error = ruvia::detail::http_buffered_response_preparation_error(
        policy, parsed_value.request_, bodyful, bodyful_preparation.compression_result());
    RUVIA_CHECK(bodyful_error.has_value());
    if (bodyful_error.has_value()) {
        RUVIA_CHECK_EQ(bodyful_error->status(), ruvia::http_status::not_acceptable);
    }
}

RUVIA_TEST(buffered_recovery_preserves_terminal_representation_and_websocket_boundary) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET /recovery HTTP/1.1\r\nHost: x\r\nAccept-Encoding: identity;q=0, gzip;q=0, br;q=0, zstd;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);
    ruvia::detail::http_server_options options;
    using action = ruvia::detail::buffered_response_recovery_action;
    using mode = ruvia::detail::buffered_response_recovery_mode;
    for (const auto recovery_mode : {mode::negotiated_then_disabled, mode::immediately_disabled}) {
        auto policy = ruvia::detail::http_response_coding_policy::no_acceptable_coding();
        ruvia::detail::buffered_response_recovery recovery(recovery_mode);
        auto response = response_with_body("application representation");
        auto preparation = ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        const auto first = recovery.advance(policy, parsed_value.request_, response, preparation.compression_result());
        RUVIA_CHECK(first.action_ == action::handle_error);
        RUVIA_CHECK(first.error_.has_value());
        RUVIA_CHECK_EQ(first.error_->status(), ruvia::http_status::not_acceptable);
        RUVIA_CHECK(recovery.recovered());
        RUVIA_CHECK((policy.selection() == nullptr) == (recovery_mode == mode::immediately_disabled));

        response = response_with_body("custom error representation");
        response.status(ruvia::http_status::bad_request);
        response.header("x-custom-error", "preserved");
        preparation = ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        const auto second = recovery.advance(policy, parsed_value.request_, response, preparation.compression_result());
        if (recovery_mode == mode::negotiated_then_disabled) {
            RUVIA_CHECK(second.action_ == action::prepare_terminal);
            RUVIA_CHECK(!second.error_.has_value());
            RUVIA_CHECK(policy.selection() == nullptr);
            preparation = ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        } else {
            RUVIA_CHECK(second.action_ == action::ready);
        }
        RUVIA_CHECK(recovery.advance(policy, parsed_value.request_, response, preparation.compression_result()).action_ == action::ready);
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(response.header("x-custom-error"), std::optional<std::string_view>("preserved"));
        RUVIA_CHECK_EQ(preparation.write_plan().content_length(), std::size_t{27});
    }

    for (const auto status : {ruvia::http_status::no_content, ruvia::http_status::reset_content, ruvia::http_status::not_modified}) {
        auto policy = ruvia::detail::http_response_coding_policy::no_acceptable_coding();
        ruvia::detail::buffered_response_recovery recovery;
        auto response = response_with_body("suppressed representation");
        response.status(status);
        const auto preparation = ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, response, options);
        RUVIA_CHECK(recovery.advance(policy, parsed_value.request_, response, preparation.compression_result()).action_ == action::ready);
        RUVIA_CHECK(!recovery.recovered());
        RUVIA_CHECK(!preparation.write_plan().send_body());
    }
}

RUVIA_TEST(compress_skips_when_no_coding_but_preserves_head_metadata) {
    {
        auto response = response_with_body(compressible_body);
        RUVIA_CHECK(
            !try_compress(response, compression_type{.min_bytes_ = 16}, http_content_coding::identity));
    }
    {
        auto response = response_with_body(compressible_body);
        RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}, http_content_coding::gzip,
            http_known_method::head));
        const auto write_plan =
            ruvia::plan_buffered_http_response_write(http_known_method::head, response);
        RUVIA_CHECK(write_plan.body_suppressed());
        RUVIA_CHECK(!write_plan.send_body());
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
        RUVIA_CHECK(response.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
                    std::string_view::npos);
    }
}

RUVIA_TEST(compress_skips_non_compressible_status_codes) {
    // 206/204/205/304 and any 1xx must never carry a compressed representation.
    for (const ruvia::http_status_code status :
        {ruvia::http_status::partial_content, ruvia::http_status::no_content,
            ruvia::http_status::reset_content, ruvia::http_status::not_modified}) {
        auto response = response_with_body(compressible_body);
        response.status(status);
        RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
    }
}

RUVIA_TEST(compress_respects_below_min_bytes) {
    auto response = response_with_body("too small to bother");
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 1024}));
}

RUVIA_TEST(compress_respects_no_transform) {
    auto response = response_with_body(compressible_body);
    response.header("Cache-Control", "no-transform");
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
}

RUVIA_TEST(compress_respects_no_transform_in_later_cache_control_field) {
    auto response = response_with_body(compressible_body);
    response.header("Cache-Control", "public");
    response.header("Cache-Control", "no-transform",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
}

RUVIA_TEST(compress_ignores_no_transform_inside_quoted_extension) {
    auto response = response_with_body(compressible_body);
    response.header("Cache-Control", R"(extension="a, no-transform, b")");
    RUVIA_CHECK(try_compress(response, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(compress_skips_already_encoded_body) {
    auto response = response_with_body(compressible_body);
    response.header("Content-Encoding", "gzip");  // already encoded upstream
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
}

RUVIA_TEST(preencoded_response_must_be_acceptable_to_client) {
    ruvia::http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(
        "GET /encoded HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: br, identity;q=0\r\n\r\n");
    RUVIA_CHECK(parsed_value.message_ready() != nullptr);

    const auto negotiation = ruvia::detail::http_response_coding_for(parsed_value.request_);
    RUVIA_CHECK(negotiation.selected() != nullptr);
    if (const auto* selected = negotiation.selected()) {
        RUVIA_CHECK(selected->coding() == http_content_coding::brotli);

        auto buffered = response_with_body(compressible_body);
        buffered.header("Content-Encoding", "gzip");
        const auto policy = ruvia::detail::http_response_coding_policy::selected(*selected);
        ruvia::detail::http_server_options options;
        const auto preparation =
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, buffered, options);
        const auto error = ruvia::detail::http_buffered_response_preparation_error(
            policy, parsed_value.request_, buffered, preparation.compression_result());
        RUVIA_CHECK(error.has_value());
        if (error.has_value()) {
            RUVIA_CHECK_EQ(error->status(), ruvia::http_status::not_acceptable);
        }

        // A valid stack made only from Ruvia-known codings must be checked
        // member by member. Treating every multi-coding field as opaque would
        // let `gzip, br` through even though this request explicitly rejects
        // gzip.
        auto stacked = response_with_body(compressible_body);
        stacked.header("Content-Encoding", "gzip, br");
        const auto stacked_preparation =
            ruvia::detail::prepare_buffered_http_response(parsed_value.request_, policy, stacked, options);
        const auto stacked_error = ruvia::detail::http_buffered_response_preparation_error(
            policy, parsed_value.request_, stacked, stacked_preparation.compression_result());
        RUVIA_CHECK(stacked_error.has_value());
        if (stacked_error.has_value()) {
            RUVIA_CHECK_EQ(stacked_error->status(), ruvia::http_status::not_acceptable);
        }

        auto streaming = response_with_body(compressible_body);
        streaming.header("Content-Encoding", "gzip");
        ruvia::detail::http_streaming_response_compression stream_compression(
            std::pmr::get_default_resource(), *selected,
            ruvia::detail::http_response_coding_availability::identity_and_compression);
        bool rejected = false;
        try {
            stream_compression.prepare(
                http_known_method::get, streaming, ruvia::http_response_stream_kind::generic);
        } catch (const ruvia::http_error& stream_error) {
            rejected = stream_error.info().status() == ruvia::http_status::not_acceptable;
        }
        RUVIA_CHECK(rejected);

        auto stacked_streaming = response_with_body(compressible_body);
        stacked_streaming.header("Content-Encoding", "gzip, br");
        ruvia::detail::http_streaming_response_compression stacked_compression(
            std::pmr::get_default_resource(), *selected,
            ruvia::detail::http_response_coding_availability::identity_and_compression);
        bool stacked_rejected = false;
        try {
            stacked_compression.prepare(http_known_method::get, stacked_streaming,
                ruvia::http_response_stream_kind::generic);
        } catch (const ruvia::http_error& stacked_stream_error) {
            stacked_rejected =
                stacked_stream_error.info().status() == ruvia::http_status::not_acceptable;
        }
        RUVIA_CHECK(stacked_rejected);

        ruvia::http1_server_request_parser identity_parser;
        const auto identity_parsed = identity_parser.parse_message(
            "GET /encoded HTTP/1.1\r\nHost: x\r\n"
            "Accept-Encoding: identity, gzip;q=0\r\n\r\n");
        RUVIA_CHECK(identity_parsed.message_ready() != nullptr);
        const auto identity_negotiation =
            ruvia::detail::http_response_coding_for(identity_parsed.request_);
        RUVIA_CHECK(identity_negotiation.selected() != nullptr);
        if (const auto* identity_selection = identity_negotiation.selected()) {
            RUVIA_CHECK(identity_selection->coding() == http_content_coding::identity);
            auto identity_streaming = response_with_body(compressible_body);
            identity_streaming.header("Content-Encoding", "gzip");
            ruvia::detail::http_streaming_response_compression identity_compression(
                std::pmr::get_default_resource(), *identity_selection,
                ruvia::detail::http_response_coding_availability::identity_and_compression);
            bool identity_rejected = false;
            try {
                identity_compression.prepare(http_known_method::get, identity_streaming,
                    ruvia::http_response_stream_kind::generic);
            } catch (const ruvia::http_error& identity_error) {
                identity_rejected =
                    identity_error.info().status() == ruvia::http_status::not_acceptable;
            }
            RUVIA_CHECK(identity_rejected);
        }
    }
}

RUVIA_TEST(compress_declares_vary_for_negotiated_but_uncompressed_responses) {
    const auto varies = [](http_response& r) {
        return r.header("Vary").value_or(std::string_view{}).find("Accept-Encoding") !=
               std::string_view::npos;
    };

    // A compressible representation is selected by Accept-Encoding, so it must carry
    // Vary even when THIS response is left identity: below the size threshold, or the
    // client accepted no coding we support. Otherwise a shared cache serves this
    // identity body to a client that would get the compressed one (RFC 9110 12.5.5).
    {
        auto r = response_with_body("small");
        RUVIA_CHECK(!try_compress(r, compression_type{.min_bytes_ = 4096}));  // below min_bytes
        RUVIA_CHECK(varies(r));
    }
    {
        auto r = response_with_body(compressible_body);
        RUVIA_CHECK(!try_compress(r, compression_type{.min_bytes_ = 16}, http_content_coding::identity));
        RUVIA_CHECK(varies(r));
    }

    // Responses that never vary by Accept-Encoding must NOT over-declare Vary
    // (RFC 9110 12.5.5 SHOULD NOT): incompressible media type, no-transform,
    // and an already-chosen encoding.
    {
        auto r = response_with_body(compressible_body);
        r.header("Content-Type", "image/png");
        RUVIA_CHECK(!try_compress(r, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = response_with_body(compressible_body);
        r.header("Cache-Control", "no-transform");
        RUVIA_CHECK(!try_compress(r, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK(!varies(r));
    }
    {
        auto r = response_with_body(compressible_body);
        r.header("Content-Encoding", "gzip");
        RUVIA_CHECK(!try_compress(r, compression_type{.min_bytes_ = 16}));
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
    auto response = response_with_body(incompressible);
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(compress_skips_content_range_response) {
    // A range/partial representation must not be recompressed: it would invalidate
    // the byte offsets the Content-Range header describes.
    auto response = response_with_body(compressible_body);
    response.header("Content-Range", "bytes 0-2047/8192");
    RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(compress_skips_incompressible_media_types) {
    auto png = response_with_body(compressible_body);
    png.header("Content-Type", "image/png");
    RUVIA_CHECK(!try_compress(png, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK(!png.header("Content-Encoding").has_value());

    auto svg = response_with_body(compressible_body);
    svg.header("Content-Type", "image/svg+xml");
    RUVIA_CHECK(try_compress(svg, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK_EQ(svg.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(compress_skips_video_audio_and_container_media_types) {
    // Beyond image/*, the full already-compressed set is video/*, audio/*, and the
    // specific container application types. Compressing these wastes CPU for no size
    // win, so each family and each exact container type must be left uncompressed.
    for (const char* type :
        {"video/mp4", "audio/mpeg", "application/gzip", "application/x-gzip", "application/zip",
            "application/zstd", "application/pdf", "application/octet-stream"}) {
        auto response = response_with_body(compressible_body);
        response.header("Content-Type", type);
        RUVIA_CHECK(!try_compress(response, compression_type{.min_bytes_ = 16}));
        RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    }

    // A parameterised incompressible type still matches once its parameters are
    // stripped, so it is not compressed either.
    auto png = response_with_body(compressible_body);
    png.header("Content-Type", "image/png; name=photo");
    RUVIA_CHECK(!try_compress(png, compression_type{.min_bytes_ = 16}));
    RUVIA_CHECK(!png.header("Content-Encoding").has_value());
}
