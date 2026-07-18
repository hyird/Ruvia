#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/Streaming.h"

#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/WorkerSignal.h"
#include "ruvia/core/detail/WorkerDispatcher.h"

namespace {

using ruvia::BodyReader;
using ruvia::MultipartReader;
using ruvia::Task;

static_assert(!std::is_copy_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_move_constructible_v<ruvia::MultipartParser>);
static_assert(!std::is_move_assignable_v<ruvia::MultipartParser>);
static_assert(!std::is_copy_constructible_v<MultipartReader>);
static_assert(!std::is_copy_assignable_v<MultipartReader>);
static_assert(!std::is_move_constructible_v<MultipartReader>);
static_assert(!std::is_move_assignable_v<MultipartReader>);

// A BodyReader source that yields a fixed list of chunks, then end-of-body. The
// chunks vector must outlive the reads (string_views point into it).
struct ChunkSource final {
    std::vector<std::string> chunks;
    std::size_t index = 0;

    Task<std::optional<std::string_view>> read() {
        if (index < chunks.size()) {
            co_return std::string_view(chunks[index++]);
        }
        co_return std::nullopt;
    }
};

struct SuspendedChunkSource final {
    explicit SuspendedChunkSource(asio::io_context& io)
        : dispatcher(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8)),
          worker(ruvia::detail::WorkerHandleAccess::make(dispatcher)),
          signal(worker) {}

    Task<std::optional<std::string_view>> read() {
        co_await signal.wait();
        co_return std::nullopt;
    }

    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher;
    ruvia::WorkerHandle worker;
    ruvia::detail::WorkerSignal signal;
};

Task<void> completeMultipartRead(MultipartReader& reader, bool& completed) {
    try {
        (void)co_await reader.read();
    } catch (const ruvia::HttpProtocolError&) {
        // EOF without an opening boundary is expected after the suspended read.
    }
    completed = true;
}

Task<void> rejectConcurrentMultipartRead(MultipartReader& reader, bool& rejected) {
    try {
        (void)co_await reader.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct CollectedPart final {
    std::string name;
    std::string filename;
    std::string contentType;
    std::string body;
};

// Drives the reader to completion, coalescing each part's streamed body chunks.
Task<void> collectParts(MultipartReader& reader, std::vector<CollectedPart>& out) {
    CollectedPart current;
    while (auto part = co_await reader.read()) {
        const auto phase = part->phase();
        if (phase == ruvia::MultipartChunkPhase::kFirst ||
            phase == ruvia::MultipartChunkPhase::kComplete) {
            current = CollectedPart{
                std::string(part->name()),
                std::string(part->filename()),
                std::string(part->contentType()),
                std::string()};
        }
        current.body.append(part->body());
        if (phase == ruvia::MultipartChunkPhase::kLast ||
            phase == ruvia::MultipartChunkPhase::kComplete) {
            out.push_back(current);
        }
    }
    co_return;
}

std::vector<CollectedPart> parseMultipart(std::vector<std::string> chunks, std::string_view boundary) {
    ChunkSource source{std::move(chunks), 0};
    std::optional<BodyReader> bodyReader;
    ruvia::detail::emplaceBodyReaderFacade(bodyReader, source);
    MultipartReader reader(
        *bodyReader,
        ruvia::MultipartBoundary(boundary),
        std::pmr::get_default_resource());

    std::vector<CollectedPart> parts;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(collectParts(reader, parts)), asio::use_future);
    ctx.run();
    future.get();  // propagate any parsing exception
    return parts;
}

// Split a string into fixed-size chunks to exercise the streaming reassembly.
std::vector<std::string> splitChunks(std::string_view body, std::size_t chunkSize) {
    std::vector<std::string> chunks;
    for (std::size_t offset = 0; offset < body.size(); offset += chunkSize) {
        chunks.emplace_back(body.substr(offset, chunkSize));
    }
    return chunks;
}

const std::string kTwoPartBody =
    "--BOUNDARY\r\n"
    "Content-Disposition: form-data; name=\"field1\"\r\n"
    "\r\n"
    "value1\r\n"
    "--BOUNDARY\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"f.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "file content\r\n"
    "--BOUNDARY--\r\n";

}  // namespace

