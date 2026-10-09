#include <variant>

#include "http3_server_connection_fixture.h"

namespace {

ruvia::task<void> exercise_successful_connection(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    constexpr std::array<std::uint64_t, 4> first_ids{0, 4, 8, 12};
    constexpr std::array<std::uint64_t, 2> second_ids{16, 20};
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};

    test_activation_signal scheduler(worker_value);
    buffer inbound(8, 8, 8, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation, .max_tracked_streams_ = 32});

    const auto stale = connection.accept_control(
        {control_type::kind::stream_fin, {epoch + 1, base_generation, 12}, 0});
    RUVIA_CHECK(stale.status_ == connection_type::event_status_type::input_rejected);
    RUVIA_CHECK(stale.input_.status_ == connection_type::input_type::status_type::foreign_epoch);

    const auto first = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, first_ids[0]}, "GET", "/first");
    RUVIA_CHECK(first.status_ == connection_type::event_status_type::dispatched);
    const auto task_count_before_duplicate = connection.active_task_count();
    const auto first_wire_size = request_wire(fixture_value.worker_, "GET", "/first").size();
    const auto duplicate_fin = connection.accept_control(
        {control_type::kind::stream_fin, {epoch, base_generation, first_ids[0]}, first_wire_size});
    RUVIA_CHECK(duplicate_fin.input_.status_ == connection_type::input_type::status_type::duplicate_fin);
    RUVIA_CHECK_EQ(connection.active_task_count(), task_count_before_duplicate);

    const auto second = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, first_ids[1]}, "POST", "/body", "sibling-body");
    RUVIA_CHECK(second.status_ == connection_type::event_status_type::dispatched);
    const auto third = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, first_ids[2]}, "GET", "/throw");
    RUVIA_CHECK(third.status_ == connection_type::event_status_type::dispatched);
    const auto rejected = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, 12}, "GET", "/first", {}, expect_field);
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK(rejected.rejection_ == connection_type::session::rejection_type::expectation_unsupported);
    RUVIA_CHECK_EQ(connection.request_info(12).status_, connection_type::request_status_type::rejected);

    std::array<published_wire, 6> wires{};
    co_await publish_group(connection, outbound, wires, first_ids,
        worker_value, fixture_value.worker_stop_, true, ruvia_ctx);
    const auto rejected_response = decode_response(wires[wire_index(12)],
        ruvia::http_known_method::get, 12, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(rejected_response.status_, std::uint16_t{417});
    RUVIA_CHECK_EQ(rejected_response.message_ends_, std::size_t{1});

    const auto fourth = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, second_ids[0]}, "GET", "/first");
    RUVIA_CHECK(fourth.status_ == connection_type::event_status_type::dispatched);
    const auto fifth = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation, second_ids[1]}, "POST", "/body", "later-body");
    RUVIA_CHECK(fifth.status_ == connection_type::event_status_type::dispatched);
    co_await publish_group(connection, outbound, wires, second_ids,
        worker_value, fixture_value.worker_stop_, false, ruvia_ctx);

    RUVIA_CHECK(fixture_value.routes_.handlers_.body_seen_ == "later-body");
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(connection.request_info(4).status_, connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(connection.request_info(8).status_, connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(connection.tracked_request_count(), std::size_t{6});

    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK(connection.transport_close_required());
    {
        auto cold_join = connection.join();
        static_cast<void>(cold_join);
    }
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});

    constexpr std::array<std::uint64_t, 5> published_ids{0, 4, 8, 16, 20};
    for (const auto stream_id : published_ids) {
        const auto method = stream_id == 4 || stream_id == 20
                                ? ruvia::http_known_method::post
                                : ruvia::http_known_method::get;
        const auto response = decode_response(wires[wire_index(stream_id)], method,
            stream_id, fixture_value.worker_.resource());
        RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
        RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
        if (stream_id == 8) {
            RUVIA_CHECK_EQ(response.status_, std::uint16_t{500});
        } else {
            RUVIA_CHECK_EQ(response.status_, std::uint16_t{200});
        }
        if (stream_id == 0 || stream_id == 16) {
            RUVIA_CHECK(response.body_ == "/first");
        } else if (stream_id == 4) {
            RUVIA_CHECK(response.body_ == "sibling-body");
        } else if (stream_id == 20) {
            RUVIA_CHECK(response.body_ == "later-body");
        }
    }
}

