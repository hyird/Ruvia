#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

ruvia::task<void> exercise_backpressure(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.routes_.handlers_.response_body_.assign(48 * 1024, 'z');
    feed_request(fixture_value, 0, "GET", "/large");
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    published_wire wire;
    const message_id_type id{epoch, generation, 0};

    const auto first = dispatch.publish_step();
    RUVIA_CHECK(first.status_ == dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(first.block_reason_ == block_reason_type::none);
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    const auto first_size = dispatch.published_wire_bytes();
    const auto blocked_data = dispatch.publish_step();
    RUVIA_CHECK(blocked_data.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_data.block_reason_ == block_reason_type::data);
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), first_size);
    RUVIA_CHECK(drain_data_only(fixture_value.outbound_, id, wire) == 1);

    // Keep the independent control lane full until the response cursor reaches FIN.
    const auto filler = fixture_value.outbound_.try_send_control(
        {control_type::kind::writable, id, 0});
    RUVIA_CHECK(control_accepted(filler));
    bool blocked_fin = false;
    std::size_t attempts = 0;
    while (!blocked_fin && !dispatch.complete() && ++attempts < 10000) {
        const auto demand = dispatch.publication_demand();
        RUVIA_CHECK(demand == publication_demand_type::data ||
                    demand == publication_demand_type::control);
        const auto result_value = dispatch.publish_step();
        if (result_value.status_ == dispatch_type::publish_status_type::bytes_published) {
            RUVIA_CHECK(demand == publication_demand_type::data);
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::none);
            RUVIA_CHECK(result_value.bytes_published_ <= buffer::max_block_bytes);
            RUVIA_CHECK(drain_data_only(fixture_value.outbound_, id, wire) == 1);
        } else if (result_value.status_ == dispatch_type::publish_status_type::backpressured) {
            const auto received_value = drain_data_only(fixture_value.outbound_, id, wire);
            if (received_value == 0) {
                blocked_fin = true;
                RUVIA_CHECK(demand == publication_demand_type::control);
                RUVIA_CHECK(result_value.block_reason_ == block_reason_type::control);
                RUVIA_CHECK(!dispatch.complete());
            } else {
                RUVIA_CHECK(demand == publication_demand_type::data);
                RUVIA_CHECK(result_value.block_reason_ == block_reason_type::data);
            }
        } else {
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::none);
            RUVIA_CHECK(result_value.status_ != dispatch_type::publish_status_type::failed);
            if (result_value.status_ == dispatch_type::publish_status_type::fin_published) {
                break;
            }
        }
    }
    RUVIA_CHECK(blocked_fin);
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::control);
    const auto before_fin = dispatch.published_wire_bytes();
    control_type queued_filler;
    RUVIA_CHECK(fixture_value.outbound_.try_receive_control(queued_filler));
    RUVIA_CHECK(queued_filler.kind_ == control_type::kind::writable);

    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::control);
    const auto fin = dispatch.publish_step();
    RUVIA_CHECK(fin.status_ == dispatch_type::publish_status_type::fin_published);
    RUVIA_CHECK(fin.block_reason_ == block_reason_type::none);
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), before_fin);
    drain_buffer(fixture_value.outbound_, id, wire);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::local_complete);
    RUVIA_CHECK_EQ(wire.bytes_.size(), before_fin);
    RUVIA_CHECK_EQ(wire.final_wire_bytes_.value_or(0), before_fin);
    decoded_response response;
    const auto decoded = decode_published(
        wire, ruvia::http_known_method::get, 0, fixture_value.worker_.resource(), response);
    RUVIA_CHECK(decoded.status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(response.body_.size(), std::size_t{48 * 1024});
    RUVIA_CHECK(std::ranges::all_of(response.body_, [](char value) { return value == 'z'; }));
    const auto complete_value = dispatch.publish_step();
    RUVIA_CHECK(complete_value.status_ == dispatch_type::publish_status_type::complete);
    RUVIA_CHECK(complete_value.block_reason_ == block_reason_type::none);

    feed_request(fixture_value, 4, "GET", "/empty-file");
    auto cancelled_at_control = fixture_value.make_dispatch(4, fixture_value.services_);
    RUVIA_CHECK(co_await cancelled_at_control.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(cancelled_at_control.publish_step().status_ ==
                dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(cancelled_at_control.publication_demand() == publication_demand_type::control);
    const auto control_filler = fixture_value.outbound_.try_send_control(
        {control_type::kind::writable, {epoch, generation, 4}, 0});
    RUVIA_CHECK(control_accepted(control_filler));
    const auto blocked_control = cancelled_at_control.publish_step();
    RUVIA_CHECK(blocked_control.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_control.block_reason_ == block_reason_type::control);
    RUVIA_CHECK(cancelled_at_control.publication_demand() == publication_demand_type::control);
    cancelled_at_control.cancel();
    RUVIA_CHECK(cancelled_at_control.publication_demand() ==
                publication_demand_type::local_cancelled);
    RUVIA_CHECK(cancelled_at_control.publish_step().status_ ==
                dispatch_type::publish_status_type::cancelled);
    control_type preserved_filler;
    RUVIA_CHECK(fixture_value.outbound_.try_receive_control(preserved_filler));
    RUVIA_CHECK(preserved_filler.kind_ == control_type::kind::writable);
    published_wire cancelled_wire;
    drain_buffer(fixture_value.outbound_, {epoch, generation, 4}, cancelled_wire);
    RUVIA_CHECK(!cancelled_wire.final_wire_bytes_.has_value());
    RUVIA_CHECK(fixture_value.session_.request(4) == nullptr);

    feed_request(fixture_value, 8, "GET", "/large");
    auto stop_after_block = fixture_value.make_dispatch(8, fixture_value.services_);
    RUVIA_CHECK(co_await stop_after_block.run_handler() == dispatch_type::run_status_type::response_ready);
    RUVIA_CHECK(stop_after_block.publication_demand() == publication_demand_type::data);
    const auto queued = stop_after_block.publish_step();
    RUVIA_CHECK(queued.status_ == dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(queued.block_reason_ == block_reason_type::none);
    RUVIA_CHECK(stop_after_block.publication_demand() == publication_demand_type::data);
    const auto blocked_before_stop = stop_after_block.publish_step();
    RUVIA_CHECK(blocked_before_stop.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_before_stop.block_reason_ == block_reason_type::data);
    RUVIA_CHECK(stop_after_block.publication_demand() == publication_demand_type::data);
    const auto before_stop = stop_after_block.published_wire_bytes();
    RUVIA_CHECK(fixture_value.outbound_.stop());
    const auto allocations_at_stop = fixture_value.upstream_.allocation_count();
    const auto returns_at_stop = fixture_value.upstream_.deallocation_count();
    const auto live_at_stop = fixture_value.upstream_.live_allocations();
    RUVIA_CHECK(stop_after_block.publication_demand() ==
                publication_demand_type::local_buffer_stopped);
    RUVIA_CHECK_EQ(fixture_value.upstream_.allocation_count(), allocations_at_stop);
    RUVIA_CHECK_EQ(fixture_value.upstream_.deallocation_count(), returns_at_stop);
    RUVIA_CHECK_EQ(fixture_value.upstream_.live_allocations(), live_at_stop);
    const auto stopped = stop_after_block.publish_step();
    RUVIA_CHECK(stopped.status_ == dispatch_type::publish_status_type::failed);
    RUVIA_CHECK(stopped.block_reason_ == block_reason_type::none);
    RUVIA_CHECK_EQ(stop_after_block.published_wire_bytes(), before_stop);
}

ruvia::task<void> publish_standard_response_and_measure(fixture& fixture_value, std::uint64_t stream_id,
    std::size_t& decoded_field_section_size, std::size_t& encoded_field_section_size,
    ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, stream_id, "GET", "/large");
    auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_);
    RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
    published_wire wire;
    publish_and_drain(dispatch, fixture_value, stream_id, wire, ruvia_ctx);
    decoded_response response;
    RUVIA_CHECK(decode_published(wire, ruvia::http_known_method::get, stream_id,
                    fixture_value.worker_.resource(), response)
                    .status_ == ruvia::http3_client_response_status::message_end);
    RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
    decoded_field_section_size = response.decoded_field_section_size_;
    const auto headers = ruvia::decode_http3_frame(
        std::span<const char>(wire.bytes_.data(), wire.bytes_.size()));
    RUVIA_CHECK((headers.index() == 0));
    if ((headers.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(headers).type_,
            static_cast<std::uint64_t>(ruvia::http3_frame_type::headers));
        encoded_field_section_size = std::get<0>(headers).payload_.size();
    }
    RUVIA_CHECK(fixture_value.session_.request(stream_id) == nullptr);
}

ruvia::task<void> exercise_unlimited_peer_field_section_size(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    RUVIA_CHECK(!fixture_value.session_.peer_max_field_section_size().has_value());
    std::size_t decoded_size{};
    std::size_t encoded_size{};
    co_await publish_standard_response_and_measure(fixture_value, 0, decoded_size, encoded_size, ruvia_ctx);
    RUVIA_CHECK(decoded_size > encoded_size);
    RUVIA_CHECK(!fixture_value.session_.peer_max_field_section_size().has_value());

    feed_peer_settings(fixture_value, std::nullopt);
    RUVIA_CHECK(!fixture_value.session_.peer_max_field_section_size().has_value());
    co_await publish_standard_response_and_measure(fixture_value, 4, decoded_size, encoded_size, ruvia_ctx);
    RUVIA_CHECK(decoded_size > encoded_size);
    RUVIA_CHECK(!fixture_value.session_.peer_max_field_section_size().has_value());
}

ruvia::task<void> exercise_nonbuffered_peer_refusal(
    const ruvia::worker_handle& worker_value, ruvia::test::counting_memory_resource& memory,
    ruvia::testing::test_context& ruvia_ctx) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        fixture fixture(worker_value, memory);
        feed_peer_settings(fixture, 0);
        fixture.routes_.handlers_.send_interim_ = mode == 1 || mode == 2;
        if (mode == 3) {
            const std::array fields_value{
                ruvia::http3_field_section_field_view{":method", "CONNECT"},
                ruvia::http3_field_section_field_view{":authority", "backend.test:443"}};
            const auto encoded = ruvia::encode_http3_field_section(fields_value, fixture.worker_.resource());
            RUVIA_CHECK((encoded.index() == 0));
            const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture.session_.feed(0, wire).scope_ == ruvia::http3_connection_error_scope::none);
        } else {
            feed_request(fixture, 0, "GET", mode == 2 ? "/large" : "/stream");
        }
        auto dispatch = fixture.make_dispatch(0, fixture.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::peer_field_section_limit);
        // The router invokes its error handler and constructs its fallback
        // response in every refusal mode. Even though that response is ready,
        // the peer limit forbids publishing a replacement HEADERS section.
        RUVIA_CHECK(fixture.routes_.handlers_.error_handler_called_);
        RUVIA_CHECK(fixture.routes_.handlers_.error_handler_response_ready_);
        RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::local_peer_limit_rejected);
        RUVIA_CHECK(dispatch.publish_step().status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), std::uint64_t{0});
        RUVIA_CHECK(fixture.session_.request(0) == nullptr);
        RUVIA_CHECK_EQ(fixture.session_.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(!fixture.session_.terminated());
        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!fixture.outbound_.try_receive(block));
        RUVIA_CHECK(!fixture.outbound_.try_receive_control(control));
    }
}

