#include <variant>

#include "http3_server_connection_fixture.h"

namespace ruvia::testing {
http3_datagram_receive_status plan_connect_datagram_for_peer(
    detail::http3_connection_identity identity,
    std::optional<detail::http3_stream_control> marker,
    std::uint64_t accepted_wire_bytes, std::span<const char> bytes_value) {
    using access_type = detail::http3_connection_driver_test_access;
    std::pmr::monotonic_buffer_resource resource;
    auto connection = access_type::make_connection(&resource, identity);
    const auto datagram = decode_http3_datagram(bytes_value);
    if ((datagram.index() != 0)) {
        throw std::runtime_error("CONNECT datagram fixture received invalid wire bytes");
    }
    (void)access_type::add_request_stream(connection, std::get<0>(datagram).stream_id_);
    if (marker && access_type::accept_tunnel_established(
                      connection, *marker, accepted_wire_bytes) != access_type::tunnel_result::accepted) {
        throw std::runtime_error("CONNECT datagram fixture received invalid establishment");
    }
    return access_type::plan_received_datagram(connection, std::get<0>(datagram));
}
}  // namespace ruvia::testing

namespace {

connection_type::event_result_type accept_tunnel_head(connection_type& connection, buffer& inbound,
    ruvia::worker_memory& worker_value, message_id_type id) {
    const auto wire = websocket_request_wire(worker_value, "13");
    const auto sent = inbound.try_send(id,
        std::as_bytes(std::span(wire.data(), wire.size())));
    if (!accepted(sent)) {
        throw std::runtime_error("HTTP/3 WebSocket request buffer is full");
    }
    buffer::borrowed_block block;
    if (!inbound.try_receive(block)) {
        throw std::runtime_error("HTTP/3 WebSocket request block is missing");
    }
    auto result_value = connection.accept_data(block);
    block.release();
    return result_value;
}

ruvia::task<bool> wait_for_websocket_start(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        if (fixture_value.routes_.handlers_.websocket_started_observed_) {
            co_return true;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    co_return fixture_value.routes_.handlers_.websocket_started_observed_;
}

ruvia::task<void> exercise_websocket_fin_cancellation(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx,
    bool request_stop_after_fin, bool peer_reset, bool deadline = false) {
    if (deadline) {
        fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 250ms};
    }
    test_activation_signal scheduler(worker_value);
    ruvia::worker_signal started(worker_value);
    fixture_value.routes_.handlers_.websocket_started_observed_ = false;
    fixture_value.routes_.handlers_.websocket_handler_finished_ = false;
    fixture_value.routes_.handlers_.websocket_started_ = &started;
    buffer inbound(8, 8, 8, fixture_value.worker_.resource());
    buffer outbound(8, 8, 8, fixture_value.worker_.resource());
    const auto connection_generation = base_generation + (request_stop_after_fin ? 90 : peer_reset ? 91
                                                                                                   : 92);
    connection_type connection(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
        fixture_value.services_, fixture_value.options_, outbound, scheduler,
        {.epoch_ = epoch, .connection_generation_ = connection_generation, .max_tracked_streams_ = 8, .connection_scanner_ = &fixture_value.scanner_, .executor_ = fixture_value.executor_});
    const message_id_type id{epoch, connection_generation, 0};
    const std::array request_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"}};
    const auto encoded_head = ruvia::encode_http3_field_section(request_fields, fixture_value.worker_.resource());
    RUVIA_CHECK((encoded_head.index() == 0));
    const auto request_head = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded_head).data(), std::get<0>(encoded_head).size()));
    const auto request = accept_tunnel_head(connection, inbound, fixture_value.worker_, id);
    RUVIA_CHECK(request.status_ == connection_type::event_status_type::dispatched);
    for (std::size_t attempt_value = 0;
        attempt_value < 2000 && !fixture_value.routes_.handlers_.websocket_started_observed_; ++attempt_value) {
        if (connection.ready_request_count() != 0) {
            const auto publication = connection.publish_one(all_work_lanes);
            RUVIA_CHECK(publication.status_ == connection_type::publish_status_type::attempted);
            std::array<published_wire, 6> handshake{};
            drain_all(outbound, handshake);
            (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
        } else if (co_await ruvia::sleep_for(worker_value, 1ms, fixture_value.worker_stop_) !=
                   ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    require_watchdog_success(ruvia_ctx,
        co_await wait_for_websocket_start(fixture_value, worker_value, fixture_value.worker_stop_));

    constexpr std::array<char, 8> masked_close{
        static_cast<char>(0x88), static_cast<char>(0x82), char{0x11}, char{0x22},
        char{0x33}, char{0x44}, static_cast<char>(0x12), static_cast<char>(0xca)};
    const auto tunnel_data = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data),
        std::string_view(masked_close.data(), masked_close.size()));
    const auto accepted_close = accept_wire_bytes(connection, inbound, id,
        std::span<const char>(tunnel_data.data(), tunnel_data.size()));
    RUVIA_CHECK(accepted_close.status_ == connection_type::event_status_type::accepted);
    bool local_fin_published = false;
    std::array<published_wire, 6> tunnel_wire{};
    for (std::size_t attempt_value = 0; attempt_value < 2000 && !local_fin_published; ++attempt_value) {
        if (connection.ready_request_count() == 0) {
            if (co_await ruvia::sleep_for(worker_value, 1ms, fixture_value.worker_stop_) !=
                ruvia::timer_sleep_result::elapsed) {
                break;
            }
            continue;
        }
        const auto publication = connection.publish_one(all_work_lanes);
        if (publication.status_ == connection_type::publish_status_type::no_ready_request) {
            (void)co_await ruvia::sleep_for(worker_value, 1ms, fixture_value.worker_stop_);
            continue;
        }
        if (publication.status_ != connection_type::publish_status_type::attempted) {
            break;
        }
        local_fin_published =
            publication.publication_.status_ == connection_type::dispatch_type::publish_status_type::fin_published ||
            publication.publication_.status_ == connection_type::dispatch_type::publish_status_type::complete;
        drain_all(outbound, tunnel_wire);
        (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
    }
    if (!local_fin_published) {
        RUVIA_CHECK(connection.request_stop());
        require_watchdog_success(ruvia_ctx,
            co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_));
        co_await connection.join();
        simulate_global_stop_takeover(connection);
        RUVIA_CHECK(local_fin_published);
        co_return;
    }
    RUVIA_CHECK(local_fin_published);
    RUVIA_CHECK(connection.request_info(id.stream_id_).status_ == connection_type::request_status_type::published);
    RUVIA_CHECK_EQ(connection.active_task_count(), std::size_t{1});

    if (request_stop_after_fin) {
        RUVIA_CHECK(connection.request_stop());
    } else if (peer_reset) {
        const auto reset = connection.accept_control({control_type::kind::stream_reset, id,
            static_cast<std::uint64_t>(tunnel_data.size() + request_head.size())});
        RUVIA_CHECK(reset.status_ == connection_type::event_status_type::stream_cancelled);
    } else if (!deadline) {
        const auto peer_fin = connection.accept_control({control_type::kind::stream_fin, id,
            static_cast<std::uint64_t>(tunnel_data.size() + request_head.size())});
        RUVIA_CHECK(peer_fin.status_ == connection_type::event_status_type::accepted);
        RUVIA_CHECK(peer_fin.input_.status_ == connection_type::input_type::status_type::finished);
    }
    const bool joined =
        co_await wait_for_task_count(connection, 0, worker_value, fixture_value.worker_stop_);
    require_watchdog_success(ruvia_ctx, joined);
    if (deadline) {
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
        const auto retirement = connection.peek_transport_intent();
        RUVIA_CHECK(retirement.has_value());
        if (!retirement) {
            throw std::runtime_error("deadline after FIN did not retain stream retirement intent");
        }
        RUVIA_CHECK(retirement->token_.kind_ == connection_type::transport_intent_kind_type::stream_reset);
        RUVIA_CHECK_EQ(retirement->token_.id_.stream_id_, id.stream_id_);
    } else if (peer_reset) {
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    }
    if (deadline) {
        const auto late_fin = connection.accept_control({control_type::kind::stream_fin, id,
            static_cast<std::uint64_t>(request_head.size() + tunnel_data.size())});
        RUVIA_CHECK(late_fin.input_.status_ == connection_type::input_type::status_type::closed_stream);
        std::array<published_wire, 6> discarded{};
        drain_all(outbound, discarded);
        const message_id_type sibling_id{epoch, connection_generation, 4};
        const auto sibling = route_request(connection, inbound, fixture_value.worker_, sibling_id,
            "GET", "/first");
        RUVIA_CHECK(sibling.status_ == connection_type::event_status_type::dispatched);
        co_await wait_for_ready(connection, 1, worker_value, fixture_value.worker_stop_);
        bool sibling_published = false;
        for (std::size_t attempt_value = 0; attempt_value < 32 && !sibling_published; ++attempt_value) {
            const auto publication = connection.publish_one(all_work_lanes);
            RUVIA_CHECK(publication.status_ == connection_type::publish_status_type::attempted);
            if (publication.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::fin_published ||
                publication.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::complete) {
                sibling_published = true;
            }
            drain_all(outbound, discarded);
            (void)connection.reactivate_blocked({.data_ = true, .control_ = true});
        }
        RUVIA_CHECK(sibling_published);
        RUVIA_CHECK(connection.request_info(4).status_ == connection_type::request_status_type::published);
        const auto retirement = connection.peek_transport_intent();
        RUVIA_CHECK(retirement.has_value());
        if (!retirement || retirement->token_.kind_ != connection_type::transport_intent_kind_type::stream_reset ||
            retirement->token_.id_.stream_id_ != id.stream_id_) {
            throw std::runtime_error("deadline reset intent changed after sibling publication");
        }
        RUVIA_CHECK(connection.ack_transport_intent(retirement->token_));
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{0});
    }
    if (request_stop_after_fin) {
        RUVIA_CHECK_EQ(connection.pending_transport_intent_count(), std::size_t{1});
        RUVIA_CHECK(connection.peek_transport_intent()->token_.kind_ ==
                    connection_type::transport_intent_kind_type::connection_close);
    }
    if (!connection.stopped()) {
        RUVIA_CHECK(connection.request_stop());
    }
    co_await connection.join();
    std::array<published_wire, 6> wires{};
    drain_all(outbound, wires);
    simulate_global_stop_takeover(connection);
}

