#include "test_harness.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>
#include <zlib.h>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/ConnectionScanner.h"
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
        asio::post(
            *io,
            [handler = std::move(handler)]() mutable {
                handler(asio::error::eof, std::size_t{0});
            });
    }

    template <typename Buffer, typename Handler>
    void async_write_some(const Buffer& buffer, Handler handler) {
        const auto bytes = asio::buffer_size(buffer);
        asio::post(
            *io,
            [handler = std::move(handler), bytes]() mutable {
                handler(std::error_code{}, bytes);
            });
    }
};

ruvia::detail::Http1RequestBodyPlan parseBodyPlan(std::string_view wire) {
    return ruvia::detail::Http1ServerRequestParser().parseMessage(wire).bodyPlan;
}

std::string gzipCompress(std::string_view input) {
    z_stream stream{};
    if (deflateInit2(
            &stream,
            Z_BEST_SPEED,
            Z_DEFLATED,
            15 + 16,
            8,
            Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<char*>(input.data()));
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
    const auto [end, ec] = std::to_chars(
        sizeBytes.data(),
        sizeBytes.data() + sizeBytes.size(),
        input.size(),
        16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(sizeBytes.data(), end);
    wire.append("\r\n");
    wire.append(input);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

struct TransferBodyObservation final {
    std::string body;
    std::string pipeline;
    ruvia::detail::Http1RequestBodyConsumption consumption{
        ruvia::detail::Http1RequestBodyConsumption::kIncomplete};
    std::uint16_t errorStatus{0};
};

TransferBodyObservation readTransferBody(
    std::string initial,
    bool streaming) {
    asio::io_context io;
    EofBodyStream stream{&io};
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    auto plan = parseBodyPlan(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n");
    ruvia::detail::StreamBodyReader<EofBodyStream> reader(
        stream,
        std::pmr::polymorphic_allocator<char>(&resource),
        initial,
        plan,
        ruvia::ProtocolByteLimit::limited(1u << 20),
        scannerEntry);
    TransferBodyObservation observation;

    auto future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                if (streaming) {
                    while (const auto part = co_await ruvia::detail::taskAsAwaitable(
                               reader.read())) {
                        observation.body.append(*part);
                    }
                } else {
                    std::pmr::string body(&resource);
                    const auto decoded = co_await ruvia::detail::taskAsAwaitable(
                        reader.readAll(body));
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
    std::size_t usedBytes = 0;
    reader.restorePipeline(pipeline, usedBytes);
    observation.pipeline.assign(pipeline.data(), usedBytes);
    return observation;
}

}  // namespace

RUVIA_TEST(http1_without_body_plan_preserves_the_entire_pipeline) {
    const auto plan = parseBodyPlan(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(plan.withoutBody() != nullptr);

    UnusedBodyStream stream;
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::StreamBodyReader<UnusedBodyStream> reader(
        stream,
        std::pmr::polymorphic_allocator<char>(&resource),
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n",
        plan,
        ruvia::ProtocolByteLimit::limited(1024),
        scannerEntry);
    RUVIA_CHECK(
        reader.consumption() ==
        ruvia::detail::Http1RequestBodyConsumption::kComplete);

    std::pmr::string restored(&resource);
    std::size_t usedBytes = 0;
    reader.restorePipeline(restored, usedBytes);
    RUVIA_CHECK_EQ(usedBytes, restored.size());
    RUVIA_CHECK_EQ(
        std::string_view(restored.data(), restored.size()),
        std::string_view("GET /next HTTP/1.1\r\nHost: x\r\n\r\n"));
}

RUVIA_TEST(http1_transfer_coding_uses_one_decoder_for_streaming_and_buffered_reads) {
    constexpr std::string_view plain =
        "transfer coding output shared by both body reader surfaces";
    constexpr std::string_view pipeline =
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto encoded = gzipCompress(plain);
    const auto initial = chunked(encoded) + std::string(pipeline);

    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK_EQ(observation.errorStatus, std::uint16_t{0});
        RUVIA_CHECK_EQ(observation.body, std::string(plain));
        RUVIA_CHECK_EQ(observation.pipeline, std::string(pipeline));
        RUVIA_CHECK(observation.consumption ==
            ruvia::detail::Http1RequestBodyConsumption::kComplete);
    }
}

RUVIA_TEST(http1_transfer_coding_failure_maps_once_for_both_read_surfaces) {
    const auto initial = chunked("not-gzip");
    for (const bool streaming : {false, true}) {
        const auto observation = readTransferBody(initial, streaming);
        RUVIA_CHECK_EQ(observation.errorStatus, std::uint16_t{400});
        RUVIA_CHECK(observation.consumption ==
            ruvia::detail::Http1RequestBodyConsumption::kIncomplete);
    }
}