RUVIA_TEST(multipart_reader_parses_parts_from_a_single_chunk) {
    const auto parts = parseMultipart({kTwoPartBody}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name, std::string("field1"));
    RUVIA_CHECK(parts[0].filename.empty());
    RUVIA_CHECK_EQ(parts[0].body, std::string("value1"));
    RUVIA_CHECK_EQ(parts[1].name, std::string("file"));
    RUVIA_CHECK_EQ(parts[1].filename, std::string("f.txt"));
    RUVIA_CHECK_EQ(parts[1].contentType, std::string("text/plain"));
    RUVIA_CHECK_EQ(parts[1].body, std::string("file content"));
}

RUVIA_TEST(multipart_reader_rejects_concurrent_consumers) {
    asio::io_context io(1);
    SuspendedChunkSource source(io);
    std::optional<BodyReader> bodyReader;
    ruvia::detail::emplaceBodyReaderFacade(bodyReader, source);
    MultipartReader reader(
        *bodyReader,
        ruvia::MultipartBoundary("BOUNDARY"),
        std::pmr::get_default_resource());
    bool firstCompleted = false;
    bool secondRejected = false;

    auto first = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            completeMultipartRead(reader, firstCompleted)),
        asio::use_future);
    auto second = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(
            rejectConcurrentMultipartRead(reader, secondRejected)),
        asio::use_future);
    asio::post(io, [&source] { source.signal.notify(); });
    io.run();
    first.get();
    second.get();

    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK(secondRejected);
}

RUVIA_TEST(multipart_reader_reassembles_across_chunk_boundaries) {
    // Feeding the same body three bytes at a time splits boundaries, headers and
    // bodies across reads; the streaming reader must reassemble them identically.
    const auto parts = parseMultipart(splitChunks(kTwoPartBody, 3), "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name, std::string("field1"));
    RUVIA_CHECK_EQ(parts[0].body, std::string("value1"));
    RUVIA_CHECK_EQ(parts[1].name, std::string("file"));
    RUVIA_CHECK_EQ(parts[1].body, std::string("file content"));
}

RUVIA_TEST(multipart_reader_accepts_transport_padding_and_exact_eof_close) {
    const std::string body =
        "--BOUNDARY \t\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY-- \t";  // closing delimiter is completed by HTTP body EOF
    for (const std::size_t chunkSize : {
             std::size_t{1}, std::size_t{4}, std::size_t{17}, std::size_t{4096}}) {
        const auto parts = parseMultipart(splitChunks(body, chunkSize), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].body, std::string("value"));
    }
}

RUVIA_TEST(multipart_reader_does_not_commit_ambiguous_close_before_more_input) {
    const std::string prefix =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--";
    bool threw = false;
    try {
        // Without an explicit input-finished phase, the first chunk used to be
        // accepted as complete and the invalid suffix was silently ignored.
        (void)parseMultipart({prefix, "X"}, "BOUNDARY");
    } catch (const ruvia::HttpProtocolError& error) {
        threw = error.status() == 400;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_reader_drains_a_split_epilogue_before_reporting_done) {
    ChunkSource source;
    source.chunks = {
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--\r\nfirst epilogue bytes",
        " and the remaining epilogue"};

    std::optional<BodyReader> bodyReader;
    ruvia::detail::emplaceBodyReaderFacade(bodyReader, source);
    MultipartReader reader(
        *bodyReader,
        ruvia::MultipartBoundary("BOUNDARY"),
        std::pmr::get_default_resource());
    std::vector<CollectedPart> parts;
    asio::io_context context(1);
    auto future = asio::co_spawn(
        context,
        ruvia::detail::taskAsAwaitable(collectParts(reader, parts)),
        asio::use_future);
    context.run();
    future.get();

    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].body, std::string("value"));
    RUVIA_CHECK_EQ(source.index, source.chunks.size());
}

