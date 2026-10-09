#include <zlib.h>

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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/connection_scanner.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "body/http_stream_body_reader.h"
#include "server/inbound_buffer_resource.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

struct unused_body_stream final {};

struct eof_body_stream final {
    asio::io_context* io_;

    using executor_type = asio::io_context::executor_type;

    [[nodiscard]] executor_type get_executor() noexcept {
        return io_->get_executor();
    }

    template <typename buffer_type, typename handler_type>
    void async_read_some(const buffer_type&, handler_type handler) {
        asio::post(*io_, [handler = std::move(handler)]() mutable {
            handler(asio::error::eof, std::size_t{0});
        });
    }

    template <typename buffer_type, typename handler_type>
    void async_write_some(const buffer_type& buffer, handler_type handler) {
        const auto bytes_value = asio::buffer_size(buffer);
        asio::post(*io_,
            [handler = std::move(handler), bytes_value]() mutable { handler(std::error_code{}, bytes_value); });
    }
};

// Delivers a queued list of socket reads, one segment per async_read_some (a
// segment larger than the caller's buffer is split across reads). Once the
// queue drains it returns EOF, letting a test reproduce a Content-Length body
// arriving over several TCP segments.
struct segmented_body_stream final {
    asio::io_context* io_;
    std::vector<std::string> segments_;
    std::size_t index_{0};
    std::size_t offset_{0};

    using executor_type = asio::io_context::executor_type;

    [[nodiscard]] executor_type get_executor() noexcept {
        return io_->get_executor();
    }

    template <typename buffer_type, typename handler_type>
    void async_read_some(const buffer_type& buffer, handler_type handler) {
        const auto capacity = asio::buffer_size(buffer);
        if (index_ >= segments_.size() || capacity == 0) {
            asio::post(*io_, [handler = std::move(handler)]() mutable {
                handler(asio::error::eof, std::size_t{0});
            });
            return;
        }
        auto& segment = segments_[index_];
        const auto available = segment.size() - offset_;
        const auto count = std::min(capacity, available);
        asio::buffer_copy(buffer, asio::buffer(segment.data() + offset_, count));
        offset_ += count;
        if (offset_ >= segment.size()) {
            ++index_;
            offset_ = 0;
        }
        asio::post(*io_,
            [handler = std::move(handler), count]() mutable { handler(std::error_code{}, count); });
    }

    template <typename buffer_type, typename handler_type>
    void async_write_some(const buffer_type& buffer, handler_type handler) {
        const auto bytes_value = asio::buffer_size(buffer);
        asio::post(*io_,
            [handler = std::move(handler), bytes_value]() mutable { handler(std::error_code{}, bytes_value); });
    }
};

ruvia::http1_request_body_plan parse_body_plan(std::string_view wire) {
    return ruvia::http1_server_request_parser().parse_message(wire).body_plan_;
}

struct known_length_observation final {
    std::string body_;
    std::string pipeline_;
    ruvia::http1_request_body_consumption consumption_{ruvia::http1_request_body_consumption::incomplete};
    std::optional<ruvia::http_status_code> error_status_;
};

// Streams a Content-Length body whose bytes are split as: an initial segment
// carried alongside the request head (partial body prefix, plus any pipelined
// trailer) followed by `socket_segments` delivered over the socket.
known_length_observation read_known_length_body(
    std::size_t content_length, std::string initial_value, std::vector<std::string> socket_segments) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    segmented_body_stream stream{&io, std::move(socket_segments)};
    ruvia::connection_scanner::entry_type scanner_entry;
    std::pmr::monotonic_buffer_resource resource;
    auto plan = parse_body_plan("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                                std::to_string(content_length) + "\r\n\r\n");
    ruvia::detail::stream_body_reader<segmented_body_stream> reader_value(stream,
        std::pmr::polymorphic_allocator<char>(&resource), initial_value, plan,
        ruvia::protocol_byte_limit::limited(1u << 20), scanner_entry);
    known_length_observation observation;

    auto future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                while (const auto part = co_await ruvia::as_awaitable(reader_value.read())) {
                    observation.body_.append(ruvia::as_chars(*part));
                }
            } catch (const ruvia::http_protocol_error& error) {
                observation.error_status_ = error.status();
            }
        },
        asio::use_future);
    io.run();
    future.get();

    observation.consumption_ = reader_value.consumption();
    std::pmr::string pipeline(&resource);
    reader_value.take_pipeline(pipeline);
    observation.pipeline_.assign(pipeline.data(), pipeline.size());
    return observation;
}