ruvia::task<void> exercise_decoded_peer_field_section_limit(
    const ruvia::worker_handle& worker_handle_value,
    ruvia::test::counting_memory_resource& reference_memory,
    ruvia::test::counting_memory_resource& exact_limit_memory,
    ruvia::test::counting_memory_resource& below_limit_memory,
    ruvia::testing::test_context& ruvia_ctx) {
    std::size_t decoded_size{};
    std::size_t encoded_size{};
    {
        fixture reference(worker_handle_value, reference_memory);
        co_await publish_standard_response_and_measure(reference, 0, decoded_size, encoded_size, ruvia_ctx);
    }
    RUVIA_CHECK(decoded_size > encoded_size);
    RUVIA_CHECK(decoded_size > 0);

    {
        fixture exact_limit(worker_handle_value, exact_limit_memory);
        feed_peer_settings(exact_limit, decoded_size);
        RUVIA_CHECK(exact_limit.session_.peer_max_field_section_size() == decoded_size);
        feed_request(exact_limit, 0, "GET", "/large");
        auto dispatch = exact_limit.make_dispatch(0, exact_limit.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        published_wire wire;
        publish_and_drain(dispatch, exact_limit, 0, wire, ruvia_ctx);
        decoded_response response;
        RUVIA_CHECK(decode_published(wire, ruvia::http_known_method::get, 0,
                        exact_limit.worker_.resource(), response)
                        .status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.decoded_field_section_size_, decoded_size);
        RUVIA_CHECK(dispatch.complete());
    }

    {
        fixture below_limit(worker_handle_value, below_limit_memory);
        feed_peer_settings(below_limit, decoded_size - 1);
        RUVIA_CHECK(below_limit.session_.peer_max_field_section_size() == decoded_size - 1);
        feed_request(below_limit, 0, "GET", "/large");
        auto dispatch = below_limit.make_dispatch(0, below_limit.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::peer_field_section_limit);
        const auto rejected = dispatch.publish_step();
        RUVIA_CHECK(rejected.status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), std::uint64_t{0});
        RUVIA_CHECK(dispatch.failure() == nullptr);
        RUVIA_CHECK(!dispatch.response_ready());
        RUVIA_CHECK(below_limit.session_.request(0) == nullptr);
        RUVIA_CHECK_EQ(below_limit.session_.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(!below_limit.session_.terminated());
        const auto repeated = dispatch.publish_step();
        RUVIA_CHECK(repeated.status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!below_limit.outbound_.try_receive(block));
        RUVIA_CHECK(!below_limit.outbound_.try_receive_control(control));
    }
}

ruvia::task<void> exercise_late_peer_field_section_limit(
    const ruvia::worker_handle& worker_handle_value,
    ruvia::test::counting_memory_resource& before_handoff_memory,
    ruvia::test::counting_memory_resource& committed_memory,
    ruvia::testing::test_context& ruvia_ctx) {
    {
        fixture before_handoff(worker_handle_value, before_handoff_memory);
        feed_request(before_handoff, 0, "GET", "/large");
        auto dispatch = before_handoff.make_dispatch(0, before_handoff.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);
        const message_id_type filler_id{epoch, generation, 100};
        const std::array<std::byte, 1> filler_bytes{std::byte{0x5a}};
        RUVIA_CHECK(send_accepted(before_handoff.outbound_.try_send(filler_id, filler_bytes)));
        RUVIA_CHECK(control_accepted(before_handoff.outbound_.try_send_control(
            {control_type::kind::writable, filler_id, 0})));
        feed_peer_settings(before_handoff, 0);
        RUVIA_CHECK(before_handoff.session_.peer_max_field_section_size() == 0);
        const auto allocations = before_handoff_memory.allocation_count();
        const auto returns = before_handoff_memory.deallocation_count();
        RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::local_peer_limit_rejected);
        RUVIA_CHECK_EQ(before_handoff_memory.allocation_count(), allocations);
        RUVIA_CHECK_EQ(before_handoff_memory.deallocation_count(), returns);
        const auto rejected = dispatch.publish_step();
        RUVIA_CHECK(rejected.status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), std::uint64_t{0});
        RUVIA_CHECK(dispatch.failure() == nullptr);
        RUVIA_CHECK(before_handoff.session_.request(0) == nullptr);
        RUVIA_CHECK_EQ(before_handoff.session_.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(!before_handoff.session_.terminated());
        buffer::borrowed_block preserved_block;
        RUVIA_CHECK(before_handoff.outbound_.try_receive(preserved_block));
        RUVIA_CHECK(preserved_block.id().stream_id_ == filler_id.stream_id_);
        RUVIA_CHECK(preserved_block.bytes().size() == filler_bytes.size());
        RUVIA_CHECK(preserved_block.bytes().front() == filler_bytes.front());
        preserved_block.release();
        control_type preserved_control;
        RUVIA_CHECK(before_handoff.outbound_.try_receive_control(preserved_control));
        RUVIA_CHECK(preserved_control.kind_ == control_type::kind::writable);
        RUVIA_CHECK(preserved_control.id_.stream_id_ == filler_id.stream_id_);

        feed_request(before_handoff, 4, "GET", "/large");
        auto cancelled = before_handoff.make_dispatch(4, before_handoff.services_);
        RUVIA_CHECK(co_await cancelled.prepare() == dispatch_type::prepare_status_type::prepared);
        cancelled.cancel();
        RUVIA_CHECK(co_await cancelled.run_handler() == dispatch_type::run_status_type::cancelled);
        RUVIA_CHECK(cancelled.publication_demand() == publication_demand_type::local_cancelled);
        RUVIA_CHECK(cancelled.publish_step().status_ == dispatch_type::publish_status_type::cancelled);
        RUVIA_CHECK(before_handoff.session_.request(4) == nullptr);
        RUVIA_CHECK_EQ(before_handoff.session_.active_stream_count(), std::size_t{0});

        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!before_handoff.outbound_.try_receive(block));
        RUVIA_CHECK(!before_handoff.outbound_.try_receive_control(control));
    }

    {
        fixture committed(worker_handle_value, committed_memory);
        committed.routes_.handlers_.large_response_header_.assign(20 * 1024, 'h');
        feed_request(committed, 0, "GET", "/large");
        auto dispatch = committed.make_dispatch(0, committed.services_);
        RUVIA_CHECK(co_await dispatch.run_handler() == dispatch_type::run_status_type::response_ready);

        published_wire wire;
        const message_id_type id{epoch, generation, 0};
        const auto prefix = dispatch.publish_step();
        RUVIA_CHECK(prefix.status_ == dispatch_type::publish_status_type::bytes_published);
        RUVIA_CHECK_EQ(prefix.bytes_published_, buffer::max_block_bytes);
        RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), buffer::max_block_bytes);
        drain_buffer(committed.outbound_, id, wire);
        RUVIA_CHECK_EQ(wire.bytes_.size(), buffer::max_block_bytes);
        feed_peer_settings(committed, 0);
        RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);

        publish_and_drain(dispatch, committed, 0, wire, ruvia_ctx);
        RUVIA_CHECK(dispatch.complete());
        decoded_response response;
        RUVIA_CHECK(decode_published(wire, ruvia::http_known_method::get, 0,
                        committed.worker_.resource(), response)
                        .status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
        RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
        RUVIA_CHECK_EQ(response.large_response_header_bytes_, std::size_t{20 * 1024});
        RUVIA_CHECK(response.body_ == committed.routes_.handlers_.response_body_);

        feed_request(committed, 4, "GET", "/large");
        auto next_request = committed.make_dispatch(4, committed.services_);
        RUVIA_CHECK(co_await next_request.run_handler() == dispatch_type::run_status_type::peer_field_section_limit);
        RUVIA_CHECK(next_request.publication_demand() ==
                    publication_demand_type::local_peer_limit_rejected);
        const auto rejected = next_request.publish_step();
        RUVIA_CHECK(rejected.status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        RUVIA_CHECK_EQ(next_request.published_wire_bytes(), std::uint64_t{0});
        RUVIA_CHECK(committed.session_.request(4) == nullptr);
        RUVIA_CHECK_EQ(committed.session_.active_stream_count(), std::size_t{0});
        RUVIA_CHECK(!committed.session_.terminated());

        feed_request(committed, 8, "GET", "/large");
        auto shutting_down = committed.make_dispatch(8, committed.services_);
        RUVIA_CHECK(co_await shutting_down.run_handler() == dispatch_type::run_status_type::peer_field_section_limit);
        RUVIA_CHECK(committed.outbound_.stop());
        // A later buffer shutdown does not overwrite the already committed
        // encoding refusal or require a second retirement of its request.
        RUVIA_CHECK(shutting_down.publish_step().status_ == dispatch_type::publish_status_type::peer_limit_rejected);
        RUVIA_CHECK(committed.session_.request(8) == nullptr);
        RUVIA_CHECK_EQ(committed.session_.active_stream_count(), std::size_t{0});

        buffer::borrowed_block block;
        control_type control;
        RUVIA_CHECK(!committed.outbound_.try_receive(block));
        RUVIA_CHECK(!committed.outbound_.try_receive_control(control));
    }
}

