#include "test_io_context.h"
#include "test_harness.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <zlib.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/body/HttpStreamBodyReader.h"

namespace {

struct UnusedBodyStream final {};

struct EofBodyStream final {
    asio::io_context* io;

    using executor_type = asio::io_context::executor_type;

    [[nodiscard]] executor_type get_executor() noexcept {
        return io->get_executor();
    }

    template <typename Buffer, typename Handler>
    void async_read_some(const Buffer&, Handler handler) {
        asio::post(*io, [handler = std::move(handler)]() mutable { handler(asio::error::eof, std::size_t{0}); });
    }

    template <typename Buffer, typename Handler>
    void async_write_some(const Buffer& buffer, Handler handler) {
        const auto bytes = asio::buffer_size(buffer);
        asio::post(*io, [handler = std::move(handler), bytes]() mutable { handler(std::error_code{}, bytes); });
    }
};

// Delivers a queued list of socket reads, one segment per async_read_some (a
// segment larger than the caller's buffer is split across reads). Once the
// queue drains it returns EOF, letting a test reproduce a Content-Length body
// arriving over several TCP segments.
struct SegmentedBodyStream final {
    asio::io_context* io;
    std::vector<std::string> segments;
    std::size_t index{0};
    std::size_t offset{0};

    using executor_type = asio::io_context::executor_type;

    [[nodiscard]] executor_type get_executor() noexcept {
        return io->get_executor();
    }

    template <typename Buffer, typename Handler>
    void async_read_some(const Buffer& buffer, Handler handler) {
        const auto capacity = asio::buffer_size(buffer);
        if (index >= segments.size() || capacity == 0) {
            asio::post(*io, [handler = std::move(handler)]() mutable { handler(asio::error::eof, std::size_t{0}); });
            return;
        }
        auto& segment = segments[index];
        const auto available = segment.size() - offset;
        const auto count = std::min(capacity, available);
        std::memcpy(asio::buffer_cast<void*>(buffer), segment.data() + offset, count);
        offset += count;
        if (offset >= segment.size()) {
            ++index;
            offset = 0;
        }
        asio::post(*io, [handler = std::move(handler), count]() mutable { handler(std::error_code{}, count); });
    }

    template <typename Buffer, typename Handler>
    void async_write_some(const Buffer& buffer, Handler handler) {
        const auto bytes = asio::buffer_size(buffer);
        asio::post(*io, [handler = std::move(handler), bytes]() mutable { handler(std::error_code{}, bytes); });
    }
};

ruvia::Http1RequestBodyPlan parseBodyPlan(std::string_view wire) {
    return ruvia::detail::Http1ServerRequestParser().parseMessage(wire).bodyPlan;
}

struct KnownLengthObservation final {
    std::string body;
    std::string pipeline;
    ruvia::Http1RequestBodyConsumption consumption{ruvia::Http1RequestBodyConsumption::kIncomplete};
    std::optional<ruvia::HttpStatusCode> errorStatus;
};

// Streams a Content-Length body whose bytes are split as: an initial segment
// carried alongside the request head (partial body prefix, plus any pipelined
// trailer) followed by `socketSegments` delivered over the socket.
KnownLengthObservation readKnownLengthBody(std::size_t contentLength, std::string initial, std::vector<std::string> socketSegments) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    SegmentedBodyStream stream{&io, std::move(socketSegments)};
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    auto plan = parseBodyPlan("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(contentLength) + "\r\n\r\n");
    ruvia::detail::StreamBodyReader<SegmentedBodyStream> reader(stream, std::pmr::polymorphic_allocator<char>(&resource), initial, plan, ruvia::ProtocolByteLimit::limited(1u << 20), scannerEntry);
    KnownLengthObservation observation;

    auto future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                while (const auto part = co_await ruvia::detail::taskAsAwaitable(reader.read())) {
                    observation.body.append(*part);
                }
            } catch (const ruvia::HttpProtocolError& error) {
                observation.errorStatus = error.status();
            }
        },
        asio::use_future);
    io.run();
    future.get();

    observation.consumption = reader.consumption();
    std::pmr::string pipeline(&resource);
    reader.takePipeline(pipeline);
    observation.pipeline.assign(pipeline.data(), pipeline.size());
    return observation;
}

std::string distinctBytes(std::size_t count) {
    std::string out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<char>('A' + (i % 26)));
    }
    return out;
}

std::string gzipCompress(std::string_view input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::string output;
    std::array<char, 1024> window{};
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(window.data());
        stream.avail_out = static_cast<uInt>(window.size());
        status = deflate(&stream, Z_FINISH);
        output.append(window.data(), window.size() - stream.avail_out);
    } while (status == Z_OK);
    (void)deflateEnd(&stream);
    return status == Z_STREAM_END ? output : std::string{};
}

