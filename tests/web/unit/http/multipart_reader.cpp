#include "ruvia/web/multipart_reader.h"

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/streaming.h"

#include "body/http_request_body_facade.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::body_reader;
using ruvia::multipart_reader;
using ruvia::task;

// A body_reader source that yields a fixed list of chunks, then end-of-body. The
// chunks vector must outlive the reads (string_views point into it).
struct chunk_source final {
    std::vector<std::string> chunks_;
    std::size_t index_ = 0;

    task<std::optional<std::span<const std::byte>>> read() {
        if (index_ < chunks_.size()) {
            co_return ruvia::as_bytes(chunks_[index_++]);
        }
        co_return std::nullopt;
    }
};

struct suspended_chunk_source final {
    explicit suspended_chunk_source(const ruvia::worker_handle& worker_value)
        : signal_(worker_value) {}

    task<std::optional<std::span<const std::byte>>> read() {
        waiting_ = true;
        co_await signal_.wait();
        co_return std::nullopt;
    }

    ruvia::worker_signal signal_;
    bool waiting_ = false;
};

task<void> complete_multipart_read(multipart_reader& reader_value, bool& completed) {
    try {
        (void)co_await reader_value.read();
    } catch (const ruvia::http_protocol_error&) {
        // EOF without an opening boundary is expected after the suspended read.
    }
    completed = true;
}

task<void> reject_concurrent_multipart_read(multipart_reader& reader_value, bool& rejected) {
    try {
        (void)co_await reader_value.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct collected_part final {
    std::string name_;
    std::string filename_;
    std::string content_type_;
    std::string body_;
};

// Drives the reader to completion, coalescing each part's streamed body chunks.
task<void> collect_parts(multipart_reader& reader_value, std::vector<collected_part>& out) {
    collected_part current;
    while (auto part = co_await reader_value.read()) {
        const auto phase = part->phase();
        if (phase == ruvia::multipart_chunk_phase::first ||
            phase == ruvia::multipart_chunk_phase::complete) {
            current = collected_part{std::string(part->name()), std::string(part->filename()),
                std::string(part->content_type()), std::string()};
        }
        current.body_.append(part->body());
        if (phase == ruvia::multipart_chunk_phase::last ||
            phase == ruvia::multipart_chunk_phase::complete) {
            out.push_back(current);
        }
    }
    co_return;
}

std::vector<collected_part> parse_multipart(
    std::vector<std::string> chunks, std::string_view boundary) {
    chunk_source source_value{std::move(chunks), 0};
    std::optional<body_reader> body_reader;
    ruvia::detail::emplace_body_reader_facade(body_reader, source_value);
    multipart_reader reader_value(*body_reader, {.boundary_ = ruvia::multipart_boundary(boundary),
                                                    .resource_ = std::pmr::get_default_resource()});

    std::vector<collected_part> parts;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(collect_parts(reader_value, parts)), asio::use_future);
    ctx.run();
    future.get();  // propagate any parsing exception
    return parts;
}

// Split a string into fixed-size chunks to exercise the streaming reassembly.
std::vector<std::string> split_chunks(std::string_view body, std::size_t chunk_size) {
    std::vector<std::string> chunks;
    for (std::size_t offset = 0; offset < body.size(); offset += chunk_size) {
        chunks.emplace_back(body.substr(offset, chunk_size));
    }
    return chunks;
}

const std::string two_part_body =
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
    const auto parts = parse_multipart({two_part_body}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name_, std::string("field1"));
    RUVIA_CHECK(parts[0].filename_.empty());
    RUVIA_CHECK_EQ(parts[0].body_, std::string("value1"));
    RUVIA_CHECK_EQ(parts[1].name_, std::string("file"));
    RUVIA_CHECK_EQ(parts[1].filename_, std::string("f.txt"));
    RUVIA_CHECK_EQ(parts[1].content_type_, std::string("text/plain"));
    RUVIA_CHECK_EQ(parts[1].body_, std::string("file content"));
}

