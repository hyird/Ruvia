#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::task<void> signal_probe(ruvia::worker_signal& signal, bool& awoke) {
    co_await signal.wait();
    awoke = true;
}

ruvia::task<bool> wait_for_local_work(connection_type& connection,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        const auto state_value = connection.work_state();
        if (state_value.wrong_worker_) {
            co_return false;
        }
        if (state_value.runnable_.local_) {
            co_return true;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    co_return connection.work_state().runnable_.local_;
}

ruvia::task<void> exercise_lane_queue_isolation(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    ruvia::task_scope probes(worker_value, {.resource_ = fixture_value.worker_.resource()});
    buffer inbound(8, 8, 4, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 70, .max_tracked_streams_ = 8});

    const message_id_type filler_id{epoch, base_generation + 70, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {control_type::kind::writable, filler_id, 0})));
    const auto head = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 70, 4}, "HEAD", "/file");
    RUVIA_CHECK(head.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    const auto head_headers = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(head_headers.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(head_headers.stream_id_, std::uint64_t{4});
    RUVIA_CHECK(head_headers.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    const auto blocked_control = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(blocked_control.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(blocked_control.stream_id_, std::uint64_t{4});
    RUVIA_CHECK(blocked_control.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_control.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::control);

    const auto data = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 70, 0}, "GET", "/first");
    RUVIA_CHECK(data.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    const auto blocked_data = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(blocked_data.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(blocked_data.stream_id_, std::uint64_t{0});
    RUVIA_CHECK(blocked_data.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_data.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::data);

    auto state_value = connection.work_state();
    RUVIA_CHECK(!state_value.wrong_worker_);
    RUVIA_CHECK(!state_value.runnable_.data_ && !state_value.runnable_.control_ && !state_value.runnable_.local_);
    RUVIA_CHECK(state_value.blocked_.data_ && state_value.blocked_.control_);
    RUVIA_CHECK_EQ(state_value.blocked_count_, std::size_t{2});
    RUVIA_CHECK(connection.publish_one(all_work_lanes).status_ ==
                connection_type::publish_status_type::no_ready_request);
    RUVIA_CHECK(connection.publish_one(all_work_lanes).status_ ==
                connection_type::publish_status_type::no_ready_request);

    // Consume the last work-activation notification. Parking and idle probes
    // must not manufacture repeated wakeups.
    co_await scheduler.wait();
    bool scheduler_awoke_again = false;
    probes.spawn(signal_probe(scheduler.signal_, scheduler_awoke_again));
    const auto slept = co_await ruvia::sleep_for(worker_value, 5ms, fixture_value.worker_stop_);
    require_watchdog_success(ruvia_ctx, slept == ruvia::timer_sleep_result::elapsed);
    RUVIA_CHECK(!scheduler_awoke_again);

    std::array<published_wire, 6> drained_wires{};
    drain_data_only(outbound, drained_wires);
    const auto reactivated = connection.reactivate_blocked({.data_ = true});
    RUVIA_CHECK_EQ(reactivated, std::size_t{1});
    state_value = connection.work_state();
    RUVIA_CHECK(state_value.runnable_.data_ && state_value.blocked_.control_ && !state_value.blocked_.data_);
    const auto retried_data = connection.publish_one({.data_ = true});
    RUVIA_CHECK(retried_data.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(retried_data.stream_id_, std::uint64_t{0});
    RUVIA_CHECK(retried_data.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    const auto blocked_again = connection.publish_one({.data_ = true});
    RUVIA_CHECK(blocked_again.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(blocked_again.stream_id_, std::uint64_t{0});
    RUVIA_CHECK(blocked_again.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked_again.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::data);
    RUVIA_CHECK(connection.publish_one({.control_ = true}).status_ ==
                connection_type::publish_status_type::no_ready_request);
    state_value = connection.work_state();
    RUVIA_CHECK(state_value.blocked_.data_ && state_value.blocked_.control_);
    RUVIA_CHECK(!state_value.runnable_.control_ && !state_value.runnable_.local_);
    RUVIA_CHECK(!scheduler_awoke_again);

    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.work_state().blocked_count_, std::size_t{0});
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);
    RUVIA_CHECK_EQ(connection.request_info(4).status_, connection_type::request_status_type::cancelled);
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    RUVIA_CHECK(scheduler_awoke_again);
    co_await probes.join();
    co_await connection.join();
    std::array<published_wire, 6> remaining{};
    drain_all(outbound, remaining);
    simulate_global_stop_takeover(connection);
}

ruvia::task<void> exercise_publication_deadline_for_lane(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, bool control_lane, std::uint64_t connection_generation,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 80ms};
    fixture_value.routes_.handlers_.handler_calls_ = 0;
    if (!control_lane) {
        fixture_value.routes_.handlers_.large_response_header_.assign(20 * 1024, 'd');
    }

    test_activation_signal scheduler(worker_value);
    buffer inbound(8, 8, 4, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    const message_id_type id{epoch, connection_generation, 0};
    if (control_lane) {
        RUVIA_CHECK(accepted(outbound.try_send_control(
            {control_type::kind::writable, {epoch, connection_generation, 12}, 0})));
    }

    const auto method = control_lane ? "HEAD" : "GET";
    const auto path = control_lane ? "/file" : "/first";
    const auto request = route_request(connection, inbound, fixture_value.worker_, id, method, path);
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);

    if (control_lane) {
        const auto headers = connection.publish_one(all_work_lanes);
        RUVIA_CHECK(headers.status_ == connection_type::publish_status_type::attempted);
        RUVIA_CHECK(headers.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::bytes_published);
        std::array<published_wire, 6> discarded{};
        drain_data_only(outbound, discarded);
    } else {
        const auto prefix = connection.publish_one(all_work_lanes);
        RUVIA_CHECK(prefix.status_ == connection_type::publish_status_type::attempted);
        RUVIA_CHECK(prefix.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::bytes_published);
        RUVIA_CHECK(prefix.publication_.bytes_published_ > 0);
    }

    const auto blocked = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(blocked.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK(blocked.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked.publication_.block_reason_ ==
                (control_lane ? connection_type::dispatch_type::publish_block_reason_type::control
                              : connection_type::dispatch_type::publish_block_reason_type::data));
    auto state_value = connection.work_state();
    RUVIA_CHECK(state_value.blocked_.contains(control_lane ? connection_type::work_lane_type::control
                                                           : connection_type::work_lane_type::data));
    RUVIA_CHECK_EQ(state_value.blocked_count_, std::size_t{1});

    require_watchdog_success(ruvia_ctx,
        co_await wait_for_local_work(connection, worker_value, fixture_value.worker_stop_));
    state_value = connection.work_state();
    RUVIA_CHECK(state_value.runnable_.local_);
    RUVIA_CHECK(!state_value.blocked_.data_ && !state_value.blocked_.control_);
    const auto expired = connection.publish_one({.local_ = true});
    RUVIA_CHECK(expired.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK_EQ(expired.stream_id_, id.stream_id_);
    RUVIA_CHECK(expired.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::cancelled);
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));

    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(!connection.transport_close_required());
    const auto reset = connection.peek_transport_intent();
    RUVIA_CHECK(reset.has_value());
    RUVIA_CHECK(reset->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK_EQ(reset->token_.id_.epoch_, id.epoch_);
    RUVIA_CHECK_EQ(reset->token_.id_.connection_generation_, id.connection_generation_);
    RUVIA_CHECK_EQ(reset->token_.id_.stream_id_, id.stream_id_);
    RUVIA_CHECK(reset->stream_reset_error_code_ ==
                ruvia::http3_connection_error_code::request_cancelled);

    const auto final_size = request_wire(fixture_value.worker_, method, path).size();
    const auto late_fin = connection.accept_control(
        {control_type::kind::stream_fin, id, static_cast<std::uint64_t>(final_size)});
    RUVIA_CHECK(late_fin.input_.status_ == connection_type::input_type::status_type::closed_stream);
    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});

    std::array<published_wire, 6> wires{};
    drain_all(outbound, wires);
    if (!control_lane) {
        RUVIA_CHECK(!wires[0].bytes_.empty());
        RUVIA_CHECK(!wires[0].final_wire_bytes_.has_value());
    }

    const auto sibling = route_request(connection, inbound, fixture_value.worker_,
        {epoch, connection_generation, 4}, "GET", "/first");
    RUVIA_CHECK(sibling.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    bool sibling_published = false;
    for (std::size_t attempt_value = 0; attempt_value < 32 && !sibling_published; ++attempt_value) {
        const auto publication = connection.publish_one(all_work_lanes);
        if (publication.status_ == connection_type::publish_status_type::no_ready_request) {
            break;
        }
        RUVIA_CHECK(publication.status_ == connection_type::publish_status_type::attempted);
        switch (publication.publication_.status_) {
            case connection_type::dispatch_type::publish_status_type::bytes_published:
                drain_data_only(outbound, wires);
                (void)connection.reactivate_blocked({.data_ = true});
                break;
            case connection_type::dispatch_type::publish_status_type::fin_published:
            case connection_type::dispatch_type::publish_status_type::complete:
                drain_all(outbound, wires);
                (void)connection.reactivate_blocked({.control_ = true});
                sibling_published = true;
                break;
            case connection_type::dispatch_type::publish_status_type::backpressured:
                if (publication.publication_.block_reason_ ==
                    connection_type::dispatch_type::publish_block_reason_type::data) {
                    drain_data_only(outbound, wires);
                    (void)connection.reactivate_blocked({.data_ = true});
                } else {
                    drain_all(outbound, wires);
                    (void)connection.reactivate_blocked({.control_ = true});
                }
                break;
            default:
                RUVIA_CHECK(false);
                break;
        }
    }
    RUVIA_CHECK(sibling_published);
    RUVIA_CHECK_EQ(connection.request_info(4).status_, connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{2});
    RUVIA_CHECK(!connection.transport_close_required());

    RUVIA_CHECK(connection.request_stop());
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    co_await connection.join();
    simulate_global_stop_takeover(connection);
}

ruvia::task<void> exercise_handler_deadline_cancellation(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 80ms};
    fixture_value.routes_.handlers_.handler_calls_ = 0;
    test_activation_signal scheduler(worker_value);
    ruvia::worker_signal handler_started(worker_value);
    fixture_value.routes_.handlers_.slow_started_ = &handler_started;
    buffer inbound(4, 4, 2, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 83;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    const message_id_type id{epoch, connection_generation, 0};
    const auto request = route_request(connection, inbound, fixture_value.worker_, id, "GET", "/slow");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_slow_start(fixture_value, worker_value, fixture_value.worker_stop_));
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));

    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);
    RUVIA_CHECK(connection.request_info(0).run_status_ == connection_type::dispatch_type::run_status_type::cancelled);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(!connection.transport_close_required());
    const auto reset = connection.peek_transport_intent();
    RUVIA_CHECK(reset.has_value());
    RUVIA_CHECK(reset->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK_EQ(reset->token_.id_.stream_id_, std::uint64_t{0});
    RUVIA_CHECK(reset->stream_reset_error_code_ ==
                ruvia::http3_connection_error_code::request_cancelled);
    const auto late_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(request_wire(fixture_value.worker_, "GET", "/slow").size())});
    RUVIA_CHECK(late_fin.input_.status_ == connection_type::input_type::status_type::closed_stream);
    RUVIA_CHECK(fixture_value.routes_.handlers_.slow_observed_stop_);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    RUVIA_CHECK(!connection.transport_close_required());

    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
}

