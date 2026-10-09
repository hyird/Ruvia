#include <variant>

#include "http3_client_connection_fixture.h"

namespace {

using connection_test_access = ruvia::detail::http3_client_connection_test_access;

ruvia::detail::HttpClientResponseState* reject_early_test_push(void*, std::size_t,
    Connection&, std::uint64_t, const ruvia::Http3MessageHead&) {
    return nullptr;
}

void finish_early_test_push(void*) noexcept {}

ruvia::Task<void> exerciseEarlyDataRejectionRecovery(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& ticketPeer, local_quic_response_peer& acceptingPeer,
    local_quic_response_peer& rejectingPeer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        ruvia::detail::ClientTransportConfigView tls_config{
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config);
        std::optional<ruvia::detail::http3_quic_client_tls_context::ticket_lease> ticket;
        {
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = ticketPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/ticket", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            ticketPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
            }

            const bool ticket_ready = co_await wait_for_peer(worker, ticketPeer, [&] {
                if (!ticket) {
                    auto captured = tls.take_ticket("127.0.0.1", ruvia::quic_version::v1);
                    if (captured) {
                        ticket.emplace(std::move(*captured));
                    }
                }
                return ticket.has_value(); }, 8s);
            RUVIA_CHECK(ticket_ready);
            if (ticket) {
                RUVIA_CHECK(SSL_SESSION_get_max_early_data(ticket->session.get()) != 0);
                ruvia::detail::http3_quic_client_tls_context no_early_tls(tls_config);
                ruvia::detail::http3_quic_client_tls_context::ssl_session_owner no_early_session(
                    SSL_SESSION_dup(ticket->session.get()));
                RUVIA_CHECK(no_early_session != nullptr);
                if (no_early_session) {
                    SSL_SESSION_set_max_early_data(no_early_session.get(), 0);
                    no_early_tls.remember_ticket(no_early_session.get(), ticket->host,
                        ticket->version, ticket->transport_parameters, ticket->settings);
                    asio::ip::udp::socket reservation(io,
                        {asio::ip::address_v4::loopback(), 0});
                    const auto local = ruvia::detail::to_http3_quic_datagram_address(
                        reservation.local_endpoint());
                    const auto peer = ruvia::detail::to_http3_quic_datagram_address(
                        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), ticketPeer.port()));
                    RUVIA_CHECK((local.index() == 0) && (peer.index() == 0));
                    if ((local.index() == 0) && (peer.index() == 0)) {
                        ruvia::quic_connection_config quic_config;
                        quic_config.local_address = ruvia::detail::to_quic_address(std::get<0>(local));
                        quic_config.peer_address = ruvia::detail::to_quic_address(std::get<0>(peer));
                        ruvia::detail::http3_quic_client_transport transport(
                            no_early_tls, quic_config, "127.0.0.1",
                            std::chrono::steady_clock::now(), &memory, true);
                        RUVIA_CHECK(!transport.early_data_enabled());
                    }
                }
                tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                    ticket->transport_parameters, ticket->settings);
            }
            connection.requestStop();
            co_await tasks.join();
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
        }
        // Reuse the original server TLS context so this peer accepts the real
        // ticket while its handshake is gated after the early-data decision.
        if (ticket) {
            tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                ticket->transport_parameters, ticket->settings);
        }
        {
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = acceptingPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, nullptr, 30s, {}, {}, {}, {},
                ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/accepted-early", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();

            const bool handshake_paused = co_await wait_for_peer(worker, acceptingPeer, [&] { return acceptingPeer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker, acceptingPeer, [&] { return connection_test_access::early_request_finished(connection, submitted.id); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id,
                {.urgency = 1, .incremental = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id));

            acceptingPeer.allow_handshake();
            const bool priority_observed = co_await wait_for_peer(worker, acceptingPeer, [&] {
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id);
                const auto element_id = acceptingPeer.priority_element_id();
                return acceptingPeer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(priority_observed);
            RUVIA_CHECK(acceptingPeer.handshake_observed());
            RUVIA_CHECK(acceptingPeer.early_data_accepted());
            const auto accepted_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            const auto accepted_priority_id = acceptingPeer.priority_element_id();
            RUVIA_CHECK(accepted_stream_id.has_value() && accepted_priority_id == accepted_stream_id);

            acceptingPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
            }
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            connection.requestStop();
            co_await tasks.join();
        }

        // The second peer has independent ticket keys and must reject the same
        // valid ticket after the client has completely written its 0-RTT request.
        if (ticket) {
            tls.remember_ticket(ticket->session.get(), ticket->host, ticket->version,
                ticket->transport_parameters, ticket->settings);
        }
        {
            ruvia::detail::Http3ClientBodyBudget pushBudget(1024 * 1024);
            int pushObserverContext{};
            const ruvia::detail::Http3ClientPushObserver pushObserver{
                .context = &pushObserverContext,
                .config = {.enabled = true, .maxConcurrentPushes = 1},
                .receive = reject_early_test_push,
                .finished = finish_early_test_push};
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = rejectingPeer.port()}),
                10s, &memory, 4, 16 * 1024 * 1024, 2s, &pushBudget, 30s, {}, {}, {},
                pushObserver, ruvia::quic_version::v1, true);
            ruvia::detail::HttpClientRequestStorage request("GET", "/recover", &memory);
            request.set_replay_safe(true);
            const auto submitted = connection.submit(
                std::move(request), std::chrono::steady_clock::now() + 10s);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            const bool handshake_paused = co_await wait_for_peer(worker, rejectingPeer, [&] { return rejectingPeer.handshake_paused(); }, 8s);
            RUVIA_CHECK(handshake_paused);
            const bool early_request_finished = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection_test_access::early_request_finished(connection, submitted.id); }, 8s);
            RUVIA_CHECK(early_request_finished);
            const auto early_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            RUVIA_CHECK(early_stream_id.has_value());
            RUVIA_CHECK(connection.reprioritize(submitted.id,
                {.urgency = 1, .incremental = true}));
            RUVIA_CHECK(connection_test_access::has_pending_priority_update(connection, submitted.id));

            rejectingPeer.allow_handshake();
            const bool peer_control_ready = co_await wait_for_peer(worker, rejectingPeer, [&] {
                const auto max_push_id = rejectingPeer.peer_max_push_id();
                const auto stream_id = connection_test_access::request_stream_id(connection, submitted.id);
                const auto element_id = rejectingPeer.priority_element_id();
                return max_push_id == std::optional<std::uint64_t>{0} &&
                       rejectingPeer.priority_observed() == 0x101 && stream_id && element_id == stream_id; }, 8s);
            RUVIA_CHECK(peer_control_ready);
            const auto replayed_stream_id = connection_test_access::request_stream_id(connection, submitted.id);
            const auto priority_element_id = rejectingPeer.priority_element_id();
            RUVIA_CHECK(replayed_stream_id.has_value() && priority_element_id == replayed_stream_id);
            RUVIA_CHECK(rejectingPeer.early_data_rejected());
            rejectingPeer.allow_final_part();
            co_await connection.wait(submitted.id);
            const auto completed = connection.result(submitted.id);
            RUVIA_CHECK(completed != nullptr);
            if (completed != nullptr) {
                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
            }
            RUVIA_CHECK(rejectingPeer.handshake_observed());
            RUVIA_CHECK(rejectingPeer.early_data_rejected());
            RUVIA_CHECK(rejectingPeer.request_payload_bytes() != 0);
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});
            RUVIA_CHECK(connection.accepting());

            ruvia::detail::HttpClientRequestStorage cancelled_request("GET", "/cancel", &memory);
            cancelled_request.set_replay_safe(true);
            const auto cancelled = connection.submit(std::move(cancelled_request),
                std::chrono::steady_clock::now() + 5s);
            RUVIA_CHECK(cancelled.outcome == Connection::Outcome::kPending);
            const bool cancel_stream_opened = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(cancel_stream_opened);
            connection.cancel(cancelled.id);
            co_await connection.wait(cancelled.id);
            const auto cancelled_result = connection.result(cancelled.id);
            RUVIA_CHECK(cancelled_result != nullptr &&
                        cancelled_result->outcome == Connection::Outcome::kCancelled);
            RUVIA_CHECK(connection.release(cancelled.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            ruvia::detail::HttpClientRequestStorage timed_request("GET", "/timeout", &memory);
            timed_request.set_replay_safe(true);
            const auto timed = connection.submit(std::move(timed_request),
                std::chrono::steady_clock::now() + 500ms);
            RUVIA_CHECK(timed.outcome == Connection::Outcome::kPending);
            const bool timeout_stream_opened = co_await wait_for_peer(worker, rejectingPeer, [&] { return connection.active_response_streams() != 0; }, 2s);
            RUVIA_CHECK(timeout_stream_opened);
            co_await connection.wait(timed.id);
            const auto timed_result = connection.result(timed.id);
            RUVIA_CHECK(timed_result != nullptr &&
                        timed_result->outcome == Connection::Outcome::kDeadline);
            RUVIA_CHECK(connection.release(timed.id));
            RUVIA_CHECK_EQ(connection.active_response_streams(), std::size_t{0});

            connection.requestStop();
            co_await tasks.join();
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3ClientReplaysBodylessGetAfterRejectedEarlyDataOnRebuiltCriticalStreams) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    auto serverTlsConfig = ruvia::detail::HttpServerListenerDefinition::Tls{};
    serverTlsConfig.identity.certificateChainFile = identity.certificate().string();
    serverTlsConfig.identity.privateKeyFile = identity.privateKey().string();
    serverTlsConfig.http3_early_data = true;
    ruvia::detail::http3_quic_tls_context shared_server_tls(
        serverTlsConfig, std::pmr::get_default_resource());
    local_quic_response_peer ticketPeer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls);
    local_quic_response_peer acceptingPeer(identity, false, true, false, false, {}, false, false,
        true, &shared_server_tls, true);
    // This independent server TLS context must reject the otherwise valid ticket.
    local_quic_response_peer rejectingPeer(identity, false, true, false, false, {}, false, false,
        true, nullptr, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseEarlyDataRejectionRecovery(
        io, worker, attachment, ticketPeer, acceptingPeer, rejectingPeer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3_client_observes_origin_frame_over_authenticated_quic_and_retains_after_shutdown) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    const auto caFile = identity.certificate().string();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx, true, caFile));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientUploadExchangeWakesQuicDriverAndWaitsForContinueWithoutWriteTimeout) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps,
                                                        .host = "127.0.0.1",
                                                        .port = peer.port(),
                                                        .connectTimeout = 10s,
                                                        .write_timeout = 100ms,
                                                        .requestTimeout = 12s,
                                                        .protocol = ruvia::HttpClientProtocol::kHttp3Only,
                                                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
        std::exception_ptr failure;
        try {
            auto exchange = co_await client.openRequest({.method = "POST", .target = "/upload"},
                {.contentLength = 6, .expectation = ruvia::HttpClientRequestExpectation::kContinue, .continueTimeout = 200ms});
            co_await exchange.body().write("abc");
            // Waiting for an application producer does not consume write inactivity.
            (void)co_await ruvia::sleepFor(worker, 150ms);
            co_await exchange.body().write("def");
            const std::array<ruvia::HttpHeaderView, 1> trailers{{{"x-end", "retained"}}};
            co_await exchange.body().end(trailers);
            RUVIA_CHECK(exchange.body().complete());
            auto response = co_await exchange.response();
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto first = co_await response.body().text();
            RUVIA_CHECK(first && *first == "abc");
            peer.allow_final_part();
            auto rest = co_await response.body().readAll(16);
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
#endif
}