ruvia::task<void> exercise_early_rejection_before_request_fin(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 91;
    const message_id_type id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto wire = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    const auto rejected = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK(rejected.rejection_ == connection_type::session::rejection_type::expectation_unsupported);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK(connection.work_state().runnable_.data_);

    std::array<published_wire, 6> wires{};
    const auto headers = connection.publish_one({.data_ = true});
    RUVIA_CHECK(headers.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK(headers.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    drain_data_only(outbound, wires);
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.status_ == connection_type::publish_status_type::attempted);
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    drain_all(outbound, wires);
    const auto response = decode_response(wires[0], ruvia::http_known_method::get,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{417});
    RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
    RUVIA_CHECK_EQ(connection.request_info(id.stream_id_).status_,
        connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{1});
    const auto late_body = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), "late");
    const auto discarded = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(late_body.data(), late_body.size()));
    RUVIA_CHECK(discarded.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});

    const auto input_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size() + late_body.size())});
    RUVIA_CHECK(input_fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

enum class receive_retirement { fin,
    reset,
    stop };

ruvia::task<void> exercise_websocket_rejection_receive_retirement(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, receive_retirement retirement,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    ruvia::detail::http3_server_body_budget budget(64);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    const message_id_type id{epoch, base_generation + 98, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler, budget,
        {.epoch_ = id.epoch_, .connection_generation_ = id.connection_generation_, .max_tracked_streams_ = 4});
    const auto request_head = websocket_request_wire(fixture_value.worker_, "12");
    const auto admitted = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(request_head.data(), request_head.size()));
    RUVIA_CHECK(admitted.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    std::array<published_wire, 6> wires{};
    constexpr std::array<std::uint64_t, 1> rejected_stream{0};
    co_await publish_group(connection, outbound, wires, rejected_stream,
        worker_value, fixture_value.worker_stop_, false, ruvia_ctx);
    const auto response = decode_response(wires[0], ruvia::http_known_method::connect,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{400});
    RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(response.message_ends_, std::size_t{1});
    RUVIA_CHECK(!connection.transport_close_required());
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});

    const auto late_body = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), "late");
    const auto body = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(late_body.data(), late_body.size()));
    RUVIA_CHECK(body.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{4});
    RUVIA_CHECK(!fixture_value.routes_.handlers_.websocket_started_observed_);

    const message_id_type sibling_id{id.epoch_, id.connection_generation_, 4};
    const auto sibling = route_request(connection, inbound, fixture_value.worker_, sibling_id, "GET", "/first");
    RUVIA_CHECK(sibling.status_ == connection_type::event_status_type::dispatched);
    co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
    constexpr std::array<std::uint64_t, 1> sibling_stream{4};
    co_await publish_group(connection, outbound, wires, sibling_stream,
        worker_value, fixture_value.worker_stop_, false, ruvia_ctx);
    const auto sibling_response = decode_response(wires[1], ruvia::http_known_method::get,
        sibling_id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(sibling_response.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(sibling_response.body_, "/first");
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{4});

    if (retirement != receive_retirement::stop) {
        const auto receive_end = connection.accept_control({retirement == receive_retirement::fin ? control_type::kind::stream_fin : control_type::kind::stream_reset,
            id, static_cast<std::uint64_t>(request_head.size() + late_body.size())});
        RUVIA_CHECK(receive_end.status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK(receive_end.input_.status_ == (retirement == receive_retirement::fin
                                                          ? connection_type::input_type::status_type::finished
                                                          : connection_type::input_type::status_type::reset));
        RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK(!connection.transport_close_required());
    }
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

ruvia::task<void> exercise_peer_limit_zero_rejection(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 94;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    std::array<char, 64> settings_payload{};
    ruvia::http3_settings settings;
    settings.max_field_section_size_ = 0;
    const auto settings_size = ruvia::encode_http3_settings(settings_payload, settings);
    if ((settings_size.index() != 0)) {
        throw std::runtime_error("HTTP/3 peer setting encoding failed");
    }
    std::string control_wire(1, '\0');
    control_wire += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::settings),
        std::string_view(settings_payload.data(), std::get<0>(settings_size)));
    const auto control = accept_wire_bytes(connection, inbound,
        {epoch, connection_generation, 2}, std::span<const char>(control_wire.data(), control_wire.size()));
    RUVIA_CHECK(control.status_ == connection_type::event_status_type::accepted);

    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const message_id_type id{epoch, connection_generation, 0};
    const auto wire = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    const auto rejected = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    std::array<published_wire, 6> wires{};
    const auto headers = connection.publish_one({.data_ = true});
    RUVIA_CHECK(headers.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    drain_data_only(outbound, wires);
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    drain_all(outbound, wires);
    const auto response = decode_response(wires[0], ruvia::http_known_method::get,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{417});
    RUVIA_CHECK(!connection.peek_transport_intent().has_value());
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    const auto input_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(input_fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_rejected_body_status(fixture& fixture_value,
    std::uint64_t connection_generation, ruvia::detail::http3_sans_io_session_limits limits,
    connection_type::session::rejection_type expected_rejection, std::uint16_t expected_status,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    const message_id_type id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .session_ = limits, .max_tracked_streams_ = 4});
    const auto wire = request_wire(fixture_value.worker_, "POST", "/body", "abcdef");
    const auto rejected = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK(rejected.rejection_ == expected_rejection);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    std::array<published_wire, 6> wires{};
    const auto head = connection.publish_one({.data_ = true});
    RUVIA_CHECK(head.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    drain_data_only(outbound, wires);
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    drain_all(outbound, wires);
    const auto response = decode_response(wires[0], ruvia::http_known_method::post,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, expected_status);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{1});
    const auto input_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(input_fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_body_rejection_statuses(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await exercise_rejected_body_status(fixture_value, base_generation + 95,
        {.max_buffered_body_bytes_ = 4, .max_live_streams_ = 4, .max_buffered_bytes_in_flight_ = 32},
        connection_type::session::rejection_type::body_too_large, 413, ruvia_ctx);
    co_await exercise_rejected_body_status(fixture_value, base_generation + 96,
        {.max_buffered_body_bytes_ = 1024, .max_live_streams_ = 4, .max_buffered_bytes_in_flight_ = 2},
        connection_type::session::rejection_type::in_flight_body_capacity, 503, ruvia_ctx);
}

ruvia::task<void> exercise_unsupported_connect(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 97;
    const message_id_type id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    auto encoded = ruvia::encode_http3_client_request_head(
        {.method_ = "CONNECT", .authority_ = "localhost:443"}, {}, fixture_value.worker_.resource());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 CONNECT request-head encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded).field_section_.data(), std::get<0>(encoded).field_section_.size()));
    const auto rejected = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK(rejected.rejection_ == connection_type::session::rejection_type::connect_unsupported);
    std::array<published_wire, 6> wires{};
    const auto head = connection.publish_one({.data_ = true});
    RUVIA_CHECK(head.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    drain_data_only(outbound, wires);
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    drain_all(outbound, wires);
    const auto response = decode_response(wires[0], ruvia::http_known_method::connect,
        id.stream_id_, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{501});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    const auto input_fin = connection.accept_control({control_type::kind::stream_fin, id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(input_fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_protocol_error_after_rejected_head(fixture& fixture_value,
    std::uint64_t connection_generation, bool same_block, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    const message_id_type id{epoch, connection_generation, 0};
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto head = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    const auto forbidden = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::settings), {});
    if (!same_block) {
        const auto rejected = accept_wire_bytes(connection, inbound, id,
            std::span<const char>(head.data(), head.size()));
        RUVIA_CHECK(rejected.status_ == connection_type::event_status_type::rejected);
        RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{1});
        RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{1});
    }
    const auto bytes_value = same_block ? head + forbidden : forbidden;
    const auto error = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(bytes_value.data(), bytes_value.size()));
    RUVIA_CHECK(error.status_ == connection_type::event_status_type::protocol_error);
    RUVIA_CHECK(error.input_.protocol_.code_ == ruvia::http3_connection_error_code::frame_unexpected);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
    buffer::borrowed_block output;
    control_type output_control;
    RUVIA_CHECK(!outbound.try_receive(output));
    RUVIA_CHECK(!outbound.try_receive_control(output_control));
    if (!connection.stopped()) {
        RUVIA_CHECK(connection.request_stop());
    }
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_rejection_protocol_error_orders(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    co_await exercise_protocol_error_after_rejected_head(
        fixture_value, base_generation + 98, true, ruvia_ctx);
    co_await exercise_protocol_error_after_rejected_head(
        fixture_value, base_generation + 99, false, ruvia_ctx);
}

ruvia::task<void> exercise_rejection_reset_and_stop(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 93;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 4});
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto wire = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    const message_id_type reset_id{epoch, connection_generation, 0};
    const auto first = accept_wire_bytes(connection, inbound, reset_id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(first.status_ == connection_type::event_status_type::rejected);
    const auto reset = connection.accept_control({.kind_ = control_type::kind::stream_reset,
        .id_ = reset_id,
        .value_ = wire.size()});
    RUVIA_CHECK(reset.status_ == connection_type::event_status_type::stream_cancelled);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::cancelled);

    const auto deferred_request_head = ruvia::encode_http3_client_request_head({.method_ = "GET",
                                                                                   .scheme_ = "https",
                                                                                   .authority_ = "example.test",
                                                                                   .path_ = "/first",
                                                                                   .fields_ = expect_field,
                                                                                   .body_length_ = 4},
        {}, fixture_value.worker_.resource());
    RUVIA_CHECK((deferred_request_head.index() == 0));
    if ((deferred_request_head.index() != 0)) {
        co_return;
    }
    const auto head_wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(deferred_request_head).field_section_.data(), std::get<0>(deferred_request_head).field_section_.size()));
    const auto body_wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), "late");
    const auto pending_wire = head_wire + body_wire;
    const message_id_type deferred_id{epoch, connection_generation, 12};
    const auto deferred_head = accept_wire_bytes(connection, inbound, deferred_id,
        std::span<const char>(head_wire.data(), head_wire.size()));
    RUVIA_CHECK(deferred_head.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{1});
    RUVIA_CHECK(ruvia::detail::http3_server_connection_reset_intent_test_access::enqueue_local(
        connection, deferred_id.stream_id_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(accepted(inbound.try_send(deferred_id, std::as_bytes(
                                                           std::span<const char>(body_wire.data(), body_wire.size())))));
    RUVIA_CHECK(accepted(inbound.try_send_control({.kind_ = control_type::kind::stream_reset,
        .id_ = deferred_id,
        .value_ = pending_wire.size(),
        .stream_reset_error_code_ = ruvia::http3_connection_error_code::request_rejected})));
    control_type deferred_reset;
    RUVIA_CHECK(inbound.try_receive_control(deferred_reset));
    const auto deferred = connection.accept_control(deferred_reset);
    RUVIA_CHECK(deferred.status_ == connection_type::event_status_type::accepted);
    RUVIA_CHECK(deferred.input_.status_ == connection_type::input_type::status_type::deferred_reset);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    buffer::borrowed_block final_data;
    RUVIA_CHECK(inbound.try_receive(final_data));
    const auto cancelled_by_data = connection.accept_data(final_data);
    final_data.release();
    RUVIA_CHECK(cancelled_by_data.status_ == connection_type::event_status_type::stream_cancelled);
    RUVIA_CHECK(cancelled_by_data.input_.status_ == connection_type::input_type::status_type::reset);
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{0});

    const message_id_type published_id{epoch, connection_generation, 4};
    const auto second = accept_wire_bytes(connection, inbound, published_id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(second.status_ == connection_type::event_status_type::rejected);
    const auto headers = connection.publish_one({.data_ = true});
    RUVIA_CHECK(headers.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    const auto fin = connection.publish_one({.control_ = true});
    RUVIA_CHECK(fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    std::array<published_wire, 6> wires{};
    drain_all(outbound, wires);
    const auto response = decode_response(wires[wire_index(4)],
        ruvia::http_known_method::get, 4, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{417});
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{1});
    const auto late_reset = connection.accept_control({.kind_ = control_type::kind::stream_reset,
        .id_ = published_id,
        .value_ = wire.size()});
    RUVIA_CHECK(late_reset.status_ == connection_type::event_status_type::stream_cancelled);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.request_info(4).status_, connection_type::request_status_type::published);

    const message_id_type pending_id{epoch, connection_generation, 8};
    const auto third = accept_wire_bytes(connection, inbound, pending_id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(third.status_ == connection_type::event_status_type::rejected);
    const auto pending_head = connection.publish_one({.data_ = true});
    RUVIA_CHECK(pending_head.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    RUVIA_CHECK(connection.request_stop());
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    drain_all(outbound, wires);
    RUVIA_CHECK(!wires[wire_index(8)].final_wire_bytes_.has_value());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_rejection_index_capacity(fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(fixture_value.services_.worker());
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr std::uint64_t connection_generation = base_generation + 100;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 1});
    constexpr std::array expect_field{ruvia::http3_field_section_field_view{"expect", "custom-expectation"}};
    const auto wire = request_wire(fixture_value.worker_, "GET", "/first", {}, expect_field);
    const message_id_type first_id{epoch, connection_generation, 0};
    const auto first = accept_wire_bytes(connection, inbound, first_id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(first.status_ == connection_type::event_status_type::rejected);
    const auto fin = connection.accept_control({control_type::kind::stream_fin, first_id,
        static_cast<std::uint64_t>(wire.size())});
    RUVIA_CHECK(fin.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.tracked_request_count(), std::size_t{1});
    const message_id_type second_id{epoch, connection_generation, 4};
    const auto second = accept_wire_bytes(connection, inbound, second_id,
        std::span<const char>(wire.data(), wire.size()));
    RUVIA_CHECK(second.connection_close_required_);
    RUVIA_CHECK(connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.tracked_request_count(), std::size_t{1});
    RUVIA_CHECK_EQ(connection.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.ready_request_count(), std::size_t{0});
    const auto close = connection.peek_transport_intent();
    RUVIA_CHECK(close.has_value());
    if (close) {
        RUVIA_CHECK(close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    }
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    (void)ruvia_ctx;
}

ruvia::task<void> exercise_shared_body_budget(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    ruvia::detail::http3_server_body_budget budget(8);
    test_activation_signal first_scheduler(worker_value);
    test_activation_signal second_scheduler(worker_value);
    ruvia::worker_signal slow_started(worker_value);
    fixture_value.routes_.handlers_.slow_started_ = &slow_started;
    buffer first_inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer first_outbound(2, 2, 2, fixture_value.worker_.resource());
    buffer second_inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer second_outbound(2, 2, 2, fixture_value.worker_.resource());
    connection_type first(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, first_outbound, first_scheduler, budget,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 10, .max_tracked_streams_ = 8});
    connection_type second(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, second_outbound, second_scheduler, budget,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 11, .max_tracked_streams_ = 8});

    const auto held = route_request(first, first_inbound, fixture_value.worker_,
        {epoch, base_generation + 10, 0}, "POST", "/slow-body", "123456");
    RUVIA_CHECK(held.status_ == connection_type::event_status_type::dispatched);
    const bool handler_started = co_await wait_for_slow_start(fixture_value, worker_value, fixture_value.worker_stop_);
    RUVIA_CHECK(handler_started);
    require_watchdog_success(ruvia_ctx, handler_started);
    RUVIA_CHECK(fixture_value.routes_.handlers_.body_seen_ == "123456");
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    RUVIA_CHECK_EQ(first.active_request_count(), std::size_t{1});

    const auto over_budget = route_request(second, second_inbound, fixture_value.worker_,
        {epoch, base_generation + 11, 0}, "POST", "/body", "xyz");
    RUVIA_CHECK(over_budget.status_ == connection_type::event_status_type::rejected);
    RUVIA_CHECK(over_budget.rejection_ ==
                connection_type::session::rejection_type::worker_body_budget_exhausted);
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    const auto refusal_head = second.publish_one({.data_ = true});
    RUVIA_CHECK(refusal_head.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::bytes_published);
    const auto refusal_fin = second.publish_one({.control_ = true});
    RUVIA_CHECK(refusal_fin.publication_.status_ ==
                connection_type::dispatch_type::publish_status_type::fin_published);
    std::array<published_wire, 6> refused_wires{};
    drain_all(second_outbound, refused_wires);
    const auto refused_response = decode_response(refused_wires[0],
        ruvia::http_known_method::post, 0, fixture_value.worker_.resource());
    RUVIA_CHECK_EQ(refused_response.status_, std::uint16_t{503});
    RUVIA_CHECK_EQ(second.active_rejection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});

    RUVIA_CHECK(first.request_stop());
    RUVIA_CHECK_EQ(budget.used(), std::size_t{6});
    const auto local_retirement = first.accept_control(
        {control_type::kind::stream_reset, {epoch, base_generation + 10, 0}, 0});
    RUVIA_CHECK(local_retirement.status_ == connection_type::event_status_type::admission_closed);
    RUVIA_CHECK(local_retirement.input_.status_ == connection_type::input_type::status_type::stopped);
    RUVIA_CHECK(local_retirement.input_.status_ != connection_type::input_type::status_type::reset);
    RUVIA_CHECK(local_retirement.connection_close_required_);

    const bool first_tasks_joined =
        co_await wait_for_task_count(first, 0, worker_value, fixture_value.worker_stop_);
    require_watchdog_success(ruvia_ctx, first_tasks_joined);
    RUVIA_CHECK(fixture_value.routes_.handlers_.slow_observed_stop_);
    RUVIA_CHECK_EQ(first.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
    co_await first.join();
    simulate_global_stop_takeover(first);

    RUVIA_CHECK(second.request_stop());
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(second, 0, worker_value, fixture_value.worker_stop_));
    co_await second.join();
    simulate_global_stop_takeover(second);
    RUVIA_CHECK_EQ(second.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
}

ruvia::task<void> exercise_header_limit_connection_close(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto connection_generation = base_generation + 92;
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8});
    const std::string large_value(70 * 1024, 'h');
    const std::array<ruvia::http3_field_section_field_view, 4> fields_value{{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"x-large", large_value}}};
    const auto section = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
    if ((section.index() != 0)) {
        throw std::runtime_error("HTTP/3 oversized-field fixture encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(section).data(), std::get<0>(section).size()));
    const auto failure = accept_wire_bytes(connection, inbound,
        {epoch, connection_generation, 0}, wire);
    RUVIA_CHECK(failure.status_ == connection_type::event_status_type::protocol_error);
    RUVIA_CHECK(failure.input_.status_ == connection_type::input_type::status_type::protocol_error);
    RUVIA_CHECK(failure.input_.protocol_.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(failure.input_.protocol_.code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK(failure.connection_close_required_);
    RUVIA_CHECK(connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    const auto close = connection.peek_transport_intent();
    RUVIA_CHECK(close.has_value());
    if (!close) {
        throw std::runtime_error("HTTP/3 oversized-header close intent was not retained");
    }
    RUVIA_CHECK(close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(close->close_reason_ == connection_type::transport_close_reason_type::connection_protocol_error);
    RUVIA_CHECK(close->connection_error_code_ == ruvia::http3_connection_error_code::excessive_load);
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::unknown);

    simulate_global_stop_takeover(connection);
    co_await connection.join();
}

ruvia::task<void> exercise_input_failure_close_codes(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal capacity_scheduler(worker_value);
    buffer inbound(1, 1, 1, fixture_value.worker_.resource());
    buffer capacity_outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto capacity_generation = base_generation + 93;
    connection_type capacity(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, capacity_outbound, capacity_scheduler,
        {.epoch_ = epoch, .connection_generation_ = capacity_generation, .max_tracked_streams_ = 1});
    constexpr std::array<char, 1> partial_data{0};
    const auto first = accept_wire_bytes(capacity, inbound,
        {epoch, capacity_generation, 0}, partial_data);
    RUVIA_CHECK(first.input_.status_ == connection_type::input_type::status_type::fed);
    const auto exhausted = accept_wire_bytes(capacity, inbound,
        {epoch, capacity_generation, 4}, partial_data);
    RUVIA_CHECK(exhausted.status_ == connection_type::event_status_type::input_rejected);
    RUVIA_CHECK(exhausted.input_.status_ == connection_type::input_type::status_type::capacity_exhausted);
    RUVIA_CHECK(exhausted.input_.protocol_.code_ == ruvia::http3_connection_error_code::no_error);
    const auto capacity_close = capacity.peek_transport_intent();
    RUVIA_CHECK(capacity_close.has_value());
    RUVIA_CHECK(capacity_close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(capacity_close->close_reason_ == connection_type::transport_close_reason_type::input_capacity_exhausted);
    RUVIA_CHECK(capacity_close->connection_error_code_ == ruvia::http3_connection_error_code::excessive_load);
    simulate_global_stop_takeover(capacity);
    co_await capacity.join();

    test_activation_signal final_size_scheduler(worker_value);
    buffer final_size_outbound(1, 1, 1, fixture_value.worker_.resource());
    constexpr auto final_size_generation = base_generation + 94;
    connection_type final_size(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, final_size_outbound, final_size_scheduler,
        {.epoch_ = epoch, .connection_generation_ = final_size_generation, .max_tracked_streams_ = 8});
    const auto fed = accept_wire_bytes(final_size, inbound,
        {epoch, final_size_generation, 0}, partial_data);
    RUVIA_CHECK(fed.input_.status_ == connection_type::input_type::status_type::fed);
    const auto bad_fin = final_size.accept_control({control_type::kind::stream_fin,
        {epoch, final_size_generation, 0}, 0});
    RUVIA_CHECK(bad_fin.status_ == connection_type::event_status_type::input_rejected);
    RUVIA_CHECK(bad_fin.input_.status_ == connection_type::input_type::status_type::final_size_error);
    RUVIA_CHECK(bad_fin.input_.protocol_.code_ == ruvia::http3_connection_error_code::no_error);
    const auto final_size_close = final_size.peek_transport_intent();
    RUVIA_CHECK(final_size_close.has_value());
    RUVIA_CHECK(final_size_close->token_.kind_ == connection_type::transport_intent_kind_type::connection_close);
    RUVIA_CHECK(final_size_close->close_reason_ == connection_type::transport_close_reason_type::final_size_error);
    RUVIA_CHECK(final_size_close->connection_error_code_ == ruvia::http3_connection_error_code::internal_error);
    simulate_global_stop_takeover(final_size);
    co_await final_size.join();
}

ruvia::task<void> exercise_peer_limit_rejection(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::test::counting_memory_resource& upstream,
    ruvia::testing::test_context& ruvia_ctx) {
    test_activation_signal scheduler(worker_value);
    buffer inbound(4, 4, 4, fixture_value.worker_.resource());
    buffer outbound(2, 2, 1, fixture_value.worker_.resource());
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = base_generation + 40, .max_tracked_streams_ = 8});

    const message_id_type queued_control_id{epoch, base_generation + 40, 12};
    RUVIA_CHECK(accepted(outbound.try_send_control(
        {control_type::kind::writable, queued_control_id, 0})));

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
    const message_id_type control_id{epoch, base_generation + 40, 2};
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

    const auto request = route_request(connection, inbound, fixture_value.worker_,
        {epoch, base_generation + 40, 0}, "GET", "/first");
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});
    const auto allocation_count_before_intent = upstream.allocation_count();
    const auto attempt_value = connection.publish_one(all_work_lanes);
    RUVIA_CHECK(attempt_value.status_ == connection_type::publish_status_type::no_ready_request);
    RUVIA_CHECK_EQ(connection.request_info(0).status_, connection_type::request_status_type::failed);
    RUVIA_CHECK(!connection.transport_close_required());
    RUVIA_CHECK_EQ(connection.active_session_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), allocation_count_before_intent);
    RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.handler_calls_, std::size_t{1});

    require_watchdog_success(ruvia_ctx,
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
    RUVIA_CHECK_EQ(connection.active_request_count(), std::size_t{0});
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    const auto reset_intent = connection.peek_transport_intent();
    RUVIA_CHECK(reset_intent.has_value());
    RUVIA_CHECK(reset_intent->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
    RUVIA_CHECK_EQ(reset_intent->token_.id_.stream_id_, std::uint64_t{0});
    RUVIA_CHECK_EQ(reset_intent->token_.id_.epoch_, epoch);
    RUVIA_CHECK_EQ(reset_intent->token_.id_.connection_generation_, base_generation + 40);
    RUVIA_CHECK(reset_intent->stream_reset_error_code_ ==
                ruvia::http3_connection_error_code::request_cancelled);

    const control_type reset_control{.kind_ = control_type::kind::stream_reset,
        .id_ = reset_intent->token_.id_,
        .stream_reset_error_code_ = reset_intent->stream_reset_error_code_};
    RUVIA_CHECK(outbound.try_send_control(reset_control) == buffer::control_result::full);
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
    RUVIA_CHECK(connection.peek_transport_intent()->token_ == reset_intent->token_);

    control_type queued_control;
    RUVIA_CHECK(outbound.try_receive_control(queued_control));
    RUVIA_CHECK(queued_control.kind_ == control_type::kind::writable);
    RUVIA_CHECK(!outbound.try_receive_control(queued_control));
    RUVIA_CHECK(!outbound.has_pending());

    const auto reset_send = outbound.try_send_control(reset_control);
    RUVIA_CHECK(accepted(reset_send));
    RUVIA_CHECK(reset_send == buffer::control_result::sent);
    RUVIA_CHECK(reset_control.value_ == 0);
    const auto allocation_count_before_ack = upstream.allocation_count();
    RUVIA_CHECK(connection.ack_transport_intent(reset_intent->token_));
    RUVIA_CHECK(!connection.ack_transport_intent(reset_intent->token_));
    RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    RUVIA_CHECK(!connection.peek_transport_intent().has_value());
    RUVIA_CHECK_EQ(upstream.allocation_count(), allocation_count_before_ack);

    control_type sent_reset;
    RUVIA_CHECK(outbound.try_receive_control(sent_reset));
    RUVIA_CHECK(sent_reset.kind_ == control_type::kind::stream_reset);
    RUVIA_CHECK(sent_reset.value_ == 0);
    RUVIA_CHECK(!outbound.try_receive_control(sent_reset));
    RUVIA_CHECK(!outbound.has_pending());

    RUVIA_CHECK(connection.request_stop());
    co_await connection.join();
    simulate_global_stop_takeover(connection);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{0});
}

}  // namespace

