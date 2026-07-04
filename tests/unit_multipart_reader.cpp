#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/MultipartReader.h"
#include "ruvia/http/Streaming.h"

#include "net/body/HttpRequestBodyFacade.h"
#include "runtime/AsioAwait.h"

namespace {

using ruvia::BodyReader;
using ruvia::MultipartReader;
using ruvia::Task;

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
        if (part->partBegin()) {
            current = CollectedPart{
                std::string(part->name()),
                std::string(part->filename()),
                std::string(part->contentType()),
                std::string()};
        }
        current.body.append(part->body());
        if (part->partEnd()) {
            out.push_back(current);
        }
    }
    co_return;
}

std::vector<CollectedPart> parseMultipart(std::vector<std::string> chunks, std::string_view boundary) {
    ChunkSource source{std::move(chunks), 0};
    std::optional<BodyReader> bodyReader;
    ruvia::detail::emplaceBodyReaderFacade(bodyReader, source);
    MultipartReader reader(*bodyReader, boundary, std::pmr::get_default_resource());

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
