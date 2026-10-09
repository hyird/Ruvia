#include <optional>

#include "ruvia/core/asio_task.h"
#include "ruvia/http/http_response_stream.h"

#include "memory_resource_fixture.h"
#include "streaming_fixture.h"
#include "util/operation_lane_lease.h"

// Writing a streamed response: exclusive output, writeln, the terminal trailer section and the
// post-head phases.

namespace {

ruvia::task<void> write_prebuilt_chunk(ruvia::response_stream_writer& writer) {
    std::pmr::string chunk("owned-chunk", ruvia::detail::process_resource());
    co_await writer.write(std::move(chunk));
}

ruvia::task<void> write_prebuilt_text_frame(ruvia::websocket& socket) {
    std::pmr::string payload_value("owned-frame", ruvia::detail::process_resource());
    co_await socket.text(std::move(payload_value));
}

}  // namespace

RUVIA_TEST(operation_lane_lease_move_keeps_one_owner_and_failed_claim_preserves_lane) {
    bool active = false;
    std::optional<ruvia::detail::operation_lane_lease> owner;
    {
        ruvia::detail::operation_lane_lease source(active);
        RUVIA_CHECK(static_cast<bool>(source));
        owner.emplace(std::move(source));
        RUVIA_CHECK(!source);
        {
            ruvia::detail::operation_lane_lease conflict(active);
            RUVIA_CHECK(!conflict);
        }
        RUVIA_CHECK(active);
    }
    RUVIA_CHECK(active);
    owner.reset();
    RUVIA_CHECK(!active);
    {
        ruvia::detail::operation_lane_lease reclaimed(active);
        RUVIA_CHECK(static_cast<bool>(reclaimed));
    }
    RUVIA_CHECK(!active);
}

RUVIA_TEST(body_reader_rejects_concurrent_consumers_of_one_borrowed_buffer) {
    asio::io_context io(1);
    ruvia::detail::body_reader_binding<suspended_body_source> binding;
    bool first_completed = false;
    bool second_rejected = false;

    {
        auto cold = binding.facade().read();
        bool cold_rejected = false;
        try {
            auto overlapping = binding.facade().read();
        } catch (const std::logic_error&) {
            cold_rejected = true;
        }
        RUVIA_CHECK(cold_rejected);
    }

    auto first = asio::co_spawn(io,
        ruvia::as_awaitable(complete_body_read(binding.facade(), first_completed)),
        asio::use_future);
    while (!binding.reader().read_suspended_) {
        RUVIA_CHECK_EQ(io.run_one(), std::size_t{1});
    }
    RUVIA_CHECK(!first_completed);

    io.restart();
    auto second = asio::co_spawn(io,
        ruvia::as_awaitable(reject_concurrent_body_read(binding.facade(), second_rejected)),
        asio::use_future);
    asio::post(io, [&binding] { binding.reader().resume(); });
    io.run();
    first.get();
    second.get();

    RUVIA_CHECK(first_completed);
    RUVIA_CHECK(second_rejected);
}

