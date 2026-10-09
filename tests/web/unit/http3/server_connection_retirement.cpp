#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::task<void> join_connection_and_notify(connection_type& connection,
    ruvia::worker_signal& started, bool& joined) {
    started.notify();
    co_await connection.join();
    joined = true;
}

ruvia::task<void> exercise_join_while_active(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    ruvia::worker_signal handler_started(worker_value);
    ruvia::worker_signal release_handler(worker_value);
    ruvia::worker_signal join_started(worker_value);
    fixture_value.routes_.handlers_.held_started_ = &handler_started;
    fixture_value.routes_.handlers_.held_release_ = &release_handler;
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(2, 2, 2, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 29, .max_tracked_streams_ = 8});

    const auto request = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 29, 0}, "GET", "/held");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    co_await handler_started.wait();
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{1});
    RUVIA_CHECK(connection.request_stop());

    bool joined = false;
    ruvia::task_scope join_scope(worker_value, {.resource_ = fixture_value.worker_.resource()});
    join_scope.spawn(join_connection_and_notify(connection, join_started, joined));
    co_await join_started.wait();
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{1});
    RUVIA_CHECK(!joined);

    release_handler.notify();
    co_await join_scope.join();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(fixture_value.routes_.handlers_.held_handler_finished_);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    simulate_global_stop_takeover(connection);
}

ruvia::task<void> exercise_reset_cancellation(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    ruvia::worker_signal slow_started(worker_value);
    fixture_value.routes_.handlers_.slow_started_ = &slow_started;
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(2, 2, 2, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 1, .max_tracked_streams_ = 8});

    const auto wire = request_wire(fixture_value.worker_, "GET", "/slow");
    const auto request = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 1, 0}, "GET", "/slow");
    if (request.status_ != connection_type::event_status_type::dispatched) {
        throw std::runtime_error("slow handler was not dispatched");
    }
    const bool slow_started_in_time =
        co_await wait_for_slow_start(fixture_value, worker_value, fixture_value.worker_stop_);
    RUVIA_CHECK(slow_started_in_time);
    if (!slow_started_in_time) {
        RUVIA_CHECK(connection.request_stop());
        require_watchdog_success(ruvia_ctx,
            co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
        co_await connection.join();
        simulate_global_stop_takeover(connection);
        co_return;
    }

    const message_id_type id{epoch, base_generation + 1, 0};
    if (!accepted(inbound.try_send_control({.kind_ = control_type::kind::stream_reset,
            .id_ = id,
            .value_ = wire.size()}))) {
        throw std::runtime_error("HTTP/3 test RESET buffer is full");
    }
    control_type reset;
    if (!inbound.try_receive_control(reset)) {
        throw std::runtime_error("HTTP/3 test RESET control is missing");
    }
    const auto cancelled = connection.accept_control(reset);
    RUVIA_CHECK(cancelled.status_ == connection_type::event_status_type::stream_cancelled);
    RUVIA_CHECK(cancelled.input_.status_ == connection_type::input_type::status_type::reset);
    RUVIA_CHECK(!cancelled.connection_close_required_);
    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});

    const bool retired_after_reset =
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_);
    require_watchdog_success(ruvia_ctx, retired_after_reset);
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK(fixture_value.routes_.handlers_.slow_observed_stop_);

    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK(connection.transport_close_required());
    const auto local_retirement = connection.accept_control(
        {.kind_ = control_type::kind::stream_reset, .id_ = id, .value_ = wire.size()});
    RUVIA_CHECK(local_retirement.status_ == connection_type::event_status_type::admission_closed);
    RUVIA_CHECK(local_retirement.input_.status_ == connection_type::input_type::status_type::stopped);
    RUVIA_CHECK(local_retirement.input_.status_ != connection_type::input_type::status_type::reset);
    RUVIA_CHECK(local_retirement.connection_close_required_);
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK(fixture_value.routes_.handlers_.slow_observed_stop_);
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    buffer::borrowed_block unexpected;
    control_type unexpected_control;
    RUVIA_CHECK(!outbound.try_receive(unexpected));
    RUVIA_CHECK(!outbound.try_receive_control(unexpected_control));
}

