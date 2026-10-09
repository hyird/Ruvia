#include <variant>

#include "http3_client_connection_fixture.h"

namespace {

using connection_test_access = ruvia::detail::http3_client_connection_test_access;

ruvia::detail::http_client_response_state* reject_early_test_push(void*, std::size_t,
    connection_type&, std::uint64_t, const ruvia::http3_message_head&) {
    return nullptr;
}

void finish_early_test_push(void*) noexcept {}

ruvia::task<void> exercise_early_data_rejection_recovery(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& ticket_peer, local_quic_response_peer& accepting_peer,
    local_quic_response_peer& rejecting_peer, ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        ruvia::detail::client_transport_config_view tls_config_value{
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config_value);
        std::optional<ruvia::detail::http3_quic_client_tls_context::ticket_lease> ticket;
        {
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = ticket_peer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::http_client_request_storage request("GET", "/ticket", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            ticket_peer.allow_final_part();
            co_await connection.wait(submitted.id_);
            const auto completed = connection.result(submitted.id_);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome_ == connection_type::outcome_type::complete);
            }

            const bool ticket_ready = co_await wait_for_peer(worker_value, ticket_peer, [&] {
                if (!ticket) {
                    auto captured_value = tls.take_ticket("127.0.0.1", ruvia::quic_version::v1);
                    if (captured_value) {
                        ticket.emplace(std::move(*captured_value));
                    }
                }
                return ticket.has_value(); }, 8s);
            RUVIA_CHECK(ticket_ready);
            if (ticket) {
                RUVIA_CHECK(SSL_SESSION_get_max_early_data(ticket->session_.get()) != 0);
                ruvia::detail::http3_quic_client_tls_context no_early_tls(tls_config_value);
                ruvia::detail::http3_quic_client_tls_context::ssl_session_owner no_early_session(
                    SSL_SESSION_dup(ticket->session_.get()));
                RUVIA_CHECK(no_early_session != nullptr);
                if (no_early_session) {
                    SSL_SESSION_set_max_early_data(no_early_session.get(), 0);
                    no_early_tls.remember_ticket(no_early_session.get(), ticket->host_,
                        ticket->version_, ticket->transport_parameters_, ticket->settings_);
                    asio::ip::udp::socket reservation(io,
                        {asio::ip::address_v4::loopback(), 0});
                    const auto local = ruvia::detail::to_http3_quic_datagram_address(
                        reservation.local_endpoint());
                    const auto peer = ruvia::detail::to_http3_quic_datagram_address(
                        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), ticket_peer.port()));
                    RUVIA_CHECK((local.index() == 0) && (peer.index() == 0));
                    if ((local.index() == 0) && (peer.index() == 0)) {
                        ruvia::quic_connection_config quic_config;
                        quic_config.local_address_ = ruvia::detail::to_quic_address(std::get<0>(local));
                        quic_config.peer_address_ = ruvia::detail::to_quic_address(std::get<0>(peer));
                        ruvia::detail::http3_quic_client_transport transport(
                            no_early_tls, quic_config, "127.0.0.1",
                            std::chrono::steady_clock::now(), &memory, true);
                        RUVIA_CHECK(!transport.early_data_enabled());
                    }
                }
                tls.remember_ticket(ticket->session_.get(), ticket->host_, ticket->version_,
                    ticket->transport_parameters_, ticket->settings_);
            }
            connection.request_stop();
            co_await tasks.join();
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
        }
        // Reuse the original server TLS context so this peer accepts the real
        // ticket while its handshake is gated after the early-data decision.
        if (ticket) {
            tls.remember_ticket(ticket->session_.get(), ticket->host_, ticket->version_,
                ticket->transport_parameters_, ticket->settings_);
        }
        {
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = accepting_peer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::http_client_request_storage request("GET", "/accepted-early", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();

            const bool handshake_paused = co_await wait_for_peer(worker_value, accepting_peer, [&] { return accepting_peer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker_value, accepting_peer, [&] { return connection_test_access::early_request_finished(connection, submitted.id_); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id_,
                {.urgency_ = 1, .incremental_ = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id_));

            accepting_peer.allow_handshake();
            const bool priority_observed = co_await wait_for_peer(worker_value, accepting_peer, [&] {
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
                const auto element_id = accepting_peer.priority_element_id();
                return accepting_peer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(priority_observed);
            RUVIA_CHECK(accepting_peer.handshake_observed());
            RUVIA_CHECK(accepting_peer.early_data_accepted());
            const auto accepted_stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
            const auto accepted_priority_id = accepting_peer.priority_element_id();
            RUVIA_CHECK(accepted_stream_id.has_value() && accepted_priority_id == accepted_stream_id);

            accepting_peer.allow_final_part();
            co_await connection.wait(submitted.id_);
            const auto completed = connection.result(submitted.id_);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome_ == connection_type::outcome_type::complete);
                RUVIA_CHECK_EQ(completed->status_, std::uint16_t{200});
            }
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            connection.request_stop();
            co_await tasks.join();
        }

        // The second peer has independent ticket keys and must reject the same
        // valid ticket after the client has completely written its 0-RTT request.
        if (ticket) {
            tls.remember_ticket(ticket->session_.get(), ticket->host_, ticket->version_,
                ticket->transport_parameters_, ticket->settings_);
        }
        {
            ruvia::detail::http3_client_body_budget push_budget(1024 * 1024);
            int push_observer_context{};
            const ruvia::detail::http3_client_push_observer push_observer{
                .context_ = &push_observer_context,
                .config_ = {.enabled_ = true, .max_concurrent_pushes_ = 1},
                .receive_ = reject_early_test_push,
                .finished_ = finish_early_test_push};
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = rejecting_peer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, &push_budget, 30s, {}, {}, {},
                push_observer, ruvia::quic_version::v1, true);
            ruvia::detail::http_client_request_storage request("GET", "/recover", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();
            const bool handshake_paused = co_await wait_for_peer(worker_value, rejecting_peer, [&] { return rejecting_peer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker_value, rejecting_peer, [&] { return connection_test_access::early_request_finished(connection, submitted.id_); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id_,
                {.urgency_ = 1, .incremental_ = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id_));

            rejecting_peer.allow_handshake();
            const bool peer_control_ready = co_await wait_for_peer(worker_value, rejecting_peer, [&] {
                const auto max_push_id = rejecting_peer.peer_max_push_id();
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
                const auto element_id = rejecting_peer.priority_element_id();
                return max_push_id == std::optional<std::uint64_t>{0} &&
                       rejecting_peer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(peer_control_ready);
            const auto replayed_stream_id = connection_test_access::request_stream_id(connection, submitted.id_);
            const auto priority_element_id = rejecting_peer.priority_element_id();
            RUVIA_CHECK(replayed_stream_id.has_value() && priority_element_id == replayed_stream_id);
            RUVIA_CHECK(rejecting_peer.early_data_rejected());
            rejecting_peer.allow_final_part();
            co_await connection.wait(submitted.id_);
            const auto completed = connection.result(submitted.id_);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome_ == connection_type::outcome_type::complete);
                RUVIA_CHECK_EQ(completed->status_, std::uint16_t{200});
            }
            RUVIA_CHECK(rejecting_peer.handshake_observed());
            RUVIA_CHECK(rejecting_peer.early_data_rejected());
            RUVIA_CHECK(rejecting_peer.request_payload_bytes() != 0);
            RUVIA_CHECK(connection.release(submitted.id_));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            RUVIA_CHECK(connection.accepting());

            ruvia::detail::http_client_request_storage cancelled_request("GET", "/cancel", &memory);
            cancelled_request.set_replay_safe(true);
            const auto cancelled = connection.submit(std::move(cancelled_request),
                std::chrono::steady_clock::now() + 5s);
            RUVIA_CHECK(cancelled.outcome_ == connection_type::outcome_type::pending);
            const bool cancel_stream_opened = co_await wait_for_peer(worker_value, rejecting_peer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(cancel_stream_opened);
            connection.cancel(cancelled.id_);
            co_await connection.wait(cancelled.id_);
            const auto cancelled_result = connection.result(cancelled.id_);
            RUVIA_CHECK(cancelled_result != nullptr &&
                        cancelled_result->outcome_ == connection_type::outcome_type::cancelled);
            RUVIA_CHECK(connection.release(cancelled.id_));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            ruvia::detail::http_client_request_storage timed_request("GET", "/timeout", &memory);
            timed_request.set_replay_safe(true);
            const auto timed = connection.submit(std::move(timed_request),
                std::chrono::steady_clock::now() + 500ms);
            RUVIA_CHECK(timed.outcome_ == connection_type::outcome_type::pending);
            const bool timeout_stream_opened = co_await wait_for_peer(worker_value, rejecting_peer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(timeout_stream_opened);
            co_await connection.wait(timed.id_);
            const auto timed_result = connection.result(timed.id_);
            RUVIA_CHECK(timed_result != nullptr &&
                        timed_result->outcome_ == connection_type::outcome_type::deadline);
            RUVIA_CHECK(connection.release(timed.id_));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            connection.request_stop();
            co_await tasks.join();
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

}  // namespace

RUVIA_TEST(http3_client_replays_bodyless_get_after_rejected_early_data_on_rebuilt_critical_streams) {
    test_identity_files identity;
    auto server_tls_config = ruvia::detail::http_server_listener_definition::tls_type{};
    server_tls_config.identity_.certificate_chain_file_ = identity.certificate().string();
    server_tls_config.identity_.private_key_file_ = identity.private_key().string();
    server_tls_config.http3_early_data_ = true;
    ruvia::detail::http3_quic_tls_context shared_server_tls(
        server_tls_config, std::pmr::get_default_resource());
    local_quic_response_peer ticket_peer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls);
    local_quic_response_peer accepting_peer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls, true);
    // This independent server TLS context must reject the otherwise valid ticket.
    local_quic_response_peer rejecting_peer(identity, false, true, false, false, {}, false, false,
        true, nullptr, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(exercise_early_data_rejection_recovery(
        io, worker_value, attachment, ticket_peer, accepting_peer, rejecting_peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_observes_origin_frame_over_authenticated_quic_and_retains_after_shutdown) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, false, true, false, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    const auto ca_file = identity.certificate().string();
    auto root = attachment.loop().start(
        exercise_public_http3_client_pool(io, worker_value, attachment, peer, ruvia_ctx, true, ca_file));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_upload_exchange_wakes_quic_driver_and_waits_for_continue_without_write_timeout) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connect_timeout_ = 10s,
                                                         .write_timeout_ = 100ms,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification});
        std::exception_ptr failure;
        try {
            auto exchange_value = co_await client.open_request({.method_ = "POST", .target_ = "/upload"},
                {.content_length_ = 6, .expectation_ = ruvia::http_client_request_expectation::continue_value, .continue_timeout_ = 200ms});
            co_await exchange_value.body().write("abc");
            // Waiting for an application producer does not consume write inactivity.
            (void)co_await ruvia::sleep_for(worker_value, 150ms);
            co_await exchange_value.body().write("def");
            const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
            co_await exchange_value.body().end(trailers);
            RUVIA_CHECK(exchange_value.body().complete());
            auto response = co_await exchange_value.response();
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto first = co_await response.body().text();
            RUVIA_CHECK(first && *first == "abc");
            peer.allow_final_part();
            auto rest = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(rest.bytes().data()), rest.size()), "def");
            RUVIA_CHECK(peer.request_payload_bytes() > 6);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3_client_push_owns_promises_reclaims_repeated_streams_and_retains_results_after_shutdown) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, false, true, false, false, quic_push_scenario{.count_ = 100, .stream_before_promise_ = true});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    counting_resource memory;
    auto task_value = [&]() -> ruvia::task<void> {
        std::exception_ptr failure;
        std::optional<ruvia::http_client_push> saved;
        std::optional<ruvia::http_client_response> saved_response;
        {
            ruvia::http_client_config config{.scheme_ = ruvia::http_scheme::https, .host_ = "127.0.0.1", .port_ = peer.port(), .connection_count_ = 1, .connect_timeout_ = 5s, .request_timeout_ = 10s, .max_response_bytes_ = 2 * 1024 * 1024, .protocol_ = ruvia::http_client_protocol::http3_only, .push_ = {.enabled_ = true, .max_concurrent_pushes_ = 1, .timeout_ = 5s}, .ca_file_ = identity.certificate().string()};
            ruvia::detail::http_client_pool client(io, worker_value, ruvia::detail::http_client_config_storage(config, &memory), ruvia::http_client_result_budget_config{}, &memory);
            std::exception_ptr operation_failure;
            try {
                auto parent_value = co_await client.execute(ruvia::detail::http_client_request_storage("GET", "/parent", &memory), {});
                for (std::size_t index = 0; index < 100; ++index) {
                    std::optional<ruvia::http_client_push> push;
                    const auto available = co_await wait_for_peer(worker_value, peer, [&] { push = client.next_push(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("HTTP/3 push did not arrive");
                    }
                    RUVIA_CHECK(std::string_view(push->request().path_) == "/push/" + std::to_string(index));
                    RUVIA_CHECK(push->request().headers_[0].name() == "x-promise");
                    {
                        auto cold = push->response();
                    }
                    auto pending = push->response();
                    bool busy = false;
                    try {
                        (void)push->response();
                    } catch (const std::logic_error&) {
                        busy = true;
                    }
                    RUVIA_CHECK(busy);
                    auto moved = std::move(*push);
                    auto response = co_await std::move(pending);
                    RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
                    auto bytes_value = co_await response.body().read_all(32);
                    RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size()) == "ppppp");
                    if (index == 0) {
                        saved.emplace(std::move(moved));
                        saved_response.emplace(std::move(response));
                    }
                    RUVIA_CHECK(saved->request().path_ == "/push/0");
                }
                peer.allow_final_part();
                auto bytes_value = co_await parent_value.body().read_all(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size()) == "abcdef");
                RUVIA_CHECK(client.stats().received_pushes_ == 100 && client.stats().rejected_pushes_ == 0);
            } catch (...) {
                operation_failure = std::current_exception();
            }
            client.close_now();
            co_await client.join();
            failure = operation_failure;
        }
        if (saved && saved_response) {
            RUVIA_CHECK(saved->request().path_ == "/push/0");
            RUVIA_CHECK(saved_response->status().value() == 200 && saved_response->body().complete());
        }
        saved_response.reset();
        saved.reset();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(task_value());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
    RUVIA_CHECK(memory.live_bytes_ == 0 && memory.allocations_ == memory.returns_);
}

RUVIA_TEST(http3_client_push_bounds_origin_validation_errors_and_cancellation_preserve_parent) {
    test_identity_files identity;
    const std::array scenarios{
        quic_push_scenario{.count_ = 2, .body_bytes_ = 1024 * 1024},
        quic_push_scenario{.promise_only_ = true},
        quic_push_scenario{.cancel_before_promise_ = true},
        quic_push_scenario{.malformed_response_ = true},
        quic_push_scenario{.cross_origin_ = true},
    };
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        local_quic_response_peer peer(identity, false, true, false, false, scenarios[index]);
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto worker_value = attachment.loop().handle();
        auto task_value = [&]() -> ruvia::task<void> {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "127.0.0.1", .port_ = peer.port(), .connection_count_ = 1, .connect_timeout_ = 5s, .request_timeout_ = 10s, .max_response_bytes_ = 2 * 1024 * 1024, .protocol_ = ruvia::http_client_protocol::http3_only, .push_ = {.enabled_ = true, .max_queued_pushes_ = 1, .max_concurrent_pushes_ = 2, .timeout_ = index == 1 ? 100ms : 5s}, .ca_file_ = identity.certificate().string()});
            std::exception_ptr operation_failure;
            try {
                auto parent_value = co_await client.send({.method_ = "GET", .target_ = "/parent"});
                std::optional<ruvia::http_client_push> push;
                if (index == 0) {
                    RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().rejected_pushes_ == 1; }, 2s));
                }
                if (index != 2 && index != 4) {
                    const auto available = co_await wait_for_peer(worker_value, peer, [&] { push = client.next_push(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("expected HTTP/3 promise did not arrive");
                    }
                    if (index == 0) {
                        push.reset();
                        RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return peer.cancelled_pushes() >= 1; }, 2s));
                        RUVIA_CHECK(client.stats().rejected_pushes_ == 1);
                    } else {
                        bool failed = false;
                        try {
                            auto response = co_await push->response();
                            (void)co_await response.body().read_all(32);
                        } catch (const ruvia::http_client_error& error) {
                            failed = error.code() == (index == 1 ? ruvia::http_client_error::code_type::timeout : ruvia::http_client_error::code_type::protocol_error);
                        }
                        RUVIA_CHECK(failed);
                    }
                } else {
                    RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return peer.promised_pushes() == 1; }, 2s));
                    if (index == 4) {
                        RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().rejected_pushes_ == 1; }, 2s));
                    } else {
                        (void)co_await ruvia::sleep_for(worker_value, 50ms);
                    }
                    auto cancelled_push = client.next_push();
                    if (index == 2) {
                        // QUIC does not order the control and request streams.
                        // A promise may arrive before its cancellation, but must
                        // never expose a usable response after cancellation.
                        if (cancelled_push) {
                            bool cancelled{};
                            try {
                                (void)co_await cancelled_push->response();
                            } catch (const ruvia::http_client_error& error) {
                                cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
                            }
                            RUVIA_CHECK(cancelled);
                        }
                        RUVIA_CHECK(client.stats().received_pushes_ <= 1);
                    } else {
                        RUVIA_CHECK(!cancelled_push);
                        RUVIA_CHECK(client.stats().received_pushes_ == 0);
                        RUVIA_CHECK(client.stats().rejected_pushes_ == 1);
                    }
                }
                peer.allow_final_part();
                auto bytes_value = co_await parent_value.body().read_all(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size()) == "abcdef");
            } catch (...) {
                operation_failure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            attachment.stop();
            if (operation_failure) {
                std::rethrow_exception(operation_failure);
            }
        };
        auto root = attachment.loop().start(task_value());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}