std::string distinct_bytes(std::size_t count) {
    std::string out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<char>('A' + (i % 26)));
    }
    return out;
}

std::string zlib_deflate_compress(std::string_view input) {
    z_stream stream{};
    if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::string output(input.size() + 256, '\0');
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = deflate(&stream, Z_FINISH);
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return status == Z_STREAM_END ? output : std::string{};
}

std::string gzip_compress(std::string_view input) {
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
    std::array<char, 2 * sizeof(std::size_t)> size_bytes{};
    const auto [end, ec] =
        std::to_chars(size_bytes.data(), size_bytes.data() + size_bytes.size(), input.size(), 16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(size_bytes.data(), end);
    wire.append("\r\n");
    wire.append(input);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

std::string chunked_parts(std::string_view first, std::string_view second) {
    const auto append_wire_chunk = [](std::string& wire, std::string_view input) {
        std::array<char, 2 * sizeof(std::size_t)> size_bytes{};
        const auto [end, ec] =
            std::to_chars(size_bytes.data(), size_bytes.data() + size_bytes.size(), input.size(), 16);
        if (ec != std::errc{}) {
            return false;
        }
        wire.append(size_bytes.data(), end);
        wire.append("\r\n");
        wire.append(input);
        wire.append("\r\n");
        return true;
    };

    std::string wire;
    if (!append_wire_chunk(wire, first) || !append_wire_chunk(wire, second)) {
        return {};
    }
    wire.append("0\r\n\r\n");
    return wire;
}

struct transfer_body_observation final {
    std::string body_;
    std::string pipeline_;
    ruvia::http1_request_body_consumption consumption_{ruvia::http1_request_body_consumption::incomplete};
    std::optional<ruvia::http_status_code> error_status_;
};

transfer_body_observation read_transfer_body(std::string initial_value, bool streaming,
    std::string_view transfer_encoding = "gzip, chunked") {
    asio::io_context& io = ruvia::test::new_test_io_context();
    eof_body_stream stream{&io};
    ruvia::connection_scanner::entry_type scanner_entry;
    std::pmr::monotonic_buffer_resource resource;
    const auto plan = parse_body_plan("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: " +
                                      std::string(transfer_encoding) + "\r\n\r\n");
    ruvia::detail::stream_body_reader<eof_body_stream> reader_value(stream,
        std::pmr::polymorphic_allocator<char>(&resource), initial_value, plan,
        ruvia::protocol_byte_limit::limited(1u << 20), scanner_entry);
    transfer_body_observation observation;

    auto future = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                if (streaming) {
                    while (
                        const auto part = co_await ruvia::as_awaitable(reader_value.read())) {
                        observation.body_.append(ruvia::as_chars(*part));
                    }
                } else {
                    std::pmr::string body(&resource);
                    const auto decoded =
                        co_await ruvia::as_awaitable(reader_value.read_all(body));
                    observation.body_.assign(decoded);
                }
            } catch (const ruvia::http_protocol_error& error) {
                observation.error_status_ = error.status();
            }
        },
        asio::use_future);
    io.run();
    future.get();

    observation.consumption_ = reader_value.consumption();
    std::pmr::string pipeline(&resource);
    reader_value.take_pipeline(pipeline);
    observation.pipeline_.assign(pipeline.data(), pipeline.size());
    return observation;
}

}  // namespace

RUVIA_TEST(http1_transfer_coding_stack_preserves_pipeline_for_streaming_and_buffered_reads) {
    constexpr std::string_view plain = "two-layer request transfer coding";
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto encoded = chunked(zlib_deflate_compress(gzip_compress(plain)));
    for (const bool streaming : {false, true}) {
        const auto observation_value = read_transfer_body(
            encoded + std::string(pipeline), streaming, "gzip, deflate, chunked");
        RUVIA_CHECK(!observation_value.error_status_.has_value());
        RUVIA_CHECK_EQ(observation_value.consumption_, ruvia::http1_request_body_consumption::complete);
        RUVIA_CHECK_EQ(observation_value.body_, plain);
        RUVIA_CHECK_EQ(observation_value.pipeline_, pipeline);
    }
}