ruvia::task<void> exercise_websocket_fin_cancellation_variants(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.scanner_.start();
    co_await exercise_websocket_fin_cancellation(fixture_value, worker_value, ruvia_ctx, false, false);
    co_await exercise_websocket_fin_cancellation(fixture_value, worker_value, ruvia_ctx, false, true);
    co_await exercise_websocket_fin_cancellation(fixture_value, worker_value, ruvia_ctx, true, false);
    co_await exercise_websocket_fin_cancellation(fixture_value, worker_value, ruvia_ctx, false, false, true);
    fixture_value.scanner_.stop();
}

}  // namespace

RUVIA_TEST(http3_worker_tunnel_body_timeout_waits_for_accepted_handshake_barrier) {
    using access_type = ruvia::detail::http3_connection_driver_test_access;
    using identity_type = ruvia::detail::http3_connection_identity;
    using control_type = ruvia::detail::http3_stream_control;
    std::pmr::monotonic_buffer_resource resource;
    const identity_type identity{
        .epoch_ = 7, .connection_generation_ = 11};
    auto make_control = [](std::uint64_t stream_id) {
        return control_type{
            .kind_ = control_type::kind::tunnel_established,
            .id_ = {.epoch_ = 7, .connection_generation_ = 11, .stream_id_ = stream_id},
            .value_ = 100};
    };

    auto fin_first = access_type::make_connection(&resource, identity);
    auto& fin_stream = access_type::add_request_stream(fin_first, 4);
    auto& fin_sibling = access_type::add_request_stream(fin_first, 8);
    access_type::note_peer_fin(fin_first, 4);
    access_type::complete_input_terminal(fin_first, 4);
    RUVIA_CHECK(fin_stream.input_terminal_);
    RUVIA_CHECK(!fin_stream.body_timeout_applies());
    RUVIA_CHECK(fin_sibling.body_timeout_applies());
    RUVIA_CHECK(access_type::accept_tunnel_established(fin_first, make_control(4), 99) ==
                access_type::tunnel_result::accepted);
    RUVIA_CHECK_EQ(access_type::pending_handshakes(fin_first), std::size_t{1});
    RUVIA_CHECK(!fin_stream.body_timeout_applies());
    RUVIA_CHECK(fin_sibling.body_timeout_applies());
    RUVIA_CHECK(!access_type::confirm_tunnel_established(fin_first, 4, 99));
    RUVIA_CHECK(access_type::confirm_tunnel_established(fin_first, 4, 100));
    RUVIA_CHECK_EQ(access_type::pending_handshakes(fin_first), std::size_t{0});
    RUVIA_CHECK(access_type::accept_tunnel_established(fin_first, make_control(4), 100) ==
                access_type::tunnel_result::protocol_failure);
    auto wrong_identity = make_control(8);
    ++wrong_identity.id_.epoch_;
    RUVIA_CHECK(access_type::accept_tunnel_established(fin_first, wrong_identity, 100) ==
                access_type::tunnel_result::protocol_failure);
    RUVIA_CHECK(fin_sibling.body_timeout_applies());

    auto marker_first = access_type::make_connection(&resource, identity);
    auto& marker_stream = access_type::add_request_stream(marker_first, 12);
    auto& marker_sibling = access_type::add_request_stream(marker_first, 16);
    RUVIA_CHECK(access_type::accept_tunnel_established(marker_first, make_control(12), 99) ==
                access_type::tunnel_result::accepted);
    RUVIA_CHECK_EQ(access_type::pending_handshakes(marker_first), std::size_t{1});
    access_type::note_peer_fin(marker_first, 12);
    access_type::complete_input_terminal(marker_first, 12);
    RUVIA_CHECK(marker_stream.input_terminal_);
    RUVIA_CHECK(!marker_stream.body_timeout_applies());
    RUVIA_CHECK(marker_sibling.body_timeout_applies());
    RUVIA_CHECK(access_type::confirm_tunnel_established(marker_first, 12, 100));
    RUVIA_CHECK_EQ(access_type::pending_handshakes(marker_first), std::size_t{0});

    ruvia::test::counting_memory_resource reset_resource;
    {
        auto reset_connection = access_type::make_connection(&reset_resource, identity);
        auto& reset_stream = access_type::add_request_stream(reset_connection, 20);
        auto& reset_sibling = access_type::add_request_stream(reset_connection, 24);
        access_type::install_frame_tracker(reset_stream, &reset_resource);
        RUVIA_CHECK(access_type::accept_tunnel_established(reset_connection, make_control(20), 99) ==
                    access_type::tunnel_result::accepted);
        RUVIA_CHECK_EQ(access_type::pending_handshakes(reset_connection), std::size_t{1});
        const auto deallocations_before_reset = reset_resource.deallocation_count();
        const auto live_allocations_before_reset = reset_resource.live_allocations();
        access_type::note_input_reset(reset_connection, 20);
        access_type::complete_input_terminal(reset_connection, 20);
        RUVIA_CHECK(reset_stream.input_terminal_);
        RUVIA_CHECK(reset_stream.input_reset_);
        RUVIA_CHECK(!reset_stream.frame_tracker_);
        // The tracker can own more than one PMR allocation on MSVC; verify
        // actual release rather than assuming its object is the only block.
        RUVIA_CHECK(reset_resource.deallocation_count() > deallocations_before_reset);
        RUVIA_CHECK(reset_resource.live_allocations() < live_allocations_before_reset);
        RUVIA_CHECK(!reset_stream.body_timeout_applies());
        RUVIA_CHECK_EQ(access_type::pending_handshakes(reset_connection), std::size_t{0});
        RUVIA_CHECK(access_type::accept_tunnel_established(
                        reset_connection, make_control(20), 100) ==
                    access_type::tunnel_result::ignored_terminal);
        RUVIA_CHECK_EQ(access_type::pending_handshakes(reset_connection), std::size_t{0});
        RUVIA_CHECK(!access_type::closing(reset_connection));
        RUVIA_CHECK(access_type::accept_tunnel_established(
                        reset_connection, make_control(20), 100) ==
                    access_type::tunnel_result::ignored_terminal);
        RUVIA_CHECK_EQ(access_type::pending_handshakes(reset_connection), std::size_t{0});
        RUVIA_CHECK(!access_type::closing(reset_connection));
        auto stale_identity = make_control(20);
        ++stale_identity.id_.connection_generation_;
        RUVIA_CHECK(access_type::accept_tunnel_established(
                        reset_connection, stale_identity, 100) ==
                    access_type::tunnel_result::protocol_failure);
        RUVIA_CHECK(reset_sibling.body_timeout_applies());
        RUVIA_CHECK_EQ(access_type::identity(reset_connection).connection_generation_,
            identity.connection_generation_);
    }
    RUVIA_CHECK_EQ(reset_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(reset_resource.allocation_count(), reset_resource.deallocation_count());
}

RUVIA_TEST(http3_server_connection_waits_for_tunnel_peer_fin_after_local_output_fin) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment,
            exercise_websocket_fin_cancellation_variants(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