RUVIA_TEST(multipart_reader_emits_an_empty_field_body) {
    // A form field with no value (name="empty" immediately followed by the next
    // boundary) has a zero-length body. The reader must still emit exactly one
    // part for it, carrying the complete phase on that empty chunk, rather
    // than swallowing the field. This hits the boundary-at-offset-0 branch that
    // the non-empty bodies never reach.
    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"empty\"\r\n"
        "\r\n"
        "\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"present\"\r\n"
        "\r\n"
        "data\r\n"
        "--BOUNDARY--\r\n";
    const auto parts = parseMultipart({body}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name, std::string("empty"));
    RUVIA_CHECK(parts[0].body.empty());
    RUVIA_CHECK_EQ(parts[1].name, std::string("present"));
    RUVIA_CHECK_EQ(parts[1].body, std::string("data"));

    // The same holds when the body is fragmented three bytes at a time, so the
    // empty part is recognized even when the boundary straddles reads.
    const auto split = parseMultipart(splitChunks(body, 3), "BOUNDARY");
    RUVIA_CHECK_EQ(split.size(), std::size_t{2});
    RUVIA_CHECK(split[0].body.empty());
    RUVIA_CHECK_EQ(split[1].body, std::string("data"));
}

RUVIA_TEST(multipart_reader_rejects_a_body_without_a_final_boundary) {
    // A body that ends mid-part (no closing --BOUNDARY--) is malformed and must
    // surface as an error rather than silently yielding a truncated part.
    const std::string truncated =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1";  // no trailing CRLF, no closing boundary
    bool threw = false;
    try {
        (void)parseMultipart({truncated}, "BOUNDARY");
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_reader_rejects_malformed_parts) {
    const auto throwsOn = [](std::string body) {
        try {
            (void)parseMultipart({std::move(body)}, "BOUNDARY");
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // A part with no "Content-Disposition: form-data" is rejected.
    RUVIA_CHECK(throwsOn(
        "--BOUNDARY\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "x\r\n"
        "--BOUNDARY--\r\n"));

    // A form-data part with no name parameter is rejected.
    RUVIA_CHECK(throwsOn(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data\r\n"
        "\r\n"
        "x\r\n"
        "--BOUNDARY--\r\n"));

    // A part whose header block exceeds the 64 KiB cap without ever terminating
    // (\r\n\r\n) is rejected rather than buffered unbounded -- a memory-DoS defense.
    std::string bigHeaders = "--BOUNDARY\r\nX-Big: ";
    bigHeaders.append(70 * 1024, 'a');
    RUVIA_CHECK(throwsOn(std::move(bigHeaders)));
}

RUVIA_TEST(multipart_reader_boundary_prefix_in_content_is_not_a_delimiter) {
    // RFC 2046 5.1.1: "\r\n--<boundary>" is a delimiter only when it ends in CRLF
    // (next part) or "--" (close). The boundary token appearing inside a part body
    // followed by any other byte is content -- the streaming reader must agree with
    // the buffered parser and not truncate/reject. Exercised across chunk sizes so
    // the false boundary lands both mid-buffer and split across a read edge.
    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "before\r\n--BOUNDARYx after"   // "\r\n--BOUNDARYx": false boundary ('x' != CRLF/--)
        "\r\n--BOUNDARY--\r\n";         // the real close delimiter
    for (const std::size_t chunkSize :
         {std::size_t{1}, std::size_t{5}, std::size_t{19}, std::size_t{4096}}) {
        const auto parts = parseMultipart(splitChunks(body, chunkSize), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].name, std::string("field"));
        RUVIA_CHECK_EQ(parts[0].body, std::string("before\r\n--BOUNDARYx after"));
    }
}

RUVIA_TEST(multipart_reader_skips_a_preamble_before_the_first_boundary) {
    // RFC 2046 §5.1.1: a preamble before the first boundary is ignored. The buffered
    // parser already skips it; the streaming reader must agree rather than reject the
    // body. Exercised across chunk sizes so the preamble/boundary split lands both
    // mid-buffer and across read edges.
    const std::string body =
        "This is a preamble a client or proxy may prepend.\r\n"
        "It can span several lines.\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value"
        "\r\n--BOUNDARY--\r\n";
    for (const std::size_t chunkSize :
         {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{4096}}) {
        const auto parts = parseMultipart(splitChunks(body, chunkSize), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].name, std::string("field"));
        RUVIA_CHECK_EQ(parts[0].body, std::string("value"));
    }

    // A bare leading CRLF (a minimal/empty preamble) is likewise skipped.
    const std::string emptyPreamble =
        "\r\n--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "\r\n"
        "v"
        "\r\n--BOUNDARY--\r\n";
    const auto parts = parseMultipart({emptyPreamble}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].body, std::string("v"));
}