RUVIA_TEST(websocket_rejects_overlapping_cold_operations) {
    capture_websocket capture;
    auto socket =
        ruvia::detail::websocket_access::make(*ruvia::detail::process_resource(), &capture, &read_socket, &write_socket, &close_socket);

    {
        auto cold = socket.read();
        bool read_rejected = false;
        bool close_rejected = false;
        try {
            auto overlapping = socket.read();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            read_rejected = true;
        }
        try {
            auto overlapping = socket.close();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            close_rejected = true;
        }
        RUVIA_CHECK(read_rejected);
        RUVIA_CHECK(close_rejected);
    }

    {
        auto cold = socket.text("cold");
        bool write_rejected = false;
        bool close_rejected = false;
        try {
            auto overlapping = socket.binary("overlap");
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            write_rejected = true;
        }
        try {
            auto overlapping = socket.close();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            close_rejected = true;
        }
        RUVIA_CHECK(write_rejected);
        RUVIA_CHECK(close_rejected);
        // close claimed read before finding the occupied output lane; unwinding must release read.
        auto independent_read = socket.read();
    }

    {
        auto cold = socket.close();
        bool read_rejected = false;
        bool write_rejected = false;
        bool close_rejected = false;
        try {
            auto overlapping = socket.read();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            read_rejected = true;
        }
        try {
            auto overlapping = socket.text("overlap");
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            write_rejected = true;
        }
        try {
            auto overlapping = socket.close();
            static_cast<void>(overlapping);
        } catch (const std::logic_error&) {
            close_rejected = true;
        }
        RUVIA_CHECK(read_rejected);
        RUVIA_CHECK(write_rejected);
        RUVIA_CHECK(close_rejected);
    }

    asio::io_context io(1);
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(write_stored_temporary_websocket_payload(socket)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK_EQ(capture.writes_.size(), std::size_t{1});
}

RUVIA_TEST(response_stream_byte_writes_own_and_reclaim_each_payload) {
    ruvia::test::counting_memory_resource resource;
    suspended_stream_sink sink;
    sink.suspend_next_write_ = false;
    auto writer = ruvia::detail::streaming_access::make_response_stream_writer(resource,
        &sink, &write_suspended_stream, &end_suspended_stream, &sleep_stream,
        &bind_context, &release_context, &committed, &aborted);
    const auto baseline = resource.live_allocations();
    std::vector<std::byte> payload_value(1024, std::byte{0xff});
    payload_value.front() = std::byte{0};
    {
        auto discarded = writer.write(std::span<const std::byte>(payload_value));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    RUVIA_CHECK(sink.writes_.empty());
    auto operation = [&]() -> ruvia::task<void> {
        for (int i = 0; i < 64; ++i) {
            payload_value.back() = std::byte{0x80};
            auto output = writer.write(std::span<const std::byte>(payload_value));
            payload_value.back() = std::byte{0x7f};
            co_await std::move(output);
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(sink.writes_.back().size(), payload_value.size());
            RUVIA_CHECK_EQ(sink.writes_.back().front(), '\0');
            RUVIA_CHECK_EQ(static_cast<unsigned char>(sink.writes_.back().back()), 0x80);
        }
        sink.fail_next_write_ = true;
        bool failed = false;
        try {
            co_await writer.write(std::span<const std::byte>(payload_value));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
    };
    asio::io_context io(1);
    auto result_value = asio::co_spawn(io, ruvia::as_awaitable(operation()), asio::use_future);
    io.run();
    result_value.get();
    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
}

RUVIA_TEST(response_stream_rejects_overlapping_output_operations) {
    asio::io_context io(1);
    suspended_stream_sink sink;
    auto writer = make_suspended_writer(sink);
    bool first_completed = false;
    bool write_rejected = false;
    bool end_rejected = false;
    bool failure_observed = false;

    {
        auto cold = writer.write("cold");
        bool cold_write_rejected = false;
        bool cold_end_rejected = false;
        try {
            auto overlapping_write = writer.write("overlap");
        } catch (const std::logic_error&) {
            cold_write_rejected = true;
        }
        try {
            auto overlapping_end = writer.end();
        } catch (const std::logic_error&) {
            cold_end_rejected = true;
        }
        RUVIA_CHECK(cold_write_rejected);
        RUVIA_CHECK(cold_end_rejected);
    }

    auto first = asio::co_spawn(io,
        ruvia::as_awaitable(complete_stream_write(writer, "first", first_completed)),
        asio::use_future);
    while (!sink.write_suspended_) {
        RUVIA_CHECK_EQ(io.run_one(), std::size_t{1});
    }
    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{1});
    RUVIA_CHECK(!first_completed);

    io.restart();
    auto overlapping_write = asio::co_spawn(io,
        ruvia::as_awaitable(reject_concurrent_stream_write(writer, write_rejected)),
        asio::use_future);
    auto overlapping_end = asio::co_spawn(io,
        ruvia::as_awaitable(reject_concurrent_stream_end(writer, end_rejected)),
        asio::use_future);
    io.poll();
    overlapping_write.get();
    overlapping_end.get();

    asio::post(io, [&sink] { sink.resume(); });
    io.restart();
    io.run();
    first.get();

    RUVIA_CHECK(first_completed);
    RUVIA_CHECK(write_rejected);
    RUVIA_CHECK(end_rejected);
    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.ends_, std::size_t{0});

    sink.fail_next_write_ = true;
    io.restart();
    auto failing = asio::co_spawn(io,
        ruvia::as_awaitable(observe_stream_write_failure(writer, failure_observed)),
        asio::use_future);
    io.run();
    failing.get();
    RUVIA_CHECK(failure_observed);

    io.restart();
    auto following = asio::co_spawn(io,
        ruvia::as_awaitable(complete_stream_write(writer, "following", first_completed)),
        asio::use_future);
    io.run();
    following.get();
    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{3});
    if (sink.writes_.size() >= 3) {
        RUVIA_CHECK_EQ(sink.writes_[2], std::string("following"));
    }
}

