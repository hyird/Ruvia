#include "http3_server_connection_fixture.h"

namespace {

ruvia::task<void> exercise_unknown_uni_fin_order(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, std::uint64_t connection_generation, bool fin_first,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});

    constexpr std::uint64_t unknown_stream_id = 2;
    constexpr std::array<char, 1> unknown_stream_type{static_cast<char>(0x21)};
    const message_id_type unknown_id{epoch, connection_generation, unknown_stream_id};
    const auto unknown_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(unknown_stream_type.data()), unknown_stream_type.size());
    if (!accepted(inbound.try_send(unknown_id, unknown_bytes)) ||
        !accepted(inbound.try_send_control(
            {control_type::kind::stream_fin, unknown_id, unknown_stream_type.size()}))) {
        throw std::runtime_error("HTTP/3 unknown-unidirectional fixture buffer is full");
    }

    const auto accept_unknown_data = [&]() {
        buffer::borrowed_block block;
        if (!inbound.try_receive(block)) {
            throw std::runtime_error("HTTP/3 unknown-unidirectional data is missing");
        }
        auto result_value = connection.accept_data(block);
        block.release();
        return result_value;
    };
    const auto accept_unknown_fin = [&]() {
        control_type fin;
        if (!inbound.try_receive_control(fin)) {
            throw std::runtime_error("HTTP/3 unknown-unidirectional FIN is missing");
        }
        return connection.accept_control(fin);
    };

    connection_type::event_result_type completed;
    if (fin_first) {
        const auto deferred = accept_unknown_fin();
        RUVIA_CHECK(deferred.status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK(deferred.input_.status_ == connection_type::input_type::status_type::deferred_fin);
        RUVIA_CHECK(!deferred.connection_close_required_);
        completed = accept_unknown_data();
    } else {
        const auto fed = accept_unknown_data();
        RUVIA_CHECK(fed.status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK(fed.input_.status_ == connection_type::input_type::status_type::fed);
        RUVIA_CHECK(!fed.connection_close_required_);
        completed = accept_unknown_fin();
    }
    RUVIA_CHECK(completed.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK(completed.input_.status_ == connection_type::input_type::status_type::finished);
    RUVIA_CHECK(!completed.connection_close_required_);
    RUVIA_CHECK(!connection.stopped());
    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.request_info(unknown_stream_id).status_,
        connection_type::request_status_type::unknown);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.tracked_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});

    const auto normal = route_request(connection, inbound, fixture_value.worker_,
        {epoch, connection_generation, 0}, "GET", "/first");
    RUVIA_CHECK(normal.status_ == connection_type::event_status_type::dispatched);
    std::array<published_wire, 6> wires{};
    constexpr std::array<std::uint64_t, 1> normal_id{0};
    co_await publish_group(connection, outbound, wires, normal_id,
        worker_value, fixture_value.worker_stop_, false, ruvia_ctx);
    const auto response = decode_response(wires[0], ruvia::http_known_method::get,
        0, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
    RUVIA_CHECK(response.body_ == "/first");

    RUVIA_CHECK(connection.request_stop());
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
}

ruvia::task<void> exercise_critical_uni_fin(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, std::uint64_t connection_generation,
    std::uint64_t stream_id, char stream_type_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(2, 2, 2, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});

    const message_id_type id{epoch, connection_generation, stream_id};
    const std::array<char, 1> stream_type_bytes{stream_type_value};
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(stream_type_bytes.data()), stream_type_bytes.size());
    if (!accepted(inbound.try_send(id, bytes_value))) {
        throw std::runtime_error("HTTP/3 critical-unidirectional fixture buffer is full");
    }
    buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 critical-unidirectional data is missing");
    }
    const auto fed = connection.accept_data(block);
    block.release();
    RUVIA_CHECK(fed.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK(fed.input_.status_ == connection_type::input_type::status_type::fed);

    const auto closed = connection.accept_control(
        {control_type::kind::stream_fin, id, stream_type_bytes.size()});
    RUVIA_CHECK(closed.status_ == connection_type::event_status_type::protocol_error);
    RUVIA_CHECK(closed.input_.status_ == connection_type::input_type::status_type::protocol_error);
    RUVIA_CHECK(closed.input_.protocol_.code_ == ruvia::http3_connection_error_code::closed_critical_stream);
    RUVIA_CHECK(closed.connection_close_required_);
    RUVIA_CHECK(connection.transport_close_required());
    const auto close_intent = connection.peek_transport_intent();
    RUVIA_CHECK(close_intent.has_value());
    RUVIA_CHECK(close_intent->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(close_intent->close_reason_ ==
                connection_type::transport_close_reason_type::connection_protocol_error);
    RUVIA_CHECK(close_intent->connection_error_code_ ==
                ruvia::http3_connection_error_code::closed_critical_stream);
    const auto persistent_close = connection.accept_control(
        {control_type::kind::stream_fin, id, stream_type_bytes.size()});
    RUVIA_CHECK(persistent_close.status_ == connection_type::event_status_type::admission_closed);
    RUVIA_CHECK(persistent_close.connection_close_required_);
    RUVIA_CHECK(persistent_close.input_.status_ == connection_type::input_type::status_type::stopped);
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == close_intent->token_);

    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
}

ruvia::task<void> exercise_unknown_uni_fin_orders(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    co_await exercise_unknown_uni_fin_order(
        fixture_value, worker_value, base_generation + 20, false, ruvia_ctx);
    co_await exercise_unknown_uni_fin_order(
        fixture_value, worker_value, base_generation + 21, true, ruvia_ctx);
}

ruvia::task<void> exercise_critical_uni_fins(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    co_await exercise_critical_uni_fin(
        fixture_value, worker_value, base_generation + 30, 2, static_cast<char>(0x00), ruvia_ctx);
    co_await exercise_critical_uni_fin(
        fixture_value, worker_value, base_generation + 31, 6, static_cast<char>(0x02), ruvia_ctx);
    co_await exercise_critical_uni_fin(
        fixture_value, worker_value, base_generation + 32, 10, static_cast<char>(0x03), ruvia_ctx);
}

}  // namespace

RUVIA_TEST(http3_server_connection_ignores_unknown_client_uni_streams_in_either_fin_order) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_unknown_uni_fin_orders(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_rejects_fin_on_peer_critical_uni_streams) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_critical_uni_fins(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