RUVIA_TEST(http1_without_body_plan_preserves_the_entire_pipeline) {
    const auto plan = parse_body_plan("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(plan.without_body() != nullptr);

    unused_body_stream stream;
    ruvia::connection_scanner::entry_type scanner_entry;
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::stream_body_reader<unused_body_stream> reader_value(stream,
        std::pmr::polymorphic_allocator<char>(&resource), "GET /next HTTP/1.1\r\nHost: x\r\n\r\n",
        plan, ruvia::protocol_byte_limit::limited(1024), scanner_entry);
    RUVIA_CHECK(reader_value.consumption() == ruvia::http1_request_body_consumption::complete);

    std::pmr::string taken(&resource);
    reader_value.take_pipeline(taken);
    RUVIA_CHECK_EQ(std::string_view(taken.data(), taken.size()),
        std::string_view("GET /next HTTP/1.1\r\nHost: x\r\n\r\n"));
}

RUVIA_TEST(http1_transfer_coding_uses_one_decoder_for_streaming_and_buffered_reads) {
    constexpr std::string_view plain = "transfer coding output shared by both body reader surfaces";
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto encoded = gzip_compress(plain);
    const auto initial_value = chunked(encoded) + std::string(pipeline);

    for (const bool streaming : {false, true}) {
        const auto observation_value = read_transfer_body(initial_value, streaming);
        RUVIA_CHECK(!observation_value.error_status_.has_value());
        RUVIA_CHECK_EQ(observation_value.body_, std::string(plain));
        RUVIA_CHECK_EQ(observation_value.pipeline_, std::string(pipeline));
        RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::complete);
    }
}

RUVIA_TEST(http1_transfer_coding_preserves_gzip_members_across_chunks) {
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto first = gzip_compress("first-");
    const auto second = gzip_compress("second");
    const auto initial_value = chunked_parts(first, second) + std::string(pipeline);
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());

    for (const bool streaming : {false, true}) {
        const auto observation_value = read_transfer_body(initial_value, streaming);
        RUVIA_CHECK(!observation_value.error_status_.has_value());
        RUVIA_CHECK_EQ(observation_value.body_, std::string("first-second"));
        RUVIA_CHECK_EQ(observation_value.pipeline_, std::string(pipeline));
        RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::complete);
    }
}

RUVIA_TEST(http1_transfer_coding_failure_maps_once_for_both_read_surfaces) {
    const auto initial_value = chunked("not-gzip");
    for (const bool streaming : {false, true}) {
        const auto observation_value = read_transfer_body(initial_value, streaming);
        RUVIA_CHECK_EQ(observation_value.error_status_, ruvia::http_status::bad_request);
        RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::incomplete);
    }
}

RUVIA_TEST(http1_streaming_content_length_body_split_across_socket_reads) {
    // The head segment carries a 30-byte body prefix; the remaining 70 bytes
    // arrive over two socket reads. Before the fix, the second read recorded a
    // buffer_-relative compaction offset while the initial view was still live,
    // so the next read re-exposed already-delivered bytes and dropped the tail.
    const auto body = distinct_bytes(100);
    const auto observation_value =
        read_known_length_body(100, body.substr(0, 30), {body.substr(30, 40), body.substr(70, 30)});
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK_EQ(observation_value.body_, body);
    RUVIA_CHECK(observation_value.pipeline_.empty());
    RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::complete);
}

RUVIA_TEST(http1_streaming_content_length_keeps_pipelined_request_out_of_body) {
    // The final socket read carries the last body bytes immediately followed by
    // a pipelined request. The leftover must reach the pipeline stash verbatim,
    // never prepended with body bytes (which would desync the next request).
    constexpr std::string_view pipeline = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto body = distinct_bytes(100);
    const auto observation_value =
        read_known_length_body(100, body.substr(0, 30), {body.substr(30, 70) + std::string(pipeline)});
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK_EQ(observation_value.body_, body);
    RUVIA_CHECK_EQ(observation_value.pipeline_, std::string(pipeline));
    RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::complete);
}

RUVIA_TEST(http1_transfer_coding_eof_commits_only_the_complete_decode_pipeline) {
    auto incomplete = gzip_compress("truncated transfer coding");
    incomplete.resize(incomplete.size() - 4);
    const auto initial_value = chunked(incomplete);
    for (const bool streaming : {false, true}) {
        const auto observation_value = read_transfer_body(initial_value, streaming);
        RUVIA_CHECK_EQ(observation_value.error_status_, ruvia::http_status::bad_request);
        RUVIA_CHECK(observation_value.consumption_ == ruvia::http1_request_body_consumption::incomplete);
    }
}