RUVIA_TEST(response_stream_writeln_emits_independent_lines) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);

    asio::io_context ctx(1);
    auto future =
        asio::co_spawn(ctx, ruvia::as_awaitable(write_lines(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes_[0], std::string("first\n"));
    RUVIA_CHECK_EQ(sink.writes_[1], std::string("second\n"));
}

RUVIA_TEST(response_stream_stored_writeln_operations_own_independent_payloads) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(write_stored_lines(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.writes_[0], std::string("stored-first\n"));
    RUVIA_CHECK_EQ(sink.writes_[1], std::string("stored-second\n"));
}

RUVIA_TEST(websocket_stored_operation_owns_temporary_payload) {
    capture_websocket capture;
    auto socket =
        ruvia::detail::websocket_access::make(*ruvia::detail::process_resource(), &capture, &read_socket, &write_socket, &close_socket);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(write_stored_temporary_websocket_payload(socket)),
        asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(capture.writes_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(capture.writes_[0], std::string("owned-payload"));
}

RUVIA_TEST(response_stream_pmr_overload_transfers_prebuilt_chunk) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(write_prebuilt_chunk(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.writes_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.writes_[0], std::string("owned-chunk"));
}

RUVIA_TEST(websocket_text_pmr_overload_transfers_prebuilt_payload) {
    capture_websocket capture;
    auto socket =
        ruvia::detail::websocket_access::make(*ruvia::detail::process_resource(), &capture, &read_socket, &write_socket, &close_socket);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(write_prebuilt_text_frame(socket)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(capture.writes_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(capture.writes_[0], std::string("owned-frame"));
}

RUVIA_TEST(response_stream_end_submits_one_terminal_trailer_section) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::as_awaitable(end_with_trailers(writer)), asio::use_future);
    ctx.run();
    future.get();

    RUVIA_CHECK_EQ(sink.trailers_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(sink.trailers_[0], std::string("Digest=sha-256=value"));
    RUVIA_CHECK_EQ(sink.trailers_[1], std::string("Server-Timing=db;dur=7"));
}

RUVIA_TEST(response_stream_stored_end_owns_trailer_names_and_values) {
    capture_stream_sink sink;
    auto writer = make_writer(sink);
    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(end_with_expired_trailer_sources(writer)), asio::use_future);
    ctx.run();
    future.get();
    RUVIA_CHECK_EQ(sink.trailers_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(sink.trailers_[0], std::string("X-Owned-Trailer=temporary-value"));
}

RUVIA_TEST(response_stream_state_drives_typed_post_head_phases) {
    using ruvia::detail::response_stream_state;
    response_stream_state bound;
    capture_stream_sink opaque_context_storage;
    auto* opaque_context = reinterpret_cast<ruvia::context*>(&opaque_context_storage);
    bound.bind_context(opaque_context, &unused_streaming_head);
    bool rebind_rejected = false;
    try {
        bound.bind_context(opaque_context, &unused_streaming_head);
    } catch (const std::logic_error&) {
        rebind_rejected = true;
    }
    RUVIA_CHECK(rebind_rejected);
    response_stream_state detached;
    detached.bind_context(opaque_context, &unused_streaming_head);
    detached.release_context();
    bool detached_context_rejected = false;
    try {
        (void)detached.streaming_head();
    } catch (const std::logic_error&) {
        detached_context_rejected = true;
    }
    RUVIA_CHECK(detached_context_rejected);
    bound.mark_committed(ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::none));
    bool committed_context_released = false;
    try {
        (void)bound.streaming_head();
    } catch (const std::logic_error& error) {
        committed_context_released =
            std::string_view(error.what()) == "response stream is already committed";
    }
    RUVIA_CHECK(committed_context_released);

    // A committed stream that allows a body accepts a chunk before end()...
    response_stream_state open;
    open.mark_committed(ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::multi_status, ruvia::http_response_trailer_intent::none));
    RUVIA_CHECK(open.commit_plan() != nullptr);
    RUVIA_CHECK_EQ(open.commit_plan()->response_status(), ruvia::http_status::multi_status);
    RUVIA_CHECK(
        open.commit_plan()->framing() == ruvia::http_response_stream_framing::http1_chunked);
    bool recommit_rejected = false;
    try {
        open.mark_committed(ruvia::plan_http_response_stream_commit(
            ruvia::http_response_stream_framing::http2_frames, ruvia::http_known_method::get,
            ruvia::http_status_code::from_value(418), ruvia::http_response_trailer_intent::none));
    } catch (const std::logic_error&) {
        recommit_rejected = true;
    }
    RUVIA_CHECK(recommit_rejected);
    RUVIA_CHECK_EQ(open.commit_plan()->response_status(), ruvia::http_status::multi_status);
    open.ensure_body_allowed();  // no throw
    open.ensure_trailers_allowed(ruvia::http_response_stream_trailer_framing::http1_chunked);

    // ...but after end() a further body chunk would land past the terminal
    // 0\r\n\r\n (HTTP/1.1) or END_STREAM (HTTP/2) and desync the connection, so
    // it must be rejected -- the same way a post-end trailer already is.
    open.mark_ended();
    open.mark_ended();  // terminal transition is idempotent
    bool body_after_end = false;
    try {
        open.ensure_body_allowed();
    } catch (const std::logic_error&) {
        body_after_end = true;
    }
    RUVIA_CHECK(body_after_end);
    bool trailer_after_end = false;
    try {
        open.ensure_trailers_allowed(ruvia::http_response_stream_trailer_framing::http1_chunked);
    } catch (const std::logic_error&) {
        trailer_after_end = true;
    }
    RUVIA_CHECK(trailer_after_end);

    // Transport failure is a terminal alternative, not a second flag that can
    // coexist with Ended. A post-commit abort retains the exact wire plan for
    // dispatch accounting, while an uncommitted abort cannot manufacture one.
    response_stream_state aborted_open;
    aborted_open.mark_committed(ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::partial_content, ruvia::http_response_trailer_intent::none));
    aborted_open.mark_aborted();
    aborted_open.mark_aborted();
    RUVIA_CHECK(aborted_open.aborted());
    RUVIA_CHECK(!aborted_open.ended());
    RUVIA_CHECK(aborted_open.committed());
    RUVIA_CHECK_EQ(aborted_open.commit_plan()->response_status(), ruvia::http_status::partial_content);
    bool end_after_abort = false;
    try {
        aborted_open.mark_ended();
    } catch (const std::logic_error&) {
        end_after_abort = true;
    }
    RUVIA_CHECK(end_after_abort);

    response_stream_state aborted_before_commit;
    aborted_before_commit.mark_aborted();
    RUVIA_CHECK(aborted_before_commit.aborted());
    RUVIA_CHECK(!aborted_before_commit.committed());
    RUVIA_CHECK(!aborted_before_commit.ended());
    RUVIA_CHECK(aborted_before_commit.commit_plan() == nullptr);

    // A suppressed body (e.g. HEAD, 204 or 304) still refuses to accept a body
    // chunk, but with the head-only completion signal: writing the body a GET
    // would have produced is correct handler behavior there, so dispatch must
    // be able to tell it apart from a post-end() sequencing bug.
    response_stream_state suppressed;
    suppressed.mark_committed(ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::head,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::none));
    bool body_rejected = false;
    try {
        suppressed.ensure_body_allowed();
    } catch (const ruvia::detail::response_stream_head_only_complete&) {
        body_rejected = true;
    }
    RUVIA_CHECK(body_rejected);

    // HTTP/2 can keep the same content-forbidden response open solely for a
    // terminal trailing-HEADERS block, without accidentally enabling DATA.
    response_stream_state trailers_only;
    trailers_only.mark_committed(ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http2_frames, ruvia::http_known_method::head,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::present));
    RUVIA_CHECK(trailers_only.committed());
    RUVIA_CHECK(!trailers_only.ended());
    bool trailers_only_body_rejected = false;
    try {
        trailers_only.ensure_body_allowed();
    } catch (const std::logic_error&) {
        trailers_only_body_rejected = true;
    }
    RUVIA_CHECK(trailers_only_body_rejected);
    trailers_only.ensure_trailers_allowed(
        ruvia::http_response_stream_trailer_framing::http2_trailing_headers);
}

RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::created);
    auto plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http_response_stream_head(
            std::move(response), ruvia::http_response_stream_kind::generic, std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