std::string chunked(std::string_view input) {
    std::array<char, 2 * sizeof(std::size_t)> sizeBytes{};
    const auto [end, ec] = std::to_chars(sizeBytes.data(), sizeBytes.data() + sizeBytes.size(), input.size(), 16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(sizeBytes.data(), end);
    wire.append("\r\n");
    wire.append(input);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

std::string chunkedParts(std::string_view first, std::string_view second) {
    const auto appendWireChunk = [](std::string& wire, std::string_view input) {
        std::array<char, 2 * sizeof(std::size_t)> sizeBytes{};
        const auto [end, ec] = std::to_chars(sizeBytes.data(), sizeBytes.data() + sizeBytes.size(), input.size(), 16);
        if (ec != std::errc{}) {
            return false;
        }
        wire.append(sizeBytes.data(), end);
        wire.append("\r\n");
        wire.append(input);
        wire.append("\r\n");
        return true;
    };

    std::string wire;
    if (!appendWireChunk(wire, first) || !appendWireChunk(wire, second)) {
        return {};
    }
    wire.append("0\r\n\r\n");
    return wire;
}

struct TransferBodyObservation final {
    std::string body;
    std::string pipeline;
    ruvia::Http1RequestBodyConsumption consumption{ruvia::Http1RequestBodyConsumption::kIncomplete};
    std::optional<ruvia::HttpStatusCode> errorStatus;
};

TransferBodyObservation readTransferBody(std::string initial, bool streaming) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    EofBodyStream stream{&io};
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    auto plan = parseBodyPlan(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n");
    ruvia::detail::StreamBodyReader<EofBodyStream> reader(stream, std::pmr::polymorphic_allocator<char>(&resource), initial, plan, ruvia::ProtocolByteLimit::limited(1u << 20), scannerEntry);
    TransferBodyObservation observation;

    auto future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                if (streaming) {
                    while (const auto part = co_await ruvia::detail::taskAsAwaitable(reader.read())) {
                        observation.body.append(*part);
                    }
                } else {
                    std::pmr::string body(&resource);
                    const auto decoded = co_await ruvia::detail::taskAsAwaitable(reader.readAll(body));
                    observation.body.assign(decoded);
                }
            } catch (const ruvia::HttpProtocolError& error) {
                observation.errorStatus = error.status();
            }
        },
        asio::use_future);
    io.run();
    future.get();

    observation.consumption = reader.consumption();
    std::pmr::string pipeline(&resource);
    reader.takePipeline(pipeline);
    observation.pipeline.assign(pipeline.data(), pipeline.size());
    return observation;
}

}  // namespace

RUVIA_TEST(http1_without_body_plan_preserves_the_entire_pipeline) {
    const auto plan = parseBodyPlan("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(plan.withoutBody() != nullptr);

    UnusedBodyStream stream;
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::StreamBodyReader<UnusedBodyStream> reader(stream, std::pmr::polymorphic_allocator<char>(&resource), "GET /next HTTP/1.1\r\nHost: x\r\n\r\n", plan, ruvia::ProtocolByteLimit::limited(1024), scannerEntry);
    RUVIA_CHECK(reader.consumption() == ruvia::Http1RequestBodyConsumption::kComplete);

    std::pmr::string taken(&resource);
    reader.takePipeline(taken);
    RUVIA_CHECK_EQ(std::string_view(taken.data(), taken.size()), std::string_view("GET /next HTTP/1.1\r\nHost: x\r\n\r\n"));
}

RUVIA_TEST(http1_transfer_coding_uses_one_decoder_for_streaming_and_buffered_reads) {
    constexpr std::string_view plain = "transfer coding output shared by both body reader surfaces";
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto encoded = gzipCompress(plain);
    const auto initial = chunked(encoded) + std::string(pipeline);

    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK(!observation.errorStatus.has_value());
        RUVIA_CHECK_EQ(observation.body, std::string(plain));
        RUVIA_CHECK_EQ(observation.pipeline, std::string(pipeline));
        RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kComplete);
    }
}

RUVIA_TEST(http1_transfer_coding_preserves_gzip_members_across_chunks) {
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto first = gzipCompress("first-");
    const auto second = gzipCompress("second");
    const auto initial = chunkedParts(first, second) + std::string(pipeline);
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());

    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK(!observation.errorStatus.has_value());
        RUVIA_CHECK_EQ(observation.body, std::string("first-second"));
        RUVIA_CHECK_EQ(observation.pipeline, std::string(pipeline));
        RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kComplete);
    }
}

RUVIA_TEST(http1_transfer_coding_failure_maps_once_for_both_read_surfaces) {
    const auto initial = chunked("not-gzip");
    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK_EQ(observation.errorStatus, ruvia::http_status::kBadRequest);
        RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kIncomplete);
    }
}

RUVIA_TEST(http1_streaming_content_length_body_split_across_socket_reads) {
    // The head segment carries a 30-byte body prefix; the remaining 70 bytes
    // arrive over two socket reads. Before the fix, the second read recorded a
    // buffer_-relative compaction offset while the initial view was still live,
    // so the next read re-exposed already-delivered bytes and dropped the tail.
    const auto body = distinctBytes(100);
    const auto observation = readKnownLengthBody(100, body.substr(0, 30), {body.substr(30, 40), body.substr(70, 30)});
    RUVIA_CHECK(!observation.errorStatus.has_value());
    RUVIA_CHECK_EQ(observation.body, body);
    RUVIA_CHECK(observation.pipeline.empty());
    RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kComplete);
}

RUVIA_TEST(http1_streaming_content_length_keeps_pipelined_request_out_of_body) {
    // The final socket read carries the last body bytes immediately followed by
    // a pipelined request. The leftover must reach the pipeline stash verbatim,
    // never prepended with body bytes (which would desync the next request).
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto body = distinctBytes(100);
    const auto observation = readKnownLengthBody(100, body.substr(0, 30), {body.substr(30, 70) + std::string(pipeline)});
    RUVIA_CHECK(!observation.errorStatus.has_value());
    RUVIA_CHECK_EQ(observation.body, body);
    RUVIA_CHECK_EQ(observation.pipeline, std::string(pipeline));
    RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kComplete);
}

RUVIA_TEST(http1_transfer_coding_eof_commits_only_the_complete_decode_pipeline) {
    auto incomplete = gzipCompress("truncated transfer coding");
    incomplete.resize(incomplete.size() - 4);
    const auto initial = chunked(incomplete);
    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK_EQ(observation.errorStatus, ruvia::http_status::kBadRequest);
        RUVIA_CHECK(observation.consumption == ruvia::Http1RequestBodyConsumption::kIncomplete);
    }
}