ruvia::task<void> exercise_partial_publish_stop(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 2, .max_tracked_streams_ = 8});

    const auto request = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 2, 0}, "GET", "/first");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    const auto published = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(published.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK(published.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);

    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK(!connection.request_stop());
    RUVIA_CHECK(connection.transport_close_required());
    RUVIA_CHECK(connection.publish_one(all_work_lanes).status_ ==
                connection_type::publish_status_type::no_ready_request);
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});

    std::array<published_wire, 6> wires{};
    drain_all(outbound, wires);
    RUVIA_CHECK(!wires[0].bytes_.empty());
    RUVIA_CHECK(!wires[0].final_wire_bytes_.has_value());
}

ruvia::task<void> exercise_reset_intent_merge_policy(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 89;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    constexpr std::uint64_t stream_id = 0;
    using test_access_type = ruvia::detail::http3_server_connection_reset_intent_test_access;

    RUVIA_CHECK(test_access_type::enqueue_local(connection, stream_id));
    const auto local = connection.peek_transport_intent();
    RUVIA_CHECK(local.has_value());
    if (!local) {
        throw std::runtime_error("local reset fixture intent was not retained");
    }
    RUVIA_CHECK(local->stream_reset_error_code_ == ruvia::http3_connection_error_code::request_cancelled);
    RUVIA_CHECK(test_access_type::enqueue_local(connection, stream_id));
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == local->token_);

    RUVIA_CHECK(test_access_type::enqueue_protocol(connection, stream_id,
        ruvia::http3_connection_error_code::message_error));
    const auto protocol = connection.peek_transport_intent();
    RUVIA_CHECK(protocol.has_value());
    if (!protocol) {
        throw std::runtime_error("protocol reset fixture intent was not retained");
    }
    RUVIA_CHECK(protocol->stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK_EQ(protocol->token_.id_.epoch_, local->token_.id_.epoch_);
    RUVIA_CHECK_EQ(protocol->token_.id_.connection_generation_,
        local->token_.id_.connection_generation_);
    RUVIA_CHECK_EQ(protocol->token_.id_.stream_id_, local->token_.id_.stream_id_);
    RUVIA_CHECK(protocol->token_.sequence_ != local->token_.sequence_);
    RUVIA_CHECK(connection.ack_transport_intent(local->token_));
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == protocol->token_);

    RUVIA_CHECK(test_access_type::enqueue_protocol(connection, stream_id,
        ruvia::http3_connection_error_code::excessive_load));
    RUVIA_CHECK(test_access_type::enqueue_local(connection, stream_id));
    RUVIA_CHECK(test_access_type::enqueue_protocol(connection, stream_id,
        ruvia::http3_connection_error_code::message_error));
    const auto unchanged = connection.peek_transport_intent();
    RUVIA_CHECK(unchanged.has_value());
    RUVIA_CHECK(unchanged->token_ == protocol->token_);
    RUVIA_CHECK(unchanged->stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK(connection.ack_transport_intent(protocol->token_));
    RUVIA_CHECK(!connection.ack_transport_intent(protocol->token_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK(!connection.transport_retired());

    RUVIA_CHECK(connection.request_stop());
    simulate_global_stop_takeover(connection);
    co_await connection.join();
}

ruvia::task<void> exercise_persistent_protocol_reset_intent(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 90;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    const message_id_type id{epoch, connection_generation, 0};
    const message_id_type queued_control_id{epoch, connection_generation, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {control_type::kind::writable, queued_control_id, 0})));

    const std::array<ruvia::http3_field_section_field_view, 4> invalid_fields{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encode_http3_field_section(invalid_fields, fixture_value.worker_.resource());
    if ((section.index() != 0)) {
        throw std::runtime_error("HTTP/3 malformed-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
    {
        const auto failure = accept_wire_bytes(connection, inbound, id, wire);
        RUVIA_CHECK(failure.status_ == connection_type::event_status_type::protocol_error);
        RUVIA_CHECK(failure.input_.status_ == connection_type::input_type::status_type::protocol_error);
        RUVIA_CHECK(failure.input_.protocol_.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(failure.input_.protocol_.code_ == ruvia::http3_connection_error_code::message_error);
        RUVIA_CHECK(!failure.connection_close_required_);
    }

    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::protocol_error);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    const auto intent = connection.peek_transport_intent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 stream protocol reset intent was not retained");
    }
    RUVIA_CHECK(intent->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK_EQ(intent->token_.id_.stream_id_, std::uint64_t{0});
    RUVIA_CHECK(intent->stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);

    // A late duplicate cannot change the persistent intent or its token.
    const auto duplicate = accept_wire_bytes(connection, inbound, id, wire);
    RUVIA_CHECK(duplicate.input_.status_ == connection_type::input_type::status_type::closed_stream);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == intent->token_);

    const control_type reset_control{.kind_ = control_type::kind::stream_reset,
        .id_ = intent->token_.id_,
        .stream_reset_error_code_ = intent->stream_reset_error_code_};
    RUVIA_CHECK(outbound.try_send_control(reset_control) == buffer::control_result::full);
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == intent->token_);
    control_type queued_control;
    RUVIA_CHECK(outbound.try_receive_control(queued_control));
    RUVIA_CHECK(queued_control.kind_ == control_type::kind::writable);
    RUVIA_CHECK(!outbound.has_pending());
    const auto sent = outbound.try_send_control(reset_control);
    RUVIA_CHECK(sent == buffer::control_result::sent);
    RUVIA_CHECK_EQ(reset_control.value_, std::uint64_t{0});
    RUVIA_CHECK(connection.ack_transport_intent(intent->token_));
    RUVIA_CHECK(!connection.ack_transport_intent(intent->token_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK(!connection.transport_retired());
    control_type received_reset;
    RUVIA_CHECK(outbound.try_receive_control(received_reset));
    RUVIA_CHECK(received_reset.kind_ == control_type::kind::stream_reset);
    RUVIA_CHECK_EQ(received_reset.value_, std::uint64_t{0});
    RUVIA_CHECK(received_reset.stream_reset_error_code_ == ruvia::http3_connection_error_code::message_error);
    RUVIA_CHECK(!outbound.has_pending());
    const auto after_handoff = accept_wire_bytes(connection, inbound, id, wire);
    RUVIA_CHECK(after_handoff.input_.status_ == connection_type::input_type::status_type::closed_stream);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK(!connection.transport_retired());

    RUVIA_CHECK(connection.request_stop());
    simulate_global_stop_takeover(connection);
    co_await connection.join();
}

ruvia::task<void> exercise_stream_excessive_load_reset_intent(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 91;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch,
            .connection_generation_ = connection_generation,
            .session_ = {.max_live_streams_ = 200},
            .max_tracked_streams_ = 200});
    constexpr std::array<char, 1> partial_data{0};
    connection_type::event_result_type result;
    for (std::uint64_t stream_id = 0; stream_id <= 128 * 4; stream_id += 4) {
        result = accept_wire_bytes(connection, inbound,
            {epoch, connection_generation, stream_id}, partial_data);
        if (stream_id < 128 * 4) {
            RUVIA_CHECK(result.status_ == connection_type::event_status_type::accepted);
            RUVIA_CHECK(result.input_.status_ == connection_type::input_type::status_type::fed);
        }
    }
    RUVIA_CHECK(result.status_ == connection_type::event_status_type::protocol_error);
    RUVIA_CHECK(result.input_.status_ == connection_type::input_type::status_type::protocol_error);
    RUVIA_CHECK(result.input_.protocol_.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(result.input_.protocol_.code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK(!result.connection_close_required_);
    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.request_info(128 * 4).status_,
        connection_type::request_status_type::protocol_error);
    const auto intent = connection.peek_transport_intent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 stream excessive-load reset intent was not retained");
    }
    RUVIA_CHECK(intent->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK(intent->stream_reset_error_code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});

    RUVIA_CHECK(connection.request_stop());
    simulate_global_stop_takeover(connection);
    co_await connection.join();
}

ruvia::task<connection_type::transport_intent_token_type> create_peer_limit_intent(
    connection_type& connection, buffer& inbound, ruvia::worker_memory& worker_value,
    std::uint64_t connection_epoch, std::uint64_t connection_generation, const ruvia::worker_handle& worker_handle_value,
    const ruvia::stop_token& stop_token_value, ruvia::testing::test_context& ruvia_ctx) {
    std::array<char, 64> settings_payload{};
    ruvia::http3_settings settings;
    settings.max_field_section_size_ = 0;
    const auto settings_size = ruvia::encode_http3_settings(settings_payload, settings);
    if ((settings_size.index() != 0)) {
        throw std::runtime_error("HTTP/3 peer-settings fixture encoding failed");
    }
    std::string control_wire(1, '\0');
    control_wire += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::settings),
        std::string_view(settings_payload.data(), std::get<0>(settings_size)));
    const message_id_type control_id{connection_epoch, connection_generation, 2};
    const auto control_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(control_wire.data()), control_wire.size());
    if (!accepted(inbound.try_send(control_id, control_bytes))) {
        throw std::runtime_error("HTTP/3 peer-settings fixture buffer is full");
    }
    buffer::borrowed_block control_block;
    if (!inbound.try_receive(control_block)) {
        throw std::runtime_error("HTTP/3 peer-settings data is missing");
    }
    const auto settings_result = connection.accept_data(control_block);
    control_block.release();
    RUVIA_CHECK(settings_result.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK(settings_result.input_.status_ == connection_type::input_type::status_type::fed);

    const auto request = route_request(connection, inbound, worker_value,
        {connection_epoch, connection_generation, 0}, "GET", "/first");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    const bool task_retired =
        co_await wait_for_task_count(connection, 0, worker_handle_value, stop_token_value);
    require_watchdog_success(ruvia_ctx, task_retired);
    const auto intent = connection.peek_transport_intent();
    RUVIA_CHECK(intent.has_value());
    if (!intent) {
        throw std::runtime_error("HTTP/3 peer-limit reset intent was not retained");
    }
    RUVIA_CHECK(intent->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    co_return intent->token_;
}

ruvia::task<void> exercise_stale_intent_tokens(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    constexpr std::uint64_t old_generation = base_generation + 50;
    test_activation_signal old_scheduler(worker_value);
    buffer old_inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer old_outbound(2, 2, 2, fixture_value.worker_.resource());
    connection_type old_owner(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, old_outbound, old_scheduler,
        {.epoch_ = epoch, .connection_generation_ = old_generation, .max_tracked_streams_ = 8});
    const auto old_token = co_await create_peer_limit_intent(old_owner, old_inbound,
        fixture_value.worker_, epoch, old_generation, worker_value, fixture_value.worker_stop_, ruvia_ctx);
    RUVIA_CHECK(old_owner.request_stop());
    co_await old_owner.join();
    simulate_global_stop_takeover(old_owner);

    constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 2> new_identities{{
        {epoch + 1, old_generation},
        {epoch, old_generation + 1},
    }};
    for (const auto& [connection_epoch, connection_generation] : new_identities) {
        test_activation_signal scheduler(worker_value);
        buffer inbound(4, 4, 4, fixture_value.worker_.resource());
        buffer outbound(2, 2, 2, fixture_value.worker_.resource());
        connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, scheduler,
            {.epoch_ = connection_epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
        const auto current_token = co_await create_peer_limit_intent(connection, inbound,
            fixture_value.worker_, connection_epoch, connection_generation, worker_value, fixture_value.worker_stop_, ruvia_ctx);

        RUVIA_CHECK_EQ(current_token.id_.stream_id_, old_token.id_.stream_id_);
        RUVIA_CHECK_EQ(current_token.sequence_, old_token.sequence_);
        RUVIA_CHECK(!connection.ack_transport_intent(old_token));
        RUVIA_CHECK(connection.peek_transport_intent()->token_ == current_token);

        RUVIA_CHECK(connection.request_stop());
        RUVIA_CHECK(!connection.request_stop());
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{2});
        const auto close_intent = connection.peek_transport_intent();
        RUVIA_CHECK(close_intent.has_value());
        RUVIA_CHECK(close_intent->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
        RUVIA_CHECK(close_intent->close_reason_ == connection_type::transport_close_reason_type::local_stop);
        RUVIA_CHECK(!connection.transport_retired());
        RUVIA_CHECK(connection.ack_transport_intent(close_intent->token_));
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
        RUVIA_CHECK(!connection.transport_retired());
        RUVIA_CHECK(connection.ack_transport_intent(current_token));
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
        RUVIA_CHECK(connection.confirm_transport_retired(
            {.epoch_ = connection_epoch, .connection_generation_ = connection_generation}));
        RUVIA_CHECK(connection.transport_retired());
        co_await connection.join();
    }
}

ruvia::task<void> exercise_global_stop_with_unsent_reset(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(2, 2, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 60, .max_tracked_streams_ = 8});
    const message_id_type queued_control_id{epoch, base_generation + 60, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {control_type::kind::writable, queued_control_id, 0})));

    const auto reset_token = co_await create_peer_limit_intent(connection, inbound,
        fixture_value.worker_, epoch, base_generation + 60, worker_value, fixture_value.worker_stop_, ruvia_ctx);
    RUVIA_CHECK(outbound.try_send_control(
                    {control_type::kind::stream_reset, reset_token.id_, 0}) == buffer::control_result::full);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});

    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK(connection.transport_close_required());
    RUVIA_CHECK(!connection.transport_retired());
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{2});
    const bool tasks_joined =
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_);
    require_watchdog_success(ruvia_ctx, tasks_joined);
    co_await connection.join();
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK(!connection.transport_retired());
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{2});

    const auto close_intent = connection.peek_transport_intent();
    RUVIA_CHECK(close_intent.has_value());
    RUVIA_CHECK(close_intent->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(connection.take_over_transport_retirement(
        {.epoch_ = epoch, .connection_generation_ = base_generation + 60}));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{2});
    RUVIA_CHECK(connection.ack_transport_intent(close_intent->token_));
    const auto reset_after_close = connection.peek_transport_intent();
    RUVIA_CHECK(reset_after_close.has_value());
    RUVIA_CHECK(reset_after_close->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK(connection.ack_transport_intent(reset_after_close->token_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK(!connection.transport_retired());
}

ruvia::task<void> exercise_retirement_intent_ack_order(
    fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal activation(worker_value);
    using test_access_type = ruvia::detail::http3_server_connection_reset_intent_test_access;
    for (const bool retire_before_ack : std::array{false, true}) {
        buffer outbound(1, 1, 1, fixture_value.worker_.resource());
        const auto connection_generation = base_generation + (retire_before_ack ? 171 : 170);
        connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.services_, fixture_value.options_, outbound, activation,
            {.epoch_ = epoch,
                .connection_generation_ = connection_generation,
                .max_tracked_streams_ = 8});
        RUVIA_CHECK(test_access_type::enqueue_local(connection, 0));
        RUVIA_CHECK(connection.request_stop());
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{2});
        if (retire_before_ack) {
            RUVIA_CHECK(connection.confirm_transport_retired(
                {.epoch_ = epoch, .connection_generation_ = connection_generation}));
        }
        const auto close = connection.peek_transport_intent();
        RUVIA_CHECK(close.has_value());
        RUVIA_CHECK(close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
        RUVIA_CHECK(connection.ack_transport_intent(close->token_));
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
        const auto reset = connection.peek_transport_intent();
        RUVIA_CHECK(reset.has_value());
        RUVIA_CHECK(reset->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
        RUVIA_CHECK_EQ(reset->token_.id_.stream_id_, std::uint64_t{0});
        if (!retire_before_ack) {
            RUVIA_CHECK(connection.confirm_transport_retired(
                {.epoch_ = epoch, .connection_generation_ = connection_generation}));
        }
        RUVIA_CHECK(connection.peek_transport_intent()->token_ == reset->token_);
        RUVIA_CHECK(connection.ack_transport_intent(reset->token_));
        RUVIA_CHECK(!connection.ack_transport_intent(reset->token_));
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
        RUVIA_CHECK(connection.transport_retired());
        RUVIA_CHECK(outbound.stop());
    }
    co_return;
}

ruvia::task<void> exercise_scheduler_settles_superseded_reset(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    using scheduler_type = ruvia::detail::http3_ready_scheduler;
    using state_type = ruvia::detail::http3_connection_state;
    using test_access_type = ruvia::detail::http3_server_connection_reset_intent_test_access;
    scheduler_type scheduler(worker_value, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    state_type state;
    const ruvia::detail::http3_connection_identity identity{epoch + 180, base_generation + 180};
    RUVIA_CHECK(state.reserve(scheduler, identity.epoch_, identity.connection_generation_) ==
                state_type::status::changed);
    const auto registration = state.registration();
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler local reset fixture slot unavailable");
    }
    RUVIA_CHECK(state.bind(identity) == state_type::status::changed);
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, registration->activation_,
        {.epoch_ = identity.epoch_,
            .connection_generation_ = identity.connection_generation_,
            .max_tracked_streams_ = 8});
    RUVIA_CHECK(state.attach_handler(identity, connection) == state_type::status::changed);
    struct execution_trace final {
        state_type* state_;
        connection_type* connection_;
        std::size_t executions_{};
        bool saw_unsettled_{};
        std::optional<connection_type::transport_intent_token_type> executed_token_;
    } trace{&state, &connection};
    state.set_transport_executor({&trace, [](void* context_value, ruvia::detail::http3_connection_identity executing_identity, const connection_type::transport_intent_type& intent) noexcept -> state_type::intent_execution_result {
                                      auto& executed = *static_cast<execution_trace*>(context_value);
                                      ++executed.executions_;
                                      executed.saw_unsettled_ = executed.connection_->pending_transport_intent_count() != 0;
                                      executed.executed_token_ = intent.token_;
                                      if (intent.token_.kind_ == connection_type::transport_intent_kind_type::connection_close &&
                                          executed.state_->mark_transport_retired(executing_identity) != state_type::status::changed) {
                                          std::terminate();
                                      }
                                      return {.outcome_ = state_type::execution_outcome::executed};
                                  }});
    RUVIA_CHECK(test_access_type::enqueue_local(connection, 0));
    const auto offered = scheduler.step();
    RUVIA_CHECK(offered.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(offered.intent_.token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK_EQ(trace.executions_, std::size_t{0});
    RUVIA_CHECK(test_access_type::enqueue_protocol(connection, 0,
        ruvia::http3_connection_error_code::message_error));
    const auto current = connection.peek_transport_intent();
    RUVIA_CHECK(current.has_value());
    RUVIA_CHECK(current->token_.sequence_ != offered.intent_.token_.sequence_);
    RUVIA_CHECK(scheduler.step().kind_ == scheduler_type::step_kind::idle);
    const auto first_execution = state.execute_intent(identity, offered.intent_);
    RUVIA_CHECK(first_execution.completed());
    RUVIA_CHECK(trace.saw_unsettled_);
    RUVIA_CHECK(trace.executed_token_ == offered.intent_.token_);
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == current->token_);
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_,
        offered.intent_.token_, first_execution.push_stream_));
    const auto replacement = scheduler.step();
    RUVIA_CHECK(replacement.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(replacement.intent_.token_ == current->token_);
    const auto replacement_execution = state.execute_intent(identity, replacement.intent_);
    RUVIA_CHECK(replacement_execution.completed());
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_,
        replacement.intent_.token_, replacement_execution.push_stream_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});

    RUVIA_CHECK(connection.request_stop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind_ == scheduler_type::step_kind::transport_intent);
    RUVIA_CHECK(close.intent_.token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(state.start_worker_draining(identity) == state_type::status::changed);
    const auto close_execution = state.execute_intent(identity, close.intent_);
    RUVIA_CHECK(close_execution.outcome_ == state_type::execution_outcome::transport_retired);
    RUVIA_CHECK(state.transport_retired());
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(scheduler.acknowledge_intent(registration->token_,
        close.intent_.token_, close_execution.push_stream_));
    co_await connection.join();
    RUVIA_CHECK(state.mark_worker_finalized(identity) == state_type::status::changed);
    RUVIA_CHECK(state.retire(identity) == state_type::status::changed);
    RUVIA_CHECK(state.ready_to_destroy());
    RUVIA_CHECK_EQ(trace.executions_, std::size_t{3});
    RUVIA_CHECK(outbound.stop());
}

}  // namespace

RUVIA_TEST(http3_server_connection_starts_join_while_handler_is_active) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_join_while_active(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_merges_pending_reset_codes_with_fresh_tokens) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_reset_intent_merge_policy(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_retirement_keeps_exact_intent_debts_across_ack_order) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_retirement_intent_ack_order(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_scheduler_settles_superseded_reset_before_close) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_scheduler_settles_superseded_reset(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_persists_core_stream_reset_code) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_persistent_protocol_reset_intent(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_persists_stream_excessive_load_reset_code) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_stream_excessive_load_reset_intent(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_reset_after_fin_cancels_and_joins_handler) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_reset_cancellation(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_stop_after_partial_publish_closes_admission_and_joins) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_partial_publish_stop(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_rejects_stale_intent_tokens_and_prioritizes_close) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_stale_intent_tokens(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_global_stop_takes_over_unsent_reset_after_join) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_global_stop_with_unsent_reset(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
