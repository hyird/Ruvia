#include <limits>
#include <stdexcept>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/multipart_reader.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "streaming_fixture.h"

// What happens to a stored operation when the capability it borrowed goes away first.

namespace {

ruvia::scoped_operation<ruvia::http_response> make_expired_not_found_response() {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto [request, error] = ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
    if (error) {
        throw std::runtime_error("invalid test request");
    }
    auto context_value =
        ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());
    return context_value.not_found();
}

ruvia::task<void> await_expired_not_found_response(
    ruvia::scoped_operation<ruvia::http_response>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::sse_writer make_expired_sse_writer(capture_stream_sink& sink_value) {
    auto writer = make_writer(sink_value);
    return ruvia::detail::streaming_access::make_sse_writer(writer);
}

ruvia::sse_writer make_sse_writer_after_closed_writer_scope(capture_stream_sink& sink_value) {
    auto writer = make_writer(sink_value);
    ruvia::detail::streaming_access::release_context(writer);
    return ruvia::detail::streaming_access::make_sse_writer(writer);
}

ruvia::task<void> write_expired_sse(ruvia::sse_writer& writer, bool& rejected) {
    try {
        co_await writer.write(ruvia::sse_message{.data_ = "must-not-run"});
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::task<void> write_expired_invalid_sse(
    ruvia::sse_writer& writer, bool& lifetime_rejected, bool& validation_ran) {
    try {
        co_await writer.write(ruvia::sse_message{.data_ = "must-not-run", .event_ = "bad\nevent"});
    } catch (const std::invalid_argument&) {
        validation_ran = true;
    } catch (const std::logic_error&) {
        lifetime_rejected = true;
    }
}

ruvia::multipart_reader make_expired_multipart_reader() {
    ruvia::detail::body_reader_binding<immediate_body_source> binding;
    return ruvia::multipart_reader(
        binding.facade(), {.boundary_ = ruvia::multipart_boundary("BOUNDARY"),
                              .resource_ = ruvia::detail::process_resource()});
}

ruvia::task<void> read_expired_multipart(ruvia::multipart_reader& reader_value, bool& rejected) {
    try {
        (void)co_await reader_value.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

}  // namespace

RUVIA_TEST(response_stream_cold_operation_rejects_after_capability_teardown) {
    capture_stream_sink sink;
    auto operation = make_expired_write(sink);
    bool rejected = false;

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(await_expired_write(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK(rejected);
    RUVIA_CHECK(sink.writes_.empty());
}

RUVIA_TEST(websocket_cold_operation_rejects_after_facade_teardown) {
    capture_websocket capture;
    auto operation = make_expired_websocket_write(capture);
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(await_expired_write(operation, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(capture.writes_.empty());
}

RUVIA_TEST(body_reader_cold_operation_rejects_after_facade_teardown) {
    auto operation = make_expired_body_read();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(await_expired_body_read(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(body_reader_preserves_octets_and_shares_text_read_lane) {
    struct source {
        std::string_view bytes_{"\0\xff\xc3\xa9", 4};
        bool consumed_{false};
        ruvia::task<std::optional<std::span<const std::byte>>> read() {
            if (std::exchange(consumed_, true)) {
                co_return std::nullopt;
            }
            co_return ruvia::as_bytes(bytes_);
        }
    };
    ruvia::detail::body_reader_binding<source> binding;
    auto& reader_value = binding.facade();
    {
        auto discarded = reader_value.read();
    }
    auto operation = [&]() -> ruvia::task<void> {
        auto pending = reader_value.read();
        bool rejected = false;
        try {
            auto concurrent = reader_value.text();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        const auto bytes_value = co_await std::move(pending);
        RUVIA_CHECK(bytes_value.has_value());
        RUVIA_CHECK_EQ(bytes_value->size(), std::size_t{4});
        RUVIA_CHECK((*bytes_value)[0] == std::byte{0});
        RUVIA_CHECK((*bytes_value)[1] == std::byte{0xff});
        RUVIA_CHECK(!(co_await reader_value.read()));
    };
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::as_awaitable(operation()), asio::use_future);
    ctx.run();
    future.get();
}

RUVIA_TEST(context_not_found_cold_operation_rejects_after_context_teardown) {
    auto operation = make_expired_not_found_response();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(await_expired_not_found_response(operation, rejected)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(sse_writer_rejects_after_stream_writer_teardown) {
    capture_stream_sink sink;
    auto writer = make_expired_sse_writer(sink);
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(write_expired_sse(writer, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(writer.aborted());
    RUVIA_CHECK(sink.writes_.empty());
}

RUVIA_TEST(sse_writer_checks_lifetime_before_message_validation) {
    capture_stream_sink sink;
    auto writer = make_expired_sse_writer(sink);
    bool lifetime_rejected = false;
    bool validation_ran = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            write_expired_invalid_sse(writer, lifetime_rejected, validation_ran)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(lifetime_rejected);
    RUVIA_CHECK(!validation_ran);
    RUVIA_CHECK(sink.writes_.empty());
}

RUVIA_TEST(response_stream_writer_checks_lifetime_before_copying_payload_or_trailers) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);
    ruvia::detail::streaming_access::release_context(writer);

    const std::string_view oversized("x", (std::numeric_limits<std::size_t>::max)());
    bool write_rejected_by_lifetime = false;
    bool write_copied_payload = false;
    try {
        (void)writer.write(oversized);
    } catch (const std::length_error&) {
        write_copied_payload = true;
    } catch (const std::logic_error&) {
        write_rejected_by_lifetime = true;
    }

    const std::array<ruvia::http_header_view, 1> trailers{ruvia::http_header_view{oversized, "value"}};
    bool end_rejected_by_lifetime = false;
    bool end_copied_trailer = false;
    try {
        (void)writer.end(trailers);
    } catch (const std::length_error&) {
        end_copied_trailer = true;
    } catch (const std::logic_error&) {
        end_rejected_by_lifetime = true;
    }

    RUVIA_CHECK(write_rejected_by_lifetime);
    RUVIA_CHECK(!write_copied_payload);
    RUVIA_CHECK(end_rejected_by_lifetime);
    RUVIA_CHECK(!end_copied_trailer);
    RUVIA_CHECK(sink.writes_.empty());
    RUVIA_CHECK(sink.trailers_.empty());
}

RUVIA_TEST(sse_writer_aborted_is_safe_after_closed_writer_scope) {
    capture_stream_sink sink;
    auto writer = make_sse_writer_after_closed_writer_scope(sink);
    RUVIA_CHECK(writer.aborted());
}

RUVIA_TEST(multipart_reader_rejects_after_body_reader_teardown) {
    auto reader_value = make_expired_multipart_reader();
    bool rejected = false;
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(read_expired_multipart(reader_value, rejected)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability first(scope, expired_count);
    test_scoped_capability moved(std::move(first));
    moved.use();
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expired_count = 0;
    ruvia::operation_scope scope;
    test_scoped_capability source(scope, expired_count);
    {
        test_scoped_capability destroyed_early(source);
        destroyed_early.use();
    }
    test_scoped_capability surviving_copy(source);
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);

    bool source_rejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        source_rejected = true;
    }
    RUVIA_CHECK(source_rejected);
}

RUVIA_TEST(scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    auto operation =
        ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    RUVIA_CHECK(!destroyed);
    scope.close();
    RUVIA_CHECK(destroyed);
    (void)operation;
}
