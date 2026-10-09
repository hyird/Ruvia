#include "http3_client_connection_fixture.h"

namespace {

struct observation final {
    bool cold_cancelled_{};
    bool running_cancelled_{};
    bool joined_{};
    bool storage_released_{};
    bool invalid_idle_timeout_rejected_{};
};

ruvia::task<void> exercise(asio::io_context& io, const ruvia::worker_handle& worker_value,
    ruvia::event_loop_attachment& attachment, observation& observed_value,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            try {
                connection_type invalid(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "localhost", .port_ = 49529}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 0ms);
            } catch (const std::invalid_argument&) {
                observed_value.invalid_idle_timeout_rejected_ = true;
            }
            for (const auto invalid : {0ms, -1ms, std::chrono::milliseconds::max()}) {
                const auto baseline = memory.live_bytes_;
                RUVIA_CHECK(ruvia::testing::throws_on([&] {
                    connection_type rejected(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "localhost", .port_ = 49529}),
                        invalid, &memory);
                }));
                RUVIA_CHECK(ruvia::testing::throws_on([&] {
                    connection_type rejected(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "localhost", .port_ = 49529}),
                        2s, &memory, 4, 16 * 1024 * 1024, invalid);
                }));
                RUVIA_CHECK_EQ(memory.live_bytes_, baseline);
            }
            ruvia::detail::http3_client_body_budget receive_body_budget(1024);
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "localhost", .port_ = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024, 30s, &receive_body_budget);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_response_state response_state(worker_value, &memory);
            ruvia::detail::http_client_request_storage bound_request("GET", "/response-state", &memory);
            const auto bound = connection.submit(std::move(bound_request), response_state);
            RUVIA_CHECK(bound.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(response_state.references_ == 2);
            RUVIA_CHECK(response_state.has_http3_body_budget());
            RUVIA_CHECK_EQ(receive_body_budget.used(), std::size_t{0});
            RUVIA_CHECK(response_state.transport_ == ruvia::detail::http_client_response_transport::http3);
            RUVIA_CHECK(response_state.http3_connection_ == &connection);
            bool bound_waiter_done = false;
            auto bound_waiter = [&]() -> ruvia::task<void> {
                co_await connection.wait(bound.id_);
                bound_waiter_done = true;
            };
            tasks.spawn(bound_waiter());
            RUVIA_CHECK(!connection.release_response_request(bound.id_));
            RUVIA_CHECK(co_await ruvia::sleep_for(worker_value, 1ms) == ruvia::timer_sleep_result::elapsed);
            RUVIA_CHECK(!bound_waiter_done);
            const auto retained_failure =
                std::make_exception_ptr(std::runtime_error("preserved response failure"));
            connection.cancel(bound.id_);
            response_state.failure_ = retained_failure;
            RUVIA_CHECK(connection.result(bound.id_) && response_state.complete_);
            RUVIA_CHECK(!connection.release_response_request(bound.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{1});
            RUVIA_CHECK(response_state.references_ == 2);
            RUVIA_CHECK(co_await ruvia::sleep_for(worker_value, 1ms) == ruvia::timer_sleep_result::elapsed);
            RUVIA_CHECK(bound_waiter_done);
            RUVIA_CHECK(connection.release_response_request(bound.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
            RUVIA_CHECK(response_state.references_ == 1);
            RUVIA_CHECK(response_state.transport_ == ruvia::detail::http_client_response_transport::unassigned);
            RUVIA_CHECK(response_state.http3_connection_ == nullptr && response_state.http3_request_id_ == 0);
            RUVIA_CHECK(!response_state.has_http3_body_budget());
            RUVIA_CHECK(response_state.error_code_.has_value());
            RUVIA_CHECK_EQ(*response_state.error_code_,
                static_cast<std::uint8_t>(ruvia::http_client_error::code_type::cancelled));
            RUVIA_CHECK(response_state.complete_);
            RUVIA_CHECK(response_state.failure_ == retained_failure);
            RUVIA_CHECK_EQ(receive_body_budget.used(), std::size_t{0});

            ruvia::detail::http_client_response_state local_state(worker_value, &memory);
            connection_type local_budget_connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "localhost", .port_ = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024);
            const auto local = local_budget_connection.submit(
                ruvia::detail::http_client_request_storage("GET", "/local-budget", &memory),
                local_state);
            RUVIA_CHECK(local.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(!local_budget_connection.release_response_request(local.id_));
            local_state.pending_.assign("retained");
            RUVIA_CHECK(local_state.replace_producer_body_bytes(local_state.pending_.size()));
            local_budget_connection.cancel(local.id_);
            RUVIA_CHECK(local_budget_connection.result(local.id_) && local_state.complete_);
            RUVIA_CHECK(!local_budget_connection.release_response_request(local.id_));
            RUVIA_CHECK_EQ(local_state.pending_, std::string_view("retained"));
            local_state.discard_response_body();
            RUVIA_CHECK(local_budget_connection.release_response_request(local.id_));
            RUVIA_CHECK_EQ(local_state.error_code_.value(),
                static_cast<std::uint8_t>(ruvia::http_client_error::code_type::cancelled));
            RUVIA_CHECK_EQ(local_state.references_, std::size_t{1});
            struct origin_case {
                std::string_view host_;
                std::uint16_t port_;
                std::string_view authority_;
            };
            constexpr std::array origins{
                origin_case{"localhost", 443, "localhost"},
                origin_case{"localhost", 49529, "localhost:49529"},
                origin_case{"[::1]", 443, "[::1]"},
                origin_case{"[::1]", 8443, "[::1]:8443"}};
            for (const auto& item : origins) {
                std::string host(item.host_);
                connection_type selected(io, worker_value, tasks, tls,
                    ruvia::http_origin_view::https({.host_ = host, .port_ = item.port_}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 30s, &receive_body_budget);
                host.assign("modified-after-construction.invalid");
                ruvia::detail::http_client_request_storage matching("GET", "/origin", &memory);
                matching.append_header("Host", item.authority_);
                const auto admitted = selected.submit(std::move(matching));
                RUVIA_CHECK(admitted.outcome_ == connection_type::outcome_type::pending);
                selected.cancel(admitted.id_);
                RUVIA_CHECK(selected.release(admitted.id_));
                ruvia::detail::http_client_request_storage conflicting("GET", "/origin", &memory);
                conflicting.append_header("Host", "different.invalid");
                RUVIA_CHECK(selected.submit(std::move(conflicting)).outcome_ == connection_type::outcome_type::invalid_request);
            }
            RUVIA_CHECK(ruvia::testing::throws_on([&] {
                connection_type plain(io, worker_value, tasks, tls,
                    ruvia::http_origin_view::http({.host_ = "localhost"}), 2s, &memory);
            }));
            const auto before_expired = memory.live_bytes_;
            const auto expired = connection.submit(connection_type::rejected_request_type{
                .request_ = ruvia::detail::http_client_request_storage("POST", "/expired-handoff", &memory),
                .deadline_ = connection_type::time_point_type::min()});
            RUVIA_CHECK(expired.outcome_ == connection_type::outcome_type::deadline);
            RUVIA_CHECK_EQ(connection.retained_requests(), 0U);
            RUVIA_CHECK_EQ(memory.live_bytes_, before_expired);
            ruvia::detail::http_client_request_storage cold("GET", "/cold", &memory);
            const auto first = connection.submit(std::move(cold));
            RUVIA_CHECK(first.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(!connection.take_rejected_request(first.id_));
            connection.cancel(first.id_);
            RUVIA_CHECK(!connection.take_rejected_request(first.id_));
            RUVIA_CHECK(!connection.release_response_request(first.id_));
            RUVIA_CHECK(connection.result(first.id_) &&
                        !connection.result(first.id_)->response_body_plan_);
            observed_value.cold_cancelled_ = connection.result(first.id_) &&
                                             connection.result(first.id_)->outcome_ ==
                                                 connection_type::outcome_type::cancelled;
            RUVIA_CHECK(connection.release(first.id_));
            RUVIA_CHECK(!connection.take_rejected_request(first.id_));

            ruvia::detail::http_client_request_storage active("GET", "/active", &memory);
            ruvia::detail::http_client_request_storage queued("GET", "/queued", &memory);
            const auto second = connection.submit(std::move(active));
            const auto third = connection.submit(std::move(queued));
            RUVIA_CHECK(second.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(third.outcome_ == connection_type::outcome_type::pending);
            connection.start();  // The sole task is now waiting on DNS or UDP.
            connection.request_stop();
            co_await connection.wait(second.id_);
            co_await connection.wait(third.id_);
            observed_value.running_cancelled_ = connection.result(second.id_) &&
                                                connection.result(third.id_) &&
                                                connection.result(second.id_)->outcome_ == connection_type::outcome_type::cancelled &&
                                                connection.result(third.id_)->outcome_ == connection_type::outcome_type::cancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed_value.joined_ = !connection.running();
            RUVIA_CHECK(connection.release(second.id_));
            RUVIA_CHECK(connection.release(third.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), 0U);
        }
        observed_value.storage_released_ = memory.allocations_ == memory.returns_ && memory.live_bytes_ == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_socket_stop(asio::io_context& io, const ruvia::worker_handle& worker_value,
    ruvia::event_loop_attachment& attachment, std::uint16_t port, observation& observed_value,
    ruvia::testing::test_context& ruvia_ctx, std::chrono::milliseconds timeout) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = port}), timeout,
                &memory, 32, 16 * 1024 * 1024, timeout);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_request_storage request("GET", "/blackhole", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            RUVIA_CHECK(co_await ruvia::sleep_for(worker_value, 40ms) ==
                        ruvia::timer_sleep_result::elapsed);
            observed_value.cold_cancelled_ = connection.running() && !connection.result(submitted.id_);
            connection.request_stop();
            co_await connection.wait(submitted.id_);
            observed_value.running_cancelled_ = connection.result(submitted.id_) &&
                                                connection.result(submitted.id_)->outcome_ == connection_type::outcome_type::cancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed_value.joined_ = !connection.running();
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), 0U);
        }
        observed_value.storage_released_ = memory.allocations_ == memory.returns_ && memory.live_bytes_ == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_terminal_timeout(asio::io_context& io, const ruvia::worker_handle& worker_value,
    ruvia::event_loop_attachment& attachment, std::uint16_t port, observation& observed_value,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = port}), 80ms, &memory);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_request_storage request("GET", "/timeout", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            co_await connection.wait(submitted.id_);
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed_value.running_cancelled_ = connection.result(submitted.id_) &&
                                                connection.result(submitted.id_)->outcome_ == connection_type::outcome_type::deadline;
            ruvia::detail::http_client_request_storage later("GET", "/later", &memory);
            observed_value.joined_ = !connection.running() &&
                                     connection.submit(std::move(later)).outcome_ == connection_type::outcome_type::connection_draining;
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), 0U);
            RUVIA_CHECK_EQ(connection.retained_result_body_bytes(), 0U);
        }
        observed_value.storage_released_ = memory.allocations_ == memory.returns_ && memory.live_bytes_ == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_per_request_deadline(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    std::uint16_t port, observation& observed_value, ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls, ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = port}), 2s, &memory);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_request_storage first("GET", "/first", &memory);
            ruvia::detail::http_client_request_storage second("GET", "/short", &memory);
            const auto ongoing = connection.submit(std::move(first));
            const auto started = std::chrono::steady_clock::now();
            const auto short_wait = connection.submit(connection_type::rejected_request_type{
                .request_ = std::move(second), .deadline_ = started + 35ms});
            RUVIA_CHECK(ongoing.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(short_wait.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            co_await connection.wait(short_wait.id_);
            observed_value.cold_cancelled_ = connection.result(short_wait.id_) &&
                                             connection.result(short_wait.id_)->outcome_ == connection_type::outcome_type::deadline &&
                                             !connection.result(ongoing.id_) &&
                                             std::chrono::steady_clock::now() - started < 500ms;
            connection.request_stop();
            co_await connection.wait(ongoing.id_);
            observed_value.running_cancelled_ = connection.result(ongoing.id_) &&
                                                connection.result(ongoing.id_)->outcome_ == connection_type::outcome_type::cancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed_value.joined_ = !connection.running();
            RUVIA_CHECK(connection.release(ongoing.id_));
            RUVIA_CHECK(connection.release(short_wait.id_));
        }
        observed_value.storage_released_ = memory.allocations_ == memory.returns_ && memory.live_bytes_ == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_write_inactivity_timeout(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config{
                .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https(
                    {.host_ = "127.0.0.1", .port_ = peer.port()}),
                5s, &memory, 4, 16 * 1024 * 1024, 30s, nullptr, 120ms);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_request_storage request(
                "POST", "/write-timeout", &memory);
            const std::string body(4 * 1024 * 1024, 'x');
            request.set_body(body);
            const auto started = std::chrono::steady_clock::now();
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            co_await connection.wait(submitted.id_);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const auto result_value = connection.result(submitted.id_);
            RUVIA_CHECK(result_value != nullptr);
            if (result_value != nullptr) {
                RUVIA_CHECK(result_value->outcome_ == connection_type::outcome_type::deadline);
            }
            RUVIA_CHECK(elapsed >= 100ms && elapsed < 3s);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.handshake_observed());
            connection.request_stop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
        }
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
        RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_migration_retirement(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    counting_resource memory;
    std::optional<std::uint64_t> migration_id;
    {
        ruvia::detail::client_transport_config_view tls_config_value{
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config_value);
        ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
        connection_type connection(io, worker_value, tasks, tls,
            ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = peer.port()}), 10s,
            &memory, 4, 16 * 1024 * 1024, 2s);
        ruvia::detail::http_client_request_storage request("GET", "/public-pool", &memory);
        const auto submitted = connection.submit(
            std::move(request), std::chrono::steady_clock::now() + 10s);
        RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
        connection.start();
        peer.allow_final_part();
        co_await connection.wait(submitted.id_);
        const auto completed = connection.result(submitted.id_);
        RUVIA_CHECK(completed != nullptr);
        if (completed != nullptr) {
            RUVIA_CHECK(completed->outcome_ == connection_type::outcome_type::complete);
        }

        asio::ip::udp::socket reservation(
            io, {asio::ip::address_v4::loopback(), 0});
        const auto local_endpoint = reservation.local_endpoint();
        reservation.close();
        const auto migration = connection.start_path_migration(local_endpoint);
        RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::started ||
                    migration.status_ == ruvia::quic_migration_status::validated);
        if (migration.status_ == ruvia::quic_migration_status::started ||
            migration.status_ == ruvia::quic_migration_status::validated) {
            migration_id = migration.id_;
            const bool validated = co_await wait_for_peer(worker_value, peer, [&] {
                const auto status = connection.path_migration(*migration_id);
                return status && status->status_ != ruvia::quic_migration_status::started; }, 8s);
            RUVIA_CHECK(validated);
            const auto status = connection.path_migration(*migration_id);
            RUVIA_CHECK(status.has_value());
            if (status) {
                RUVIA_CHECK(status->status_ == ruvia::quic_migration_status::validated);
            }
        }

        const bool retired = co_await wait_for_peer(
            worker_value, peer, [&] { return !connection.running(); }, 5s);
        RUVIA_CHECK(retired);
        if (migration_id) {
            const auto result_value = connection.path_migration(*migration_id);
            RUVIA_CHECK(result_value.has_value());
            if (result_value) {
                RUVIA_CHECK(result_value->status_ == ruvia::quic_migration_status::validated);
            }
        }
        connection.request_stop();
        co_await tasks.join();
        if (submitted.outcome_ == connection_type::outcome_type::pending) {
            RUVIA_CHECK(connection.release(submitted.id_));
        }
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3_client_connection_retains_validated_migration_after_idle_session_retirement) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_migration_retirement(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_pool_retains_validated_migration_after_session_retirement) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_public_http3_client_pool(io, worker_value, attachment, peer, ruvia_ctx, false, {}, true));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_connection_write_inactivity_timeout_stops_a_flow_controlled_request) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, false, false);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_write_inactivity_timeout(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_connection_cold_cancel_and_started_driver_stop_join_are_worker_owned) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    observation observed;
    auto root = attachment.loop().start(exercise(io, worker_value, attachment, observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.cold_cancelled_ && observed.running_cancelled_ && observed.joined_);
    RUVIA_CHECK(observed.storage_released_);
    RUVIA_CHECK(observed.invalid_idle_timeout_rejected_);
}

RUVIA_TEST(http3_client_connection_request_deadline_does_not_fail_other_pending_requests) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    observation observed;
    auto root = attachment.loop().start(exercise_per_request_deadline(io, worker_value, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.cold_cancelled_ && observed.running_cancelled_ && observed.joined_);
    RUVIA_CHECK(observed.storage_released_);
}

RUVIA_TEST(http3_client_connection_terminal_handshake_timeout_rejects_new_requests) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    observation observed;
    auto root = attachment.loop().start(exercise_terminal_timeout(io, worker_value, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.running_cancelled_ && observed.joined_ && observed.storage_released_);
}

RUVIA_TEST(http3_client_connection_stop_wakes_pending_quic_socket_wait_and_joins_driver) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    observation observed;
    const auto largest_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max());
    auto root = attachment.loop().start(exercise_socket_stop(io, worker_value, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx, largest_timeout));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.cold_cancelled_ && observed.running_cancelled_ && observed.joined_);
    RUVIA_CHECK(observed.storage_released_);
}