RUVIA_TEST(http3_client_push_owns_promises_reclaims_repeated_streams_and_retains_results_after_shutdown) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, false, quic_push_scenario{.count = 100, .stream_before_promise = true});
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    CountingResource memory;
    auto task = [&]() -> ruvia::Task<void> {
        std::exception_ptr failure;
        std::optional<ruvia::HttpClientPush> saved;
        std::optional<ruvia::HttpClientResponse> savedResponse;
        {
            ruvia::HttpClientConfig config{.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = true, .maxConcurrentPushes = 1, .timeout = 5s}, .caFile = identity.certificate().string()};
            ruvia::detail::HttpClientPool client(io, worker, ruvia::detail::HttpClientConfigStorage(config, &memory), ruvia::HttpClientResultBudgetConfig{}, &memory);
            std::exception_ptr operationFailure;
            try {
                auto parent = co_await client.execute(ruvia::detail::HttpClientRequestStorage("GET", "/parent", &memory), {});
                for (std::size_t index = 0; index < 100; ++index) {
                    std::optional<ruvia::HttpClientPush> push;
                    const auto available = co_await wait_for_peer(worker, peer, [&] { push = client.nextPush(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("HTTP/3 push did not arrive");
                    }
                    RUVIA_CHECK(std::string_view(push->request().path) == "/push/" + std::to_string(index));
                    RUVIA_CHECK(push->request().headers[0].name() == "x-promise");
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
                    RUVIA_CHECK(response.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
                    auto bytes = co_await response.body().readAll(32);
                    RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "ppppp");
                    if (index == 0) {
                        saved.emplace(std::move(moved));
                        savedResponse.emplace(std::move(response));
                    }
                    RUVIA_CHECK(saved->request().path == "/push/0");
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
                RUVIA_CHECK(client.stats().receivedPushes == 100 && client.stats().rejectedPushes == 0);
            } catch (...) {
                operationFailure = std::current_exception();
            }
            client.closeNow();
            co_await client.join();
            failure = operationFailure;
        }
        if (saved && savedResponse) {
            RUVIA_CHECK(saved->request().path == "/push/0");
            RUVIA_CHECK(savedResponse->status().value() == 200 && savedResponse->body().complete());
        }
        savedResponse.reset();
        saved.reset();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(task());
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
    RUVIA_CHECK(memory.liveBytes == 0 && memory.allocations == memory.returns);
}

RUVIA_TEST(http3_client_push_bounds_origin_validation_errors_and_cancellation_preserve_parent) {
    TestIdentityFiles identity;
    const std::array scenarios{
        quic_push_scenario{.count = 2, .body_bytes = 1024 * 1024},
        quic_push_scenario{.promise_only = true},
        quic_push_scenario{.cancel_before_promise = true},
        quic_push_scenario{.malformed_response = true},
        quic_push_scenario{.cross_origin = true},
    };
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        local_quic_response_peer peer(identity, false, true, false, false, scenarios[index]);
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        const auto worker = attachment.loop().handle();
        auto task = [&]() -> ruvia::Task<void> {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = true, .maxQueuedPushes = 1, .maxConcurrentPushes = 2, .timeout = index == 1 ? 100ms : 5s}, .caFile = identity.certificate().string()});
            std::exception_ptr operationFailure;
            try {
                auto parent = co_await client.send({.method = "GET", .target = "/parent"});
                std::optional<ruvia::HttpClientPush> push;
                if (index == 0) {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().rejectedPushes == 1; }, 2s));
                }
                if (index != 2 && index != 4) {
                    const auto available = co_await wait_for_peer(worker, peer, [&] { push = client.nextPush(); return push.has_value(); }, 5s);
                    RUVIA_CHECK(available);
                    if (!push) {
                        throw std::runtime_error("expected HTTP/3 promise did not arrive");
                    }
                    if (index == 0) {
                        push.reset();
                        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.cancelled_pushes() >= 1; }, 2s));
                        RUVIA_CHECK(client.stats().rejectedPushes == 1);
                    } else {
                        bool failed = false;
                        try {
                            auto response = co_await push->response();
                            (void)co_await response.body().readAll(32);
                        } catch (const ruvia::HttpClientError& error) {
                            failed = error.code() == (index == 1 ? ruvia::HttpClientError::Code::kTimeout : ruvia::HttpClientError::Code::kProtocolError);
                        }
                        RUVIA_CHECK(failed);
                    }
                } else {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.promised_pushes() == 1; }, 2s));
                    if (index == 4) {
                        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().rejectedPushes == 1; }, 2s));
                    } else {
                        (void)co_await ruvia::sleepFor(worker, 50ms);
                    }
                    auto cancelled_push = client.nextPush();
                    if (index == 2) {
                        // QUIC does not order the control and request streams.
                        // A promise may arrive before its cancellation, but must
                        // never expose a usable response after cancellation.
                        if (cancelled_push) {
                            bool cancelled{};
                            try {
                                (void)co_await cancelled_push->response();
                            } catch (const ruvia::HttpClientError& error) {
                                cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
                            }
                            RUVIA_CHECK(cancelled);
                        }
                        RUVIA_CHECK(client.stats().receivedPushes <= 1);
                    } else {
                        RUVIA_CHECK(!cancelled_push);
                        RUVIA_CHECK(client.stats().receivedPushes == 0);
                        RUVIA_CHECK(client.stats().rejectedPushes == 1);
                    }
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
            } catch (...) {
                operationFailure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            attachment.stop();
            if (operationFailure) {
                std::rethrow_exception(operationFailure);
            }
        };
        auto root = attachment.loop().start(task());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}