RUVIA_TEST(multipart_reader_rejects_concurrent_consumers) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    suspended_chunk_source source(worker_value);
    std::optional<body_reader> body_reader;
    ruvia::detail::emplace_body_reader_facade(body_reader, source);
    multipart_reader reader_value(*body_reader, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                                    .resource_ = std::pmr::get_default_resource()});
    bool first_completed = false;
    bool second_rejected = false;

    {
        auto cold = reader_value.read();
        bool cold_rejected = false;
        try {
            auto overlapping = reader_value.read();
        } catch (const std::logic_error&) {
            cold_rejected = true;
        }
        RUVIA_CHECK(cold_rejected);
    }

    std::exception_ptr first_failure;
    std::exception_ptr second_failure;
    int completed_operations = 0;
    const auto complete_value = [&] {
        if (++completed_operations == 2) {
            io.stop();
        }
    };
    asio::co_spawn(io, ruvia::as_awaitable(complete_multipart_read(reader_value, first_completed)),
        [&](std::exception_ptr failure) {
            first_failure = failure;
            complete_value();
        });
    // Pause after the first operation suspends, without stopping its coroutine.
    asio::post(io, [&io] { io.stop(); });
    attachment.run();
    RUVIA_CHECK(source.waiting_);
    RUVIA_CHECK(!first_completed);

    io.restart();
    asio::co_spawn(io, ruvia::as_awaitable(reject_concurrent_multipart_read(reader_value, second_rejected)),
        [&](std::exception_ptr failure) {
            second_failure = failure;
            complete_value();
        });
    bool notify_on_worker = false;
    asio::post(io, [&source, &attachment, &notify_on_worker] {
        notify_on_worker = attachment.loop().is_current();
        source.signal_.notify();
    });
    attachment.run();
    if (first_failure) {
        std::rethrow_exception(first_failure);
    }
    if (second_failure) {
        std::rethrow_exception(second_failure);
    }

    RUVIA_CHECK(first_completed);
    RUVIA_CHECK(second_rejected);
    RUVIA_CHECK(notify_on_worker);
}