RUVIA_TEST(multipart_reader_rejects_an_unbounded_preamble_without_a_boundary) {
    // A preamble that never presents a boundary must be bounded, not buffered
    // without limit -- the same memory-DoS defense as the per-part header cap.
    std::string noBoundary(70 * 1024, 'x');  // 70 KiB, never a --BOUNDARY line
    bool threw = false;
    try {
        (void)parseMultipart({std::move(noBoundary)}, "BOUNDARY");
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_reader_rejects_invalid_boundary_terminator_without_buffering_body) {
    // A boundary line followed by a malformed terminator (here a bare CR, which the
    // boundary finder accepts but the terminator check does not) must be rejected
    // immediately, NOT by buffering the entire remaining body while waiting for a
    // "\r\n"/"--" that can never appear -- the boundary-terminator phase previously
    // lacked the memory cap the preamble and per-part header phases have.
    ChunkSource source;
    source.chunks.push_back("--BOUNDARY\rXX");             // boundary + bare-CR terminator (invalid)
    source.chunks.push_back(std::string(80 * 1024, 'A'));  // large trailing payload the bug would buffer
    source.chunks.push_back(std::string(80 * 1024, 'B'));

    std::optional<BodyReader> bodyReader;
    ruvia::detail::emplaceBodyReaderFacade(bodyReader, source);
    MultipartReader reader(
        *bodyReader,
        ruvia::MultipartBoundary("BOUNDARY"),
        std::pmr::get_default_resource());

    std::vector<CollectedPart> parts;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(collectParts(reader, parts)), asio::use_future);
    ctx.run();

    bool threw = false;
    try {
        future.get();
    } catch (const ruvia::HttpProtocolError& error) {
        threw = error.status() == 413;
    }
    RUVIA_CHECK(threw);
    // Rejected without pulling the large trailing payload chunks. Without the fix the
    // reader loops appendMore() over the whole body, consuming every chunk before it
    // finally throws at end-of-body.
    RUVIA_CHECK(source.index < source.chunks.size());
}

RUVIA_TEST(multipart_reader_decodes_quoted_pairs_in_name_and_filename) {
    // RFC 7230 §3.2.6: a quoted-pair "\X" in a Content-Disposition parameter decodes
    // to X. The streaming reader must unescape name/filename (matching the buffered
    // parser) rather than surface the raw backslashes.
    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"a\\\"b\"; filename=\"x\\\\y.txt\"\r\n"
        "\r\n"
        "content"
        "\r\n--BOUNDARY--\r\n";
    const auto parts = parseMultipart(splitChunks(body, 64), "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].name, std::string("a\"b"));         // name="a\"b" -> a"b
    RUVIA_CHECK_EQ(parts[0].filename, std::string("x\\y.txt")); // filename="x\\y.txt" -> x\y.txt
    RUVIA_CHECK_EQ(parts[0].body, std::string("content"));
}