ruvia::task<void> exercise_async_response(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    std::uint64_t stream_id, std::string_view path, bool cancel, ruvia::testing::test_context& ruvia_ctx) {
    feed_request(fixture_value, stream_id, "GET", path);
    tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
    auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_, callbacks.callbacks());
    {
        auto cold = dispatch.run_handler();
    }
    ruvia::worker_signal finished(worker_value);
    ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
    dispatch_type::run_status_type status{dispatch_type::run_status_type::failed};
    bool joined = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    published_wire wire;
    const message_id_type id{epoch, generation, stream_id};
    const auto deadline_value = std::chrono::steady_clock::now() + 5s;
    bool backpressured = false;
    while (!joined && std::chrono::steady_clock::now() < deadline_value) {
        const auto step = dispatch.publish_step();
        if (step.status_ == dispatch_type::publish_status_type::bytes_published) {
            const auto blocked = dispatch.publish_step();
            backpressured = backpressured || blocked.status_ == dispatch_type::publish_status_type::backpressured;
            if (cancel) {
                dispatch.cancel();
            }
        }
        drain_buffer(fixture_value.outbound_, id, wire);
        co_await ruvia::sleep_for(worker_value, 1ms);
    }
    if (!joined) {
        dispatch.cancel();
    }
    co_await tasks.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture_value.routes_.handlers_.stream_retained_stable_);
    if (cancel || fixture_value.routes_.handlers_.stream_throw_after_write_) {
        RUVIA_CHECK(status == dispatch_type::run_status_type::cancelled);
        RUVIA_CHECK(!wire.final_wire_bytes_.has_value());
    } else {
        RUVIA_CHECK(status == dispatch_type::run_status_type::output_complete);
        RUVIA_CHECK(dispatch.complete());
        decoded_response response;
        RUVIA_CHECK(decode_published(wire, ruvia::http_known_method::get, stream_id, fixture_value.worker_.resource(), response).status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK_EQ(response.status_,
            path == "/multipart-file" ? std::uint16_t{206} : std::uint16_t{200});
        if (path == "/multipart-file") {
            const auto& content = fixture_value.routes_.handlers_.response_body_;
            const auto separator = std::string("\r\n--h3_test_boundary\r\nContent-Type: text/plain\r\n");
            const std::string expected =
                "--h3_test_boundary\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-19999/65539\r\n\r\n" +
                content.substr(0, 20000) + separator +
                "Content-Range: bytes 30000-65538/65539\r\n\r\n" + content.substr(30000) +
                "\r\n--h3_test_boundary--\r\n";
            RUVIA_CHECK_EQ(response.body_, expected);
            RUVIA_CHECK_EQ(response.content_length_.value_or(0), expected.size());
            RUVIA_CHECK(response.content_type_.starts_with(
                "multipart/byteranges; boundary=h3_test_boundary"));
        }
        if (fixture_value.routes_.handlers_.send_interim_) {
            RUVIA_CHECK_EQ(response.interim_heads_, std::size_t{1});
            RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
            RUVIA_CHECK_EQ(response.early_link_, path == "/stream" ? std::string("</style.css>; rel=preload") : std::string(512, 'h'));
        }
        if (path == "/stream") {
            RUVIA_CHECK_EQ(response.body_, fixture_value.routes_.handlers_.response_body_ + fixture_value.routes_.handlers_.response_body_ + fixture_value.routes_.handlers_.response_body_ + fixture_value.routes_.handlers_.response_body_);
            RUVIA_CHECK_EQ(response.complete_trailer_, "yes");
        } else if (path != "/multipart-file") {
            RUVIA_CHECK_EQ(response.body_, fixture_value.routes_.handlers_.response_body_);
        }
        RUVIA_CHECK(backpressured);
    }
    RUVIA_CHECK(fixture_value.session_.request(stream_id) == nullptr);
}