RUVIA_TEST(http3_server_connection_routes_and_fairly_publishes_buffered_requests) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_successful_connection(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_publishes_rejection_before_request_fin) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_early_rejection_before_request_fin(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_web_socket_rejection_fin_keeps_receive_lifetime_independent) {
    for (const auto retirement : {receive_retirement::fin, receive_retirement::reset, receive_retirement::stop}) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
        const auto worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource upstream;
        {
            fixture fixture(worker_value, upstream);
            run_worker_task(attachment, exercise_websocket_rejection_receive_retirement(fixture, worker_value, retirement, ruvia_ctx));
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    }
}

RUVIA_TEST(http3_server_connection_publishes_minimal_rejection_with_peer_field_limit_zero) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_peer_limit_zero_rejection(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_retires_rejected_reset_and_unfinished_stop) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_rejection_reset_and_stop(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_publishes_body_and_connection_budget_rejections) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_body_rejection_statuses(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_publishes_unsupported_connect_status) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_unsupported_connect(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_prioritizes_protocol_error_after_rejected_head) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_rejection_protocol_error_orders(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_bounds_rejected_stream_index_capacity) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_rejection_index_capacity(fixture, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_keeps_header_limit_as_connection_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_header_limit_connection_close(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_maps_local_input_failures_to_transport_codes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_input_failure_close_codes(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_shares_body_budget_until_stopped_lease_joins) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment, exercise_shared_body_budget(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_server_connection_retires_peer_limit_encoding_rejection) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        run_worker_task(attachment,
            exercise_peer_limit_rejection(fixture, worker_value, upstream, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