RUVIA_TEST(multipart_reader_reassembles_across_chunk_boundaries) {
    // Feeding the same body three bytes at a time splits boundaries, headers and
    // bodies across reads; the streaming reader must reassemble them identically.
    const auto parts = parse_multipart(split_chunks(two_part_body, 3), "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name_, std::string("field1"));
    RUVIA_CHECK_EQ(parts[0].body_, std::string("value1"));
    RUVIA_CHECK_EQ(parts[1].name_, std::string("file"));
    RUVIA_CHECK_EQ(parts[1].body_, std::string("file content"));
}

RUVIA_TEST(multipart_reader_accepts_transport_padding_and_exact_eof_close) {
    const std::string body =
        "--BOUNDARY \t\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY-- \t";  // closing delimiter is completed by HTTP body EOF
    for (const std::size_t chunk_size :
        {std::size_t{1}, std::size_t{4}, std::size_t{17}, std::size_t{4096}}) {
        const auto parts = parse_multipart(split_chunks(body, chunk_size), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].body_, std::string("value"));
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
        (void)parse_multipart({prefix, "X"}, "BOUNDARY");
    } catch (const ruvia::http_protocol_error& error) {
        threw = error.status() == ruvia::http_status::bad_request;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_reader_drains_a_split_epilogue_before_reporting_done) {
    chunk_source source;
    source.chunks_ = {
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--\r\nfirst epilogue bytes",
        " and the remaining epilogue"};

    std::optional<body_reader> body_reader;
    ruvia::detail::emplace_body_reader_facade(body_reader, source);
    multipart_reader reader_value(*body_reader, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                                    .resource_ = std::pmr::get_default_resource()});
    std::vector<collected_part> parts;
    asio::io_context context_value(1);
    auto future = asio::co_spawn(
        context_value, ruvia::as_awaitable(collect_parts(reader_value, parts)), asio::use_future);
    context_value.run();
    future.get();

    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].body_, std::string("value"));
    RUVIA_CHECK_EQ(source.index_, source.chunks_.size());
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
    const auto parts = parse_multipart({body}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name_, std::string("empty"));
    RUVIA_CHECK(parts[0].body_.empty());
    RUVIA_CHECK_EQ(parts[1].name_, std::string("present"));
    RUVIA_CHECK_EQ(parts[1].body_, std::string("data"));

    // The same holds when the body is fragmented three bytes at a time, so the
    // empty part is recognized even when the boundary straddles reads.
    const auto split = parse_multipart(split_chunks(body, 3), "BOUNDARY");
    RUVIA_CHECK_EQ(split.size(), std::size_t{2});
    RUVIA_CHECK(split[0].body_.empty());
    RUVIA_CHECK_EQ(split[1].body_, std::string("data"));
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
        (void)parse_multipart({truncated}, "BOUNDARY");
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(multipart_reader_rejects_malformed_parts) {
    const auto throws_on = [](std::string body) {
        try {
            (void)parse_multipart({std::move(body)}, "BOUNDARY");
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // A part with no "Content-Disposition: form-data" is rejected.
    RUVIA_CHECK(
        throws_on("--BOUNDARY\r\n"
                  "Content-Type: text/plain\r\n"
                  "\r\n"
                  "x\r\n"
                  "--BOUNDARY--\r\n"));

    // A form-data part with no name parameter is rejected.
    RUVIA_CHECK(
        throws_on("--BOUNDARY\r\n"
                  "Content-Disposition: form-data\r\n"
                  "\r\n"
                  "x\r\n"
                  "--BOUNDARY--\r\n"));

    // A part whose header block exceeds the 64 KiB cap without ever terminating
    // (\r\n\r\n) is rejected rather than buffered unbounded -- a memory-DoS defense.
    std::string big_headers = "--BOUNDARY\r\nX-Big: ";
    big_headers.append(70 * 1024, 'a');
    RUVIA_CHECK(throws_on(std::move(big_headers)));
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
        "before\r\n--BOUNDARYx after"  // "\r\n--BOUNDARYx": false boundary ('x' != CRLF/--)
        "\r\n--BOUNDARY--\r\n";        // the real close delimiter
    for (const std::size_t chunk_size :
        {std::size_t{1}, std::size_t{5}, std::size_t{19}, std::size_t{4096}}) {
        const auto parts = parse_multipart(split_chunks(body, chunk_size), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].name_, std::string("field"));
        RUVIA_CHECK_EQ(parts[0].body_, std::string("before\r\n--BOUNDARYx after"));
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
    for (const std::size_t chunk_size :
        {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{4096}}) {
        const auto parts = parse_multipart(split_chunks(body, chunk_size), "BOUNDARY");
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        RUVIA_CHECK_EQ(parts[0].name_, std::string("field"));
        RUVIA_CHECK_EQ(parts[0].body_, std::string("value"));
    }

    // A bare leading CRLF (a minimal/empty preamble) is likewise skipped.
    const std::string empty_preamble =
        "\r\n--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "\r\n"
        "v"
        "\r\n--BOUNDARY--\r\n";
    const auto parts = parse_multipart({empty_preamble}, "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].body_, std::string("v"));
}

RUVIA_TEST(multipart_reader_rejects_an_unbounded_preamble_without_a_boundary) {
    // A preamble that never presents a boundary must be bounded, not buffered
    // without limit -- the same memory-DoS defense as the per-part header cap.
    std::string no_boundary(70 * 1024, 'x');  // 70 KiB, never a --BOUNDARY line
    bool threw = false;
    try {
        (void)parse_multipart({std::move(no_boundary)}, "BOUNDARY");
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
    chunk_source source;
    source.chunks_.push_back("--BOUNDARY\rXX");  // boundary + bare-CR terminator (invalid)
    source.chunks_.push_back(
        std::string(80 * 1024, 'A'));  // large trailing payload the bug would buffer
    source.chunks_.push_back(std::string(80 * 1024, 'B'));

    std::optional<body_reader> body_reader;
    ruvia::detail::emplace_body_reader_facade(body_reader, source);
    multipart_reader reader_value(*body_reader, {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                                                    .resource_ = std::pmr::get_default_resource()});

    std::vector<collected_part> parts;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(collect_parts(reader_value, parts)), asio::use_future);
    ctx.run();

    bool threw = false;
    try {
        future.get();
    } catch (const ruvia::http_protocol_error& error) {
        threw = error.status() == ruvia::http_status::content_too_large;
    }
    RUVIA_CHECK(threw);
    // Rejected without pulling the large trailing payload chunks. Without the fix the
    // reader loops append_more() over the whole body, consuming every chunk before it
    // finally throws at end-of-body.
    RUVIA_CHECK(source.index_ < source.chunks_.size());
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
    const auto parts = parse_multipart(split_chunks(body, 64), "BOUNDARY");
    RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
    RUVIA_CHECK_EQ(parts[0].name_, std::string("a\"b"));          // name="a\"b" -> a"b
    RUVIA_CHECK_EQ(parts[0].filename_, std::string("x\\y.txt"));  // filename="x\\y.txt" -> x\y.txt
    RUVIA_CHECK_EQ(parts[0].body_, std::string("content"));
}