RUVIA_TEST(http3_client_push_streaming_priority_disabled_permission_and_shutdown_are_worker_owned) {
    TestIdentityFiles identity;
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        local_quic_response_peer peer(identity, false, true, false, false,
            quic_push_scenario{.body_bytes = 1280 * 1024, .promise_only = scenario == 1});
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        const auto worker = attachment.loop().handle();
        auto task = [&]() -> ruvia::Task<void> {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .connectTimeout = 5s, .requestTimeout = 10s, .maxResponseBytes = 2 * 1024 * 1024, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .push = {.enabled = scenario != 2, .maxConcurrentPushes = 1, .timeout = std::nullopt}, .caFile = identity.certificate().string()});
            std::exception_ptr failure;
            std::optional<ruvia::HttpClientPush> retained;
            try {
                auto parent = co_await client.send({.method = "GET", .target = "/parent"});
                if (scenario != 2) {
                    RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { retained = client.nextPush(); return retained.has_value(); }, 2s));
                    if (!retained) {
                        throw std::runtime_error("expected push missing");
                    }
                    if (scenario == 0) {
                        auto response = co_await retained->response();
                        response.reprioritize({.urgency = 1, .incremental = true});
                        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.push_priority_observed() == 0x101; }, 2s));
                        std::size_t bytes{};
                        while (auto part = co_await response.body().text()) {
                            RUVIA_CHECK(part->find_first_not_of('p') == std::string_view::npos);
                            bytes += part->size();
                        }
                        RUVIA_CHECK(bytes == 1280 * 1024);
                    }
                } else {
                    RUVIA_CHECK(!client.nextPush());
                    RUVIA_CHECK(client.stats().receivedPushes == 0 && peer.promised_pushes() == 0);
                }
                peer.allow_final_part();
                auto bytes = co_await parent.body().readAll(32);
                RUVIA_CHECK(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.bytes().size()) == "abcdef");
            } catch (...) {
                failure = std::current_exception();
            }
            client.close();
            co_await client.shutdown();
            if (scenario == 1 && retained) {
                bool cancelled = false;
                try {
                    (void)co_await retained->response();
                } catch (const ruvia::HttpClientError& error) {
                    cancelled = error.code() == ruvia::HttpClientError::Code::kCancelled;
                }
                RUVIA_CHECK(cancelled);
                RUVIA_CHECK(retained->request().path == "/push/0");
            }
            retained.reset();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto root = attachment.loop().start(task());
        attachment.run();
        root.get();
        peer.rethrow_if_failed();
    }
}