RUVIA_TEST(http3_client_push_streaming_priority_disabled_permission_and_shutdown_are_worker_owned) {
    test_identity_files identity;
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        local_quic_response_peer peer(identity, false, true, false, false,
            quic_push_scenario{.body_bytes_ = 1280 * 1024, .promise_only_ = scenario == 1});
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto worker_value = attachment.loop().handle();
        auto task_value = [&]() -> ruvia::task<void> {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "127.0.0.1", .port_ = peer.port(), .connection_count_ = 1, .connect_timeout_ = 5s, .request_timeout_ = 10s, .max_response_bytes_ = 2 * 1024 * 1024, .protocol_ = ruvia::http_client_protocol::http3_only, .push_ = {.enabled_ = scenario != 2, .max_concurrent_pushes_ = 1, .timeout_ = std::nullopt}, .ca_file_ = identity.certificate().string()});
            std::exception_ptr failure;
            std::optional<ruvia::http_client_push> retained;
            try {
                auto parent_value = co_await client.send({.method_ = "GET", .target_ = "/parent"});
                if (scenario != 2) {
                    RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { retained = client.next_push(); return retained.has_value(); }, 2s));
                    if (!retained) {
                        throw std::runtime_error("expected push missing");
                    }
                    if (scenario == 0) {
                        auto response = co_await retained->response();
                        response.reprioritize({.urgency_ = 1, .incremental_ = true});
                        RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return peer.push_priority_observed() == 0x101; }, 2s));
                        std::size_t bytes_value{};
                        while (auto part = co_await response.body().text()) {
                            RUVIA_CHECK(part->find_first_not_of('p') == std::string_view::npos);
                            bytes_value += part->size();
                        }
                        RUVIA_CHECK(bytes_value == 1280 * 1024);
                    }
                } else {
                    RUVIA_CHECK(!client.next_push());
                    RUVIA_CHECK(client.stats().received_pushes_ == 0 && peer.promised_pushes() == 0);
                }
                peer.allow_final_part();
                auto bytes_value = co_await parent_value.body().read_all(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size()) == "abcdef");
            } catch (...) {
                failure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            if (scenario == 1 && retained) {
                bool cancelled = false;
                try {
                    (void)co_await retained->response();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
                }
                RUVIA_CHECK(cancelled);
                RUVIA_CHECK(retained->request().path_ == "/push/0");
            }
            retained.reset();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto root = attachment.loop().start(task_value());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}