RUVIA_TEST(http1_body_reader_retains_terminal_fields_before_compacting_pipeline) {
    auto& io = ruvia::test::new_test_io_context();
    segmented_body_stream stream{&io, {"3\r\nabc\r\n0\r\nx-checksum: first\r\nx-checksum: final\r\n\r\nGET /next HTTP/1.1\r\nHost: x\r\n\r\n"}};
    ruvia::connection_scanner::entry_type scanner;
    std::pmr::monotonic_buffer_resource resource;
    auto plan = parse_body_plan("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    ruvia::detail::stream_body_reader<segmented_body_stream> reader_value(stream, std::pmr::polymorphic_allocator<char>(&resource), {}, plan,
        ruvia::protocol_byte_limit::limited(1024), scanner);
    auto run = [&]() -> ruvia::task<void> {
        RUVIA_CHECK(reader_value.trailers().fields().empty());
        std::pmr::string body(&resource);
        RUVIA_CHECK_EQ(co_await reader_value.read_all(body), "abc");
        RUVIA_CHECK_EQ(reader_value.trailers().field("X-Checksum").value_or(""), "final");
        RUVIA_CHECK_EQ(reader_value.trailers().fields().size(), std::size_t{2});
        std::pmr::string pipeline(&resource);
        reader_value.take_pipeline(pipeline);
        RUVIA_CHECK_EQ(pipeline, "GET /next HTTP/1.1\r\nHost: x\r\n\r\n");
        RUVIA_CHECK_EQ(reader_value.trailers().fields()[0].value(), "first");
    };
    auto future = asio::co_spawn(io, ruvia::as_awaitable(run()), asio::use_future);
    io.run();
    future.get();
}

RUVIA_TEST(http1_buffered_content_length_allocates_only_received_progress) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    ruvia::detail::inbound_buffer_resource budget(std::pmr::new_delete_resource(), 64 * 1024);
    eof_body_stream stream{&io};
    ruvia::connection_scanner::entry_type scanner;
    const auto plan = parse_body_plan("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 16777216\r\n\r\n");
    bool incomplete = false;
    {
        ruvia::detail::stream_body_reader<eof_body_stream> reader_value(stream,
            std::pmr::polymorphic_allocator<char>(&budget), {}, plan,
            ruvia::protocol_byte_limit::limited(16 * 1024 * 1024), scanner);
        std::pmr::string body(&budget);
        const auto empty_body_baseline = budget.used();
        {
            auto unstarted = reader_value.read_all(body);
        }
        RUVIA_CHECK_EQ(budget.used(), empty_body_baseline);
        auto result_value = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            try {
                (void)co_await ruvia::as_awaitable(reader_value.read_all(body));
            } catch (const ruvia::http_protocol_error&) {
                incomplete = true;
            } }, asio::use_future);
        io.run();
        result_value.get();
        RUVIA_CHECK(incomplete);
        RUVIA_CHECK(body.empty());
        RUVIA_CHECK(budget.used() <= empty_body_baseline + 64 * 1024);
    }
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

RUVIA_TEST(http1_buffered_content_length_preserves_pipeline_and_releases_storage) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    ruvia::detail::inbound_buffer_resource budget(std::pmr::new_delete_resource(), 64 * 1024);
    const std::string payload_value(4096, 'b');
    segmented_body_stream stream{&io, {payload_value.substr(1024, 1024), payload_value.substr(2048) + "GET /next HTTP/1.1\r\n\r\n"}};
    ruvia::connection_scanner::entry_type scanner;
    const auto plan = parse_body_plan("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4096\r\n\r\n");
    {
        ruvia::detail::stream_body_reader<segmented_body_stream> reader_value(stream,
            std::pmr::polymorphic_allocator<char>(&budget), std::string_view(payload_value).substr(0, 1024), plan,
            ruvia::protocol_byte_limit::limited(4096), scanner);
        std::pmr::string body(&budget);
        auto result_value = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            RUVIA_CHECK_EQ(co_await ruvia::as_awaitable(reader_value.read_all(body)), payload_value);
            RUVIA_CHECK_EQ(std::string_view(body), payload_value);
            std::pmr::string pipeline;
            reader_value.take_pipeline(pipeline);
            RUVIA_CHECK_EQ(pipeline, "GET /next HTTP/1.1\r\n\r\n");
            RUVIA_CHECK_EQ(std::string_view(body), payload_value); }, asio::use_future);
        io.run();
        result_value.get();
        RUVIA_CHECK(budget.used() >= payload_value.size());
    }
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}