ruvia::task<void> exercise_deadline_activation_and_handler_cancellation(
    fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await exercise_publication_deadline_for_lane(
        fixture_value, worker_value, false, base_generation + 81, ruvia_ctx);
    co_await exercise_publication_deadline_for_lane(
        fixture_value, worker_value, true, base_generation + 82, ruvia_ctx);
    co_await exercise_handler_deadline_cancellation(fixture_value, worker_value, ruvia_ctx);
}

ruvia::task<void> exercise_rejection_backpressure(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 92;
    const message_id_type id{epoch, connection_generation, 0};
    const message_id_type filler{epoch, connection_generation, 12};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto wire = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    constexpr std::array<std::byte, 1> filler_bytes{std::byte{'x'}};
    RUVIA_CHECK(accepted(outbound.try_send(filler, filler_bytes)));
    const auto rejected = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});

    const auto data_blocked = connection.publish_one({.data_ = true});
    RUVIA_CHECK(data_blocked.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(data_blocked.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::data);
    RUVIA_CHECK(connection.work_state().blocked_.data_);
    std::array<published_wire, 6> wires{};
    drain_data_only(outbound, wires);
    RUVIA_CHECK_EQ(connection.reactivate_blocked({.data_ = true}), std::size_t{1});
    const auto headers = connection.publish_one({.data_ = true});
    RUVIA_CHECK(headers.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(accepted(outbound.try_send_control({control_type::kind::writable, filler, 0})));
    const auto control_blocked = connection.publish_one({.control_ = true});
    RUVIA_CHECK(control_blocked.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(control_blocked.publication_.block_reason_ ==
                connection_type::dispatch_type::publish_block_reason_type::control);
    RUVIA_CHECK(connection.work_state().blocked_.control_);
    drain_all(outbound, wires);
    RUVIA_CHECK_EQ(connection.reactivate_blocked({.control_ = true}), std::size_t{1});
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    drain_all(outbound, wires);
    const auto response = decode_response(wires[0], ruvia::http_known_method::get,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{417});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    const auto input_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(input_fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_dynamic_qpack_publication(fixture& fixture_value, const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 103;
    const message_id_type request_id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_, fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .session_ = {.connection_ = {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2, .enable_connect_protocol_ = true}}, .max_tracked_streams_ = 8});
    const auto prefixes = ruvia::http3_local_critical_streams::create({.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2});
    RUVIA_CHECK((prefixes.index() == 0));
    const auto settings = std::get<0>(prefixes).control_prefix();
    RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, connection_generation, 2}, settings).status_ == connection_type::event_status_type::accepted);
    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, fixture_value.worker_.resource());
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "POST"}, ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"}, ruvia::http3_field_section_field_view{":path", "/body"},
        ruvia::http3_field_section_field_view{"content-length", "7"}, ruvia::http3_field_section_field_view{"x-custom", "value"}};
    const auto section = encoder.encode(0, fields_value);
    RUVIA_CHECK((section.index() == 0));
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers), std::string_view(std::get<0>(section).data(), std::get<0>(section).size())) +
                      frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), "payload");
    const auto head = accept_wire_bytes(connection, inbound, request_id, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(head.input_.status_ == connection_type::input_type::status_type::deferred_qpack);
    const auto fin = connection.accept_control({control_type::kind::stream_fin, request_id, wire.size()});
    RUVIA_CHECK(fin.input_.status_ == connection_type::input_type::status_type::deferred_qpack);
    RUVIA_CHECK(!connection.can_accept_input(0, 1));
    RUVIA_CHECK(!connection.resume_qpack_input());
    auto instructions = std::string(1, char(2));
    const auto pending = encoder.pending_encoder_output();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, connection_generation, 6}, std::span(instructions.data(), instructions.size())).status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK(connection.resume_qpack_input());
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.body_seen_, std::string("payload"));
    std::string encoder_wire, decoder_wire, response_wire;
    bool response_fin = false;
    bool saw_blocked = false;
    for (std::size_t i = 0; i < 1000 && (!response_fin || connection.work_state().runnable_count_ || connection.work_state().blocked_count_); ++i) {
        const auto attempt_value = connection.publish_one(all_work_lanes);
        if (attempt_value.status_ == connection_type::publish_status_type::attempted && attempt_value.publication_.status_ == connection_type::dispatch_type::publish_status_type::backpressured) {
            saw_blocked = true;
        }
        // Occasionally keep the sole block borrowed across a publication attempt.
        buffer::borrowed_block block;
        if (outbound.try_receive(block)) {
            if (const auto* critical = block.critical()) {
                auto& destination = critical->kind_ == ruvia::http3_critical_stream_output::stream_kind::qpack_encoder ? encoder_wire : decoder_wire;
                destination.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            } else {
                response_wire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            }
            if (i == 0) {
                const auto blocked = connection.publish_one(all_work_lanes);
                saw_blocked |= blocked.publication_.status_ == connection_type::dispatch_type::publish_status_type::backpressured;
            }
            block.release();
        }
        control_type control;
        while (outbound.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::stream_fin) {
                response_fin = true;
                RUVIA_CHECK_EQ(control.value_, response_wire.size());
            }
        }
        (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
    }
    RUVIA_CHECK(saw_blocked);
    RUVIA_CHECK(response_fin);
    RUVIA_CHECK(!encoder_wire.empty());
    RUVIA_CHECK(!decoder_wire.empty());
    ruvia::http3_connection peer(ruvia::http3_peer_role::client, fixture_value.worker_.resource(), {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2});
    RUVIA_CHECK(peer.register_client_request(0, ruvia::http_known_method::post).scope_ == ruvia::http3_connection_error_scope::none);
    struct response_observation {
        std::string body_;
        std::uint16_t status_{};
    } received;
    const auto receive = +[](void* context_value, const ruvia::http3_connection_event& event) {
        auto& result_value = *static_cast<response_observation*>(context_value);
        if (event.kind_ == ruvia::http3_connection_event_kind::final_head) {
            result_value.status_ = event.head_->status_;
        }
        if (event.kind_ == ruvia::http3_connection_event_kind::body) {
            result_value.body_.append(event.body_.data(), event.body_.size());
        }
    };
    auto result_value = peer.feed(0, std::span(response_wire.data(), response_wire.size()), true, false, receive, &received);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::qpack_blocked);
    const auto consumed = result_value.consumed_bytes_;
    encoder_wire.insert(0, 1, char(2));
    RUVIA_CHECK(peer.feed(7, std::span(encoder_wire.data(), encoder_wire.size()), false, false, receive, &received).scope_ == ruvia::http3_connection_error_scope::none);
    result_value = peer.feed(0, std::span(response_wire.data() + consumed, response_wire.size() - consumed), true, false, receive, &received);
    RUVIA_CHECK(result_value.scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK_EQ(received.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(received.body_, std::string("payload"));
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

ruvia::task<void> exercise_peer_input_with_held_request_credit(
    fixture& fixture_value, const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal activation(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(8, 8, 8, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 104;
    const message_id_type request_id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, activation,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .session_ = {.connection_ = {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2, .enable_connect_protocol_ = true}}, .max_tracked_streams_ = 8});
    const auto prefixes = ruvia::http3_local_critical_streams::create(
        {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2});
    RUVIA_CHECK((prefixes.index() == 0));
    RUVIA_CHECK(connection.accept_peer_stream_data({epoch, connection_generation, 2},
                              std::as_bytes(std::get<0>(prefixes).control_prefix()))
                    .status_ == connection_type::event_status_type::accepted);
    ruvia::http3_qpack_encoder encoder(
        {.max_table_capacity_ = 256, .max_blocked_streams_ = 2}, fixture_value.worker_.resource());
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "POST"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/body"},
        ruvia::http3_field_section_field_view{"content-length", "7"},
        ruvia::http3_field_section_field_view{"x-custom", "value"}};
    const auto section = encoder.encode(0, fields_value);
    RUVIA_CHECK((section.index() == 0));
    const auto head_wire = frame(1, {std::get<0>(section).data(), std::get<0>(section).size()});
    RUVIA_CHECK(accept_wire_bytes(connection, inbound, request_id,
                    std::span(head_wire.data(), head_wire.size()))
                    .input_.status_ == connection_type::input_type::status_type::deferred_qpack);
    const auto body_wire = frame(0, "payload");
    RUVIA_CHECK(inbound.try_send(request_id,
                    std::as_bytes(std::span(body_wire.data(), body_wire.size()))) == buffer::send_result::sent);
    buffer::borrowed_block held_body;
    RUVIA_CHECK(inbound.try_receive(held_body));
    RUVIA_CHECK(!connection.can_accept_input(0, held_body.bytes().size()));
    RUVIA_CHECK(connection.accept_control({control_type::kind::stream_fin, request_id,
                                              head_wire.size() + body_wire.size()})
                    .input_.status_ == connection_type::input_type::status_type::deferred_fin);
    auto instructions = std::string(1, char(2));
    const auto pending = encoder.pending_encoder_output();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(instructions.size() > 2);
    const auto held_bytes = std::string(reinterpret_cast<const char*>(held_body.bytes().data()), held_body.bytes().size());
    for (const char byte : instructions) {
        buffer::data_reservation unavailable;
        RUVIA_CHECK(inbound.reserve_data({epoch, connection_generation, 6}, unavailable) == buffer::reservation_result::no_block);
        RUVIA_CHECK(connection.accept_peer_stream_data({epoch, connection_generation, 6},
                                  std::as_bytes(std::span(&byte, 1)))
                        .status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK_EQ(std::string(reinterpret_cast<const char*>(held_body.bytes().data()), held_body.bytes().size()), held_bytes);
    }
    RUVIA_CHECK(connection.resume_qpack_input());
    RUVIA_CHECK(connection.can_accept_input(0, held_body.bytes().size()));
    const auto body = connection.accept_data(held_body);
    RUVIA_CHECK(body.input_.status_ == connection_type::input_type::status_type::finished);
    held_body.release();
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.body_seen_, std::string("payload"));
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

ruvia::task<void> exercise_origin_publication(fixture& fixture_value, const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 104;
    fixture_value.services_ = fixture_value.services_.with_tls_transport("127.0.0.1", {});
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_, fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    RUVIA_CHECK(route_request(connection, inbound, fixture_value.worker_, {epoch, connection_generation, 0}, "GET", "/advertise").status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    std::string control_wire, response_wire;
    bool response_fin = false;
    bool saw_blocked = false;
    for (unsigned round = 0; round != 1000 && (!response_fin || connection.work_state().runnable_count_ || connection.work_state().blocked_count_); ++round) {
        const auto attempt_value = connection.publish_one(all_work_lanes);
        saw_blocked |= attempt_value.publication_.status_ == connection_type::dispatch_type::publish_status_type::backpressured;
        buffer::borrowed_block block;
        if (outbound.try_receive(block)) {
            if (const auto* critical = block.critical()) {
                RUVIA_CHECK(critical->epoch_ == epoch && critical->connection_generation_ == connection_generation);
                RUVIA_CHECK(critical->kind_ == ruvia::http3_critical_stream_output::stream_kind::control);
                control_wire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            } else {
                response_wire.append(reinterpret_cast<const char*>(block.bytes().data()), block.bytes().size());
            }
            const auto blocked = connection.publish_one(all_work_lanes);
            saw_blocked |= blocked.publication_.status_ == connection_type::dispatch_type::publish_status_type::backpressured;
            block.release();
        }
        control_type control;
        while (outbound.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::stream_fin) {
                response_fin = true;
                RUVIA_CHECK_EQ(control.value_, response_wire.size());
            }
        }
        (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
    }
    RUVIA_CHECK(saw_blocked && response_fin && control_wire.size() > buffer::max_block_bytes);
    ruvia::http3_connection peer(ruvia::http3_peer_role::client, fixture_value.worker_.resource(), {.receive_origin_advertisements_ = true});
    const auto prefixes = ruvia::http3_local_critical_streams::create({});
    const auto prefix = std::get<0>(prefixes).control_prefix();
    control_wire.insert(0, prefix.data(), prefix.size());
    std::size_t received_origins{};
    const auto receive = [](void* raw, const ruvia::http3_connection_event& event) {
        if (event.origin_advertisement_) {
            *static_cast<std::size_t*>(raw) += event.origin_advertisement_->origins_.size();
        }
    };
    RUVIA_CHECK(peer.feed(3, std::span<const char>(control_wire), false, false, receive, &received_origins).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK_EQ(received_origins, std::size_t{40});
    RUVIA_CHECK(peer.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(peer.feed(0, std::span<const char>(response_wire), true, false, receive, &received_origins).status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}

}  // namespace

RUVIA_TEST(http3_server_connection_parks_rejection_on_exact_buffer_lane) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_rejection_backpressure(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_keeps_data_and_control_backpressure_in_separate_lanes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_lane_queue_isolation(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_activates_blocked_publication_on_deadline) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_deadline_activation_and_handler_cancellation(
                                        fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_advertises_origins_on_control_stream_under_buffer_backpressure) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_origin_publication(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_resumes_dynamic_qpack_and_publishes_both_critical_streams_with_backpressure) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_dynamic_qpack_publication(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_server_connection_peer_encoder_fragments_progress_with_all_request_credits_borrowed) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_peer_input_with_held_request_credit(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}

namespace {
ruvia::task<void> exercise_continue_before_body(fixture& fixture_value, const ruvia::worker_handle& worker_value, unsigned mode, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(2, 2, 2, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    const auto connection_generation = base_generation + 120 + mode;
    const message_id_type id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_, fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .session_ = {.max_buffered_body_bytes_ = mode == 2 ? 3u : 16u}, .max_tracked_streams_ = 8});
    const std::array fields_value{ruvia::http3_field_section_field_view{"expect", "100-continue"}};
    const auto head = ruvia::encode_http3_client_request_head({.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .path_ = "/body", .fields_ = fields_value, .body_length_ = mode == 2 ? std::nullopt : std::optional<std::uint64_t>{mode == 3 ? 0u : 7u}}, {}, fixture_value.worker_.resource());
    RUVIA_CHECK((head.index() == 0));
    const auto wire = frame(1, {std::get<0>(head).field_section_.data(), std::get<0>(head).field_section_.size()});
    const auto received_value = accept_wire_bytes(connection, inbound, id, std::span(wire.data(), wire.size()));
    RUVIA_CHECK(received_value.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    std::array<published_wire, 6> wires{};
    if (mode == 1) {
        RUVIA_CHECK(connection.work_state().runnable_.data_);
        RUVIA_CHECK(connection.accept_control({control_type::kind::stream_reset, id, wire.size()}).status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK_EQ(connection.publish_one(all_work_lanes).status_, connection_type::publish_status_type::no_ready_request);
    } else {
        if (mode != 3) {
            const auto initial_value = connection.publish_one(all_work_lanes);
            RUVIA_CHECK(initial_value.publication_.status_ == connection_type::dispatch_type::publish_status_type::bytes_published);
            drain_all(outbound, wires);
            RUVIA_CHECK(!wires[0].final_wire_bytes_);
            ruvia::http3_client_response decoder(0, ruvia::http_known_method::post, fixture_value.worker_.resource());
            decoded_response result;
            RUVIA_CHECK(decoder.feed(std::span(wires[0].bytes_.data(), wires[0].bytes_.size()), false, false, capture_response, &result).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK_EQ(result.informational_heads_, std::size_t{1});
            RUVIA_CHECK_EQ(result.final_heads_, std::size_t{0});
        } else {
            RUVIA_CHECK_EQ(connection.publish_one(all_work_lanes).status_, connection_type::publish_status_type::no_ready_request);
        }
        const auto body = mode == 3 ? std::string{} : frame(0, "payload");
        if (!body.empty()) {
            (void)accept_wire_bytes(connection, inbound, id, std::span(body.data(), body.size()));
        }
        const auto finished = connection.accept_control({control_type::kind::stream_fin, id, wire.size() + body.size()});
        RUVIA_CHECK(finished.status_ == (mode == 2 ? connection_type::event_status_type::rejected : connection_type::event_status_type::dispatched));
        bool saw_fin = false;
        for (std::size_t i = 0; i < 2000 && !saw_fin; ++i) {
            const auto attempt_value = connection.publish_one(all_work_lanes);
            drain_all(outbound, wires);
            (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
            saw_fin = wires[0].final_wire_bytes_.has_value();
            if (attempt_value.status_ == connection_type::publish_status_type::no_ready_request) {
                co_await ruvia::sleep_for(worker_value, 1ms);
            }
        }
        RUVIA_CHECK(saw_fin);
        const auto response = decode_response(wires[0], ruvia::http_known_method::post, 0, fixture_value.worker_.resource());
        RUVIA_CHECK_EQ(response.status_, std::uint16_t(mode == 2 ? 413 : 200));
        RUVIA_CHECK_EQ(response.informational_heads_, std::size_t(mode == 3 ? 0 : 1));
        RUVIA_CHECK_EQ(response.body_, std::string(mode == 0 ? "payload" : ""));
    }
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
}
}  // namespace

RUVIA_TEST(http3_server_connection_continues_upload_before_body_and_keeps_fin_count_across_rejection_and_reset) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
        const auto worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource upstream;
        {
            fixture fixture(worker_value, upstream);
            run_worker_task(attachment, exercise_continue_before_body(fixture, worker_value, mode, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    }
}

namespace {
ruvia::task<void> exercise_push_publication(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::test::counting_memory_resource& upstream, ruvia::testing::test_context& ruvia_ctx) {
    ruvia::worker_signal slow_started(worker_value);
    fixture_value.routes_.handlers_.slow_started_ = &slow_started;
    std::size_t warmed_live_allocations{};
    std::string retained_promise_path;
    std::string retained_body;
    for (int iteration = 0; iteration != 139; ++iteration) {
        const int mode = iteration < 128 ? 0 : iteration - 128;
        fixture_value.routes_.handlers_.push_mode_ = mode;
        fixture_value.routes_.handlers_.push_completed_ = false;
        fixture_value.routes_.handlers_.push_accepted_ = false;
        fixture_value.routes_.handlers_.slow_started_observed_ = false;
        fixture_value.routes_.handlers_.slow_observed_stop_ = false;
        fixture_value.routes_.handlers_.pushed_cookie_.clear();
        fixture_value.routes_.handlers_.pushed_header_.clear();
        test_activation_signal activation(worker_value);
        buffer inbound(2, 2, 2, fixture_value.worker_.resource());
        buffer outbound(1, 1, 1, fixture_value.worker_.resource());
        connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, activation,
            {.epoch_ = epoch, .connection_generation_ = base_generation, .max_tracked_streams_ = 32});
        std::unordered_map<std::uint64_t, published_wire> wires;
        bool open_observed = false;
        bool cancelled_active_push = false;
        std::exception_ptr failure;
        try {
            if (mode != 1) {
                constexpr std::array<char, 6> settings_and_max{0, 4, 0, 0xd, 1, 0};
                RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, base_generation, 2}, settings_and_max).status_ == connection_type::event_status_type::accepted);
            }
            RUVIA_CHECK(route_request(connection, inbound, fixture_value.worker_, {epoch, base_generation, 0}, "GET", "/push").status_ == connection_type::event_status_type::dispatched);
            for (std::size_t attempt_value = 0; attempt_value != 5000; ++attempt_value) {
                if (const auto intent = connection.peek_transport_intent()) {
                    if (intent->token_.kind_ == connection_type::transport_intent_kind_type::open_push_stream) {
                        RUVIA_CHECK(!open_observed);
                        open_observed = true;
                        RUVIA_CHECK_EQ(intent->token_.id_.stream_id_, std::uint64_t{0});
                        RUVIA_CHECK_EQ(intent->token_.id_.push_id_, std::optional<std::uint64_t>{0});
                        if (mode == 3) {
                            RUVIA_CHECK(connection.accept_control({.kind_ = control_type::kind::stream_reset,
                                                                      .id_ = {epoch, base_generation, 0},
                                                                      .value_ = request_wire(fixture_value.worker_, "GET", "/push").size()})
                                            .status_ == connection_type::event_status_type::stream_cancelled);
                        } else if (mode == 4) {
                            RUVIA_CHECK(connection.request_stop());
                            const auto close = connection.peek_transport_intent();
                            RUVIA_CHECK(close && close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
                            RUVIA_CHECK(connection.ack_transport_intent(close->token_));
                        } else if (mode == 7) {
                            constexpr std::array<char, 3> cancel{3, 1, 0};
                            RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, base_generation, 2}, cancel).status_ == connection_type::event_status_type::accepted);
                        }
                        auto forged = intent->token_;
                        forged.id_.push_id_ = 1;
                        RUVIA_CHECK(!connection.ack_transport_intent(forged, connection_type::push_stream_open_result_type{.status_ = connection_type::push_stream_open_result_type::status_type::opened, .stream_id_ = 31}));
                        RUVIA_CHECK(!connection.ack_transport_intent(intent->token_));
                        RUVIA_CHECK(connection.ack_transport_intent(intent->token_,
                            connection_type::push_stream_open_result_type{.status_ = mode == 2 ? connection_type::push_stream_open_result_type::status_type::unavailable : connection_type::push_stream_open_result_type::status_type::opened,
                                .stream_id_ = 31}));
                        RUVIA_CHECK(!connection.ack_transport_intent(intent->token_, connection_type::push_stream_open_result_type{}));
                    } else {
                        RUVIA_CHECK(connection.ack_transport_intent(intent->token_));
                    }
                }
                if (!cancelled_active_push && mode == 9 && fixture_value.routes_.handlers_.slow_started_observed_) {
                    std::array<char, 32> priority{};
                    const auto encoded = ruvia::encode_http3_priority_update(priority, {.element_id_ = 0, .push_ = true, .fields_ = {.urgency_ = 1, .incremental_ = true}});
                    RUVIA_CHECK((encoded.index() == 0));
                    RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, base_generation, 2}, std::span(priority).first(std::get<0>(encoded))).status_ == connection_type::event_status_type::accepted);
                    constexpr std::array<char, 3> cancel{3, 1, 0};
                    RUVIA_CHECK(accept_wire_bytes(connection, inbound, {epoch, base_generation, 2}, cancel).status_ == connection_type::event_status_type::accepted);
                    cancelled_active_push = true;
                } else if (!cancelled_active_push && mode == 10 && connection.request_info(31).status_ == connection_type::request_status_type::running) {
                    RUVIA_CHECK(connection.accept_control({.kind_ = control_type::kind::stream_reset,
                                                              .id_ = {epoch, base_generation, 31, 0}})
                                    .status_ == connection_type::event_status_type::stream_cancelled);
                    cancelled_active_push = true;
                }
                for (std::size_t turn = 0; turn != 8; ++turn) {
                    (void)connection.publish_one(all_work_lanes);
                    control_type control;
                    while (outbound.try_receive_control(control)) {
                        if (control.kind_ == control_type::kind::stream_fin) {
                            wires[control.id_.stream_id_].final_wire_bytes_ = control.value_;
                        }
                    }
                    buffer::borrowed_block block;
                    while (outbound.try_receive(block)) {
                        if (!block.critical()) {
                            RUVIA_CHECK_EQ(block.id().push_id_, block.id().stream_id_ == 31 ? std::optional<std::uint64_t>{0} : std::nullopt);
                            auto bytes_value = block.bytes();
                            wires[block.id().stream_id_].bytes_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
                        }
                        block.release();
                    }
                    (void)connection.reactivate_blocked(all_work_lanes);
                }
                if (connection.active_task_count() == 0 && connection.pending_transport_intent_count() == 0) {
                    break;
                }
                co_await ruvia::sleep_for(worker_value, 1ms, fixture_value.worker_stop_);
            }
            RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
            RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
            RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
            RUVIA_CHECK_EQ(open_observed, mode != 1 && mode != 5 && mode != 6);
            RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.push_accepted_, mode == 0 || mode == 8 || mode == 9 || mode == 10);
            if (mode == 9 || mode == 10) {
                RUVIA_CHECK(cancelled_active_push);
                RUVIA_CHECK(!connection.transport_close_required());
                if (mode == 9) {
                    RUVIA_CHECK(fixture_value.routes_.handlers_.slow_observed_stop_);
                    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.pushed_priority_.urgency_, std::uint8_t{1});
                    RUVIA_CHECK(fixture_value.routes_.handlers_.pushed_priority_.incremental_);
                }
            }
            if (mode != 3 && mode != 4) {
                RUVIA_CHECK(fixture_value.routes_.handlers_.push_completed_ || mode == 6);
                auto& parent_value = wires[0];
                RUVIA_CHECK(parent_value.final_wire_bytes_.has_value());
                RUVIA_CHECK_EQ(*parent_value.final_wire_bytes_, parent_value.bytes_.size());
                ruvia::http3_connection client(ruvia::http3_peer_role::client, fixture_value.worker_.resource(), {.max_push_id_ = 0});
                RUVIA_CHECK(client.register_client_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
                std::unordered_map<std::uint64_t, std::string> bodies;
                auto capture_value = +[](void* raw, const ruvia::http3_connection_event& event) {
                    if (event.kind_ == ruvia::http3_connection_event_kind::body) {
                        (*static_cast<std::unordered_map<std::uint64_t, std::string>*>(raw))[event.stream_id_].append(event.body_.data(), event.body_.size());
                    }
                };
                RUVIA_CHECK(client.feed(0, std::span(parent_value.bytes_), true, false, capture_value, &bodies).scope_ == ruvia::http3_connection_error_scope::none);
                if (mode == 0 || mode == 8) {
                    auto& pushed = wires[31];
                    RUVIA_CHECK(pushed.final_wire_bytes_.has_value());
                    RUVIA_CHECK_EQ(*pushed.final_wire_bytes_, pushed.bytes_.size());
                    RUVIA_CHECK(client.feed(31, std::span(pushed.bytes_), true, false, capture_value, &bodies).scope_ == ruvia::http3_connection_error_scope::none);
                    RUVIA_CHECK_EQ(bodies[0], "parent");
                    RUVIA_CHECK_EQ(bodies[31], mode == 8 ? "" : "/first");
                    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.pushed_cookie_, "pushed");
                    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.pushed_header_, "owned-header");
                    RUVIA_CHECK(client.promised_request(0) != nullptr);
                    RUVIA_CHECK_EQ(client.promised_request(0)->path_, "/first");
                    if (iteration == 0) {
                        retained_promise_path = client.promised_request(0)->path_;
                        retained_body = bodies[31];
                    }
                    RUVIA_CHECK_EQ(retained_promise_path, "/first");
                    RUVIA_CHECK_EQ(retained_body, "/first");
                } else if (mode == 9 || mode == 10) {
                    RUVIA_CHECK(client.promised_request(0) != nullptr);
                    RUVIA_CHECK_EQ(bodies[0], "parent");
                } else {
                    RUVIA_CHECK(client.promised_request(0) == nullptr);
                    if (mode != 6) {
                        RUVIA_CHECK_EQ(bodies[0], "parent");
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        (void)connection.request_stop();
        while (const auto intent = connection.peek_transport_intent()) {
            if (intent->token_.kind_ == connection_type::transport_intent_kind_type::open_push_stream) {
                (void)connection.ack_transport_intent(intent->token_, connection_type::push_stream_open_result_type{.status_ = connection_type::push_stream_open_result_type::status_type::stopped});
            } else {
                (void)connection.ack_transport_intent(intent->token_);
            }
        }
        co_await connection.join();
        RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
        RUVIA_CHECK(inbound.stop());
        RUVIA_CHECK(outbound.stop());
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (iteration == 96) {
            warmed_live_allocations = upstream.live_allocations();
        } else if (iteration > 96 && iteration < 128) {
            RUVIA_CHECK_EQ(upstream.live_allocations(), warmed_live_allocations);
        }
    }
}
}  // namespace

RUVIA_TEST(http3_server_connection_push_routes_owned_promises_and_settles_open_cancellation_and_stop) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_push_publication(fixture, worker_value, upstream, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
