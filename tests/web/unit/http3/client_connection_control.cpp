#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_client_upload_exchange_waits_for_continue_and_preserves_producer_inactivity_and_trailers) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = peer.port(),
                                                         .connect_timeout_ = 10s,
                                                         .write_timeout_ = 100ms,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .ca_file_ = identity.certificate().string()});
        std::exception_ptr failure;
        try {
            auto exchange = co_await client.open_request({.method_ = "POST", .target_ = "/upload"},
                {.content_length_ = 6, .expectation_ = ruvia::http_client_request_expectation::continue_value, .continue_timeout_ = 200ms});
            co_await exchange.body().write("abc");
            // An application's producer pause does not consume write inactivity.
            (void)co_await ruvia::sleep_for(worker, 150ms);
            co_await exchange.body().write("def");
            const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
            co_await exchange.body().end(trailers);
            RUVIA_CHECK(exchange.body().complete());
            auto response = co_await exchange.response();
            RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto body = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), "abcdef");
            RUVIA_CHECK_EQ(peer.request_payload_bytes(), std::size_t{6});
            RUVIA_CHECK(peer.upload_trailer_observed());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3_client_shutdown_sends_connection_close_so_the_peer_retires_promptly) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    const auto server_connections = [] { return ruvia::app().http_stats().active_connections_; };
    auto run = [&]() -> ruvia::task<void> {
        // Earlier cases' clients retired their connections the same way.
        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return server_connections() == 0; }, 5s));
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = peer.port(),
                                                         .connect_timeout_ = 10s,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .ca_file_ = identity.certificate().string()});
        std::exception_ptr failure;
        try {
            auto response = co_await client.send({.target_ = "/hello"});
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto body = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), "hello");
            RUVIA_CHECK_EQ(server_connections(), std::size_t{1});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
        // Without the client's CONNECTION_CLOSE the server keeps the connection
        // until the 30 s QUIC idle timeout; the close only costs its draining
        // period of three loopback PTOs.
        RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return server_connections() == 0; }, 5s));
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3_client_replays_0rtt_requests_from_fresh_state_after_an_endpoint_refuses) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    // The peer listens on 127.0.0.1 only. Where "localhost" resolves to ::1
    // first, every new QUIC connection's first attempt is refused, after a
    // resumed one already opened its 0-RTT request stream. The next endpoint
    // must then start from fresh protocol state and send the request again.
    asio::io_context resolve_io;
    asio::ip::udp::resolver resolver(resolve_io);
    const auto resolved = resolver.resolve("localhost", std::to_string(peer.port()));
    const bool first_attempt_refused = !resolved.empty() &&
                                       resolved.begin()->endpoint().address() != asio::ip::address(asio::ip::address_v4::loopback());
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 2,
                                                         .connect_timeout_ = 10s,
                                                         .request_timeout_ = 12s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .http3_early_data_ = true,
                                                         .ca_file_ = identity.certificate().string()});
        std::array<std::string, 3> bodies;
        const auto fetch = [&](std::string& body) -> ruvia::task<void> {
            auto response = co_await client.send({.target_ = "/early", .replay_safe_ = true});
            RUVIA_CHECK_EQ(response.status().value(), 200);
            auto bytes = co_await response.body().read_all(16);
            body.assign(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size());
        };
        ruvia::task_scope requests(worker);
        std::exception_ptr failure;
        try {
            // A full handshake stores the single-use resumption ticket.
            co_await fetch(bodies[0]);
            // While one request occupies that connection, the other opens the
            // second connection with the ticket and offers its request as 0-RTT.
            requests.spawn(fetch(bodies[1]));
            requests.spawn(fetch(bodies[2]));
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await requests.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        try {
            co_await client.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK_EQ(bodies[0], "full");
        for (const auto& body : {bodies[1], bodies[2]}) {
            RUVIA_CHECK(body == "early" || body == "full");
            // The refused attempt consumed the ticket, so the replay follows
            // the next endpoint's full handshake.
            RUVIA_CHECK(!first_attempt_refused || body == "full");
        }
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3_client_fails_the_request_when_every_endpoint_refuses_and_shutdown_joins) {
    test_identity_files identity;
    std::uint16_t port{};
    {
        asio::io_context probe_io;
        asio::ip::udp::socket probe(probe_io, {asio::ip::address_v4::loopback(), 0});
        port = probe.local_endpoint().port();
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = port,
                                                         .connect_timeout_ = 2s,
                                                         .request_timeout_ = 5s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .ca_file_ = identity.certificate().string()});
        std::optional<ruvia::http_client_error::code_type> code;
        std::exception_ptr failure;
        try {
            (void)co_await client.send({.target_ = "/hello"});
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
        // Refusal surfaces immediately; a silently dropping host only ends at
        // the connect deadline.
        RUVIA_CHECK(code == ruvia::http_client_error::code_type::connect_failed ||
                    code == ruvia::http_client_error::code_type::timeout);
    };
    run_client_task(attachment, run());
}