ruvia::task<void> exercise_response_storage(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.routes_.handlers_.response_body_.assign(32768, 's');
    co_await exercise_async_response(fixture_value, worker_value, 0, "/stream", false, ruvia_ctx);
    const auto warmed = fixture_value.upstream_.live_allocations();
    co_await exercise_async_response(fixture_value, worker_value, 4, "/stream", false, ruvia_ctx);
    RUVIA_CHECK_EQ(fixture_value.upstream_.live_allocations(), warmed);
    co_await exercise_async_response(fixture_value, worker_value, 8, "/stream", true, ruvia_ctx);
    fixture_value.routes_.handlers_.stream_throw_after_write_ = true;
    co_await exercise_async_response(fixture_value, worker_value, 12, "/stream", false, ruvia_ctx);
}

}  // namespace

RUVIA_TEST(http3_buffered_dispatch_backpressures_data_and_fin_without_acknowledging_unpublished_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    fixture fixture_value(worker_handle_value, upstream, 1, 2, 1);
    run_worker_task(attachment, exercise_backpressure(fixture_value, ruvia_ctx));
}

RUVIA_TEST(http3_buffered_dispatch_treats_absent_and_omitted_peer_field_limits_as_unlimited) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_handle_value, upstream);
        run_worker_task(attachment, exercise_unlimited_peer_field_section_size(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_nonbuffered_peer_refusal_retires_stream_without_connection_failure) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource memory;
    run_worker_task(attachment, exercise_nonbuffered_peer_refusal(worker_value, memory, ruvia_ctx));
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_uses_decoded_peer_field_section_size_including_status) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource reference_memory;
    ruvia::test::counting_memory_resource exact_limit_memory;
    ruvia::test::counting_memory_resource below_limit_memory;
    run_worker_task(attachment, exercise_decoded_peer_field_section_limit(worker_handle_value,
                                    reference_memory, exact_limit_memory, below_limit_memory, ruvia_ctx));
    RUVIA_CHECK_EQ(reference_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(reference_memory.allocation_count(), reference_memory.deallocation_count());
    RUVIA_CHECK_EQ(exact_limit_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(exact_limit_memory.allocation_count(), exact_limit_memory.deallocation_count());
    RUVIA_CHECK_EQ(below_limit_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(below_limit_memory.allocation_count(), below_limit_memory.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_rechecks_limit_before_handoff_and_preserves_committed_header_prefix) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource before_handoff_memory;
    ruvia::test::counting_memory_resource committed_memory;
    run_worker_task(attachment, exercise_late_peer_field_section_limit(worker_handle_value,
                                    before_handoff_memory, committed_memory, ruvia_ctx));
    RUVIA_CHECK_EQ(before_handoff_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(before_handoff_memory.allocation_count(),
        before_handoff_memory.deallocation_count());
    RUVIA_CHECK_EQ(committed_memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(committed_memory.allocation_count(), committed_memory.deallocation_count());
}

RUVIA_TEST(http3_response_stream_publishes_bounded_data_trailers_and_reclaims_operations) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment, exercise_response_storage(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

ruvia::task<void> exercise_trailer_peer_refusal(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.routes_.handlers_.response_trailer_.assign(2048, 't');
    feed_peer_settings(fixture_value, 512);
    feed_request(fixture_value, 0, "GET", "/stream");
    tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_, callbacks.callbacks());
    ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
    ruvia::worker_signal finished(worker_value);
    dispatch_type::run_status_type status{dispatch_type::run_status_type::failed};
    bool joined = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    published_wire wire;
    const message_id_type id{epoch, generation, 0};
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    while (!joined && std::chrono::steady_clock::now() < deadline_value) {
        (void)dispatch.publish_step();
        drain_buffer(fixture_value.outbound_, id, wire);
        co_await ruvia::sleep_for(worker_value, 1ms);
    }
    if (!joined) {
        dispatch.cancel();
    }
    co_await tasks.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(status == dispatch_type::run_status_type::peer_field_section_limit);
    RUVIA_CHECK(!wire.bytes_.empty());
    RUVIA_CHECK(!wire.final_wire_bytes_);
    RUVIA_CHECK(dispatch.publish_step().status_ == dispatch_type::publish_status_type::peer_limit_rejected);
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
    RUVIA_CHECK_EQ(fixture_value.session_.active_stream_count(), std::size_t{0});
    RUVIA_CHECK(!fixture_value.session_.terminated());
}

RUVIA_TEST(http3_oversized_trailers_reject_only_the_partial_response_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource memory;
    {
        fixture fixture(worker_value, memory);
        run_worker_task(attachment, exercise_trailer_peer_refusal(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(http3_multipart_file_response_publishes_exact_body_length_and_fin_under_backpressure) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("ruvia-h3-multipart-" + std::to_string(
                                                   std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream output(path, std::ios::binary);
        output << std::string(65539, '2');
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 8});
    {
        fixture fixture(worker_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        fixture.options_.blocking_pool_ = &pool;
        fixture.routes_.handlers_.file_path_ = path;
        fixture.routes_.handlers_.response_body_.assign(65539, '2');
        auto exercise = [&]() -> ruvia::task<void> {
            co_await exercise_async_response(fixture, worker_value, 0, "/multipart-file", false, ruvia_ctx);
            co_await exercise_async_response(fixture, worker_value, 4, "/multipart-file", true, ruvia_ctx);
        };
        run_worker_task(attachment, exercise());
    }
    pool.stop();
    pool.join();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_response_file_reads_bounded_blocks_off_worker_and_publishes_fin) {
    const auto path = std::filesystem::temp_directory_path() / ("ruvia-h3-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string content(65539, 'f');
    {
        std::ofstream output(path, std::ios::binary);
        output << content;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 8});
    {
        fixture fixture(worker_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        fixture.options_.blocking_pool_ = &pool;
        fixture.routes_.handlers_.file_path_ = path;
        fixture.routes_.handlers_.response_body_ = content;
        auto exercise = [&]() -> ruvia::task<void> {
            co_await exercise_async_response(fixture, worker_value, 0, "/file", false, ruvia_ctx);
            feed_peer_settings(fixture, 0);
            feed_request(fixture, 4, "GET", "/file");
            auto refused = fixture.make_dispatch(4, fixture.services_);
            RUVIA_CHECK(co_await refused.run_handler() == dispatch_type::run_status_type::peer_field_section_limit);
            RUVIA_CHECK_EQ(refused.published_wire_bytes(), std::uint64_t{0});
            RUVIA_CHECK(fixture.session_.request(4) == nullptr);
            RUVIA_CHECK_EQ(fixture.session_.active_stream_count(), std::size_t{0});
            RUVIA_CHECK(!fixture.session_.terminated());
        };
        run_worker_task(attachment, exercise());
    }
    pool.stop();
    pool.join();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

ruvia::task<void> exercise_streaming_upload(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    bool cancel, ruvia::testing::test_context& ruvia_ctx) {
    const std::array initial_fields{ruvia::http3_field_section_field_view{"x-checksum", "initial"}};
    const auto head = request_wire(fixture_value.worker_, "POST", "/upload", {}, initial_fields);
    RUVIA_CHECK(fixture_value.session_.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(fixture_value.session_.streaming_request(0));
    RUVIA_CHECK(fixture_value.session_.stream_state(0) == engine_type::stream_state_type::ready);
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_);
    {
        auto cold = dispatch.run_handler();
    }
    ruvia::worker_signal received(worker_value);
    fixture_value.routes_.handlers_.started_ = &received;
    ruvia::worker_signal finished(worker_value);
    ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
    dispatch_type::run_status_type status{dispatch_type::run_status_type::failed};
    bool joined = false;
    tasks.spawn(run_owner(dispatch, status, joined, finished));
    const std::string body(65536, 'u');
    const auto data = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), body);
    const auto retained = fixture_value.upstream_.live_allocations();
    for (unsigned n = 0; n < 20 && !joined; ++n) {
        RUVIA_CHECK(fixture_value.session_.can_accept_input(0, body.size()));
        RUVIA_CHECK(fixture_value.session_.feed(0, data).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(!fixture_value.session_.can_accept_input(0, 1));
        dispatch.notify_tunnel_input();
        co_await received.wait();
        co_await ruvia::sleep_for(worker_value, 1ms);
        RUVIA_CHECK(fixture_value.session_.can_accept_input(0, body.size()));
    }
    if (cancel) {
        dispatch.cancel();
        (void)fixture_value.session_.cancel_request(0);
    } else {
        const std::array fields_value{ruvia::http3_field_section_field_view{"x-checksum", "final"}};
        const auto section = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
        RUVIA_CHECK((section.index() == 0));
        if ((section.index() == 0)) {
            const auto trailers = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers), std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
            RUVIA_CHECK(fixture_value.session_.feed(0, trailers).scope_ == ruvia::http3_connection_error_scope::none);
        }
        RUVIA_CHECK(fixture_value.session_.feed(0, {}, true).status_ == ruvia::http3_connection_status::message_end);
        dispatch.notify_tunnel_input();
    }
    co_await finished.wait();
    co_await tasks.join();
    if (cancel) {
        RUVIA_CHECK(status == dispatch_type::run_status_type::cancelled);
    } else {
        RUVIA_CHECK(status == dispatch_type::run_status_type::response_ready);
        RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.upload_bytes_, body.size() * 20);
        RUVIA_CHECK(fixture_value.routes_.handlers_.upload_chunks_ >= 80);
        RUVIA_CHECK(fixture_value.routes_.handlers_.upload_trailer_observed_);
        published_wire wire;
        publish_and_drain(dispatch, fixture_value, 0, wire, ruvia_ctx);
    }
    RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});
    RUVIA_CHECK(fixture_value.upstream_.live_allocations() <= retained + 16);
}

RUVIA_TEST(http3_streaming_upload_dispatches_before_fin_bounds_backlog_and_cancels_pending_read) {
    for (bool cancel : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
        const auto worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource upstream;
        {
            fixture fixture(worker_value, upstream);
            fixture.executor_ = attachment.loop().executor();
            run_worker_task(attachment, exercise_streaming_upload(fixture, worker_value, cancel, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    }
}

RUVIA_TEST(http3_interim_response_precedes_buffered_and_stream_final_and_owns_fields) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        fixture.routes_.handlers_.send_interim_ = true;
        fixture.routes_.handlers_.response_body_.assign(32768, 's');
        auto exercise = [&]() -> ruvia::task<void> {
            co_await exercise_async_response(fixture, worker_value, 0, "/large", false, ruvia_ctx);
            co_await exercise_async_response(fixture, worker_value, 4, "/stream", false, ruvia_ctx);
            RUVIA_CHECK(fixture.routes_.handlers_.interim_after_final_rejected_);
        };
        run_worker_task(attachment, exercise());
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}
