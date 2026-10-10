#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_websocket_client_drives_extended_connect_deflate_and_fin) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::wss,
                                                              .protocol_ = ruvia::websocket_client_protocol::http3,
                                                              .host_ = "localhost",
                                                              .port_ = peer.port(),
                                                              .target_ = "/ws",
                                                              .subprotocols_ = {"chat"},
                                                              .deflate_ = {.enabled_ = true},
                                                              .connect_timeout_ = 5s,
                                                              .read_timeout_ = 2s,
                                                              .write_timeout_ = 2s,
                                                              .ca_file_ = identity.certificate().string()});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            RUVIA_CHECK_EQ(client.subprotocol(), std::string_view("chat"));
            auto greeting = co_await client.read();
            RUVIA_CHECK(greeting.has_value());
            if (greeting) {
                RUVIA_CHECK_EQ(greeting->payload(), std::string(100000, 'w'));
            }
            const std::string payload_value(100000, 'c');
            for (const bool compress : {false, true}) {
                co_await client.binary(payload_value, {.compress_ = compress});
                const auto echo = co_await client.read();
                RUVIA_CHECK(echo.has_value());
                if (echo) {
                    RUVIA_CHECK(echo->opcode() == ruvia::websocket_opcode::binary);
                    RUVIA_CHECK_EQ(echo->payload(), std::string_view(payload_value));
                }
            }
            RUVIA_CHECK(!(co_await client.read()).has_value());
            const bool fin_observed = co_await wait_for_peer(worker_value, peer, [&] { return peer.client_end_observed(); }, 2s);
            RUVIA_CHECK(fin_observed);
            RUVIA_CHECK_EQ(peer.websocket_messages(), 2U);
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

RUVIA_TEST(http3_websocket_rejection_after_interim_response_is_delivered) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::wss,
                                                              .protocol_ = ruvia::websocket_client_protocol::http3,
                                                              .host_ = "localhost",
                                                              .port_ = peer.port(),
                                                              .target_ = "/ws-informed-reject",
                                                              .connect_timeout_ = 5s,
                                                              .read_timeout_ = 2s,
                                                              .write_timeout_ = 2s,
                                                              .ca_file_ = identity.certificate().string()});
        std::optional<ruvia::websocket_client_error::code_type> code;
        try {
            co_await client.connect();
        } catch (const ruvia::websocket_client_error& error) {
            code = error.code();
        }
        co_await client.shutdown();
        // The final 403 follows the published 103 on the same stream instead
        // of the server abandoning the request and closing the connection.
        RUVIA_CHECK(code == ruvia::websocket_client_error::code_type::handshake_rejected);
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}

RUVIA_TEST(http3_client_tunnel_preserves_metadata_inputs_and_both_half_close_orders) {
    for (unsigned round = 0; round != 4; ++round) {
        test_identity_files identity;
        local_http3_peer peer(identity);
        if (round >= 2) {
            peer.allow_final_part();
        }
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::task<void> {
            const auto& worker_value = attachment.loop().handle();
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "localhost", .port_ = peer.port(), .connection_count_ = 1, .request_timeout_ = 5s, .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
            std::optional<ruvia::http_client_tunnel> retired_tunnel;
            ruvia::task_scope readers(worker_value);
            std::string greeting;
            const auto receive = [&]() -> ruvia::task<void> {
                auto& tunnel = *retired_tunnel;
                try {
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                } catch (...) {
                    tunnel.abort();
                    throw;
                }
            };
            try {
                const bool extended = (round & 1) != 0;
                std::string authority = extended ? "proxy.test" : "target.test:443";
                std::string protocol = extended ? "test-protocol" : "";
                std::string target = extended ? "/tunnel" : "";
                auto open = client.open_tunnel({.authority_ = authority, .protocol_ = protocol, .target_ = target});
                authority.assign("mutated");
                protocol.assign("mutated");
                target.assign("mutated");
                auto result_value = co_await std::move(open);
                RUVIA_CHECK(result_value.tunnel() && !result_value.response());
                if (!result_value.tunnel()) {
                    throw std::runtime_error("missing CONNECT tunnel");
                }
                retired_tunnel.emplace(std::move(*result_value.tunnel()));
                auto& tunnel = *retired_tunnel;
                RUVIA_CHECK(tunnel.protocol_version() == ruvia::http_protocol_version::http3);
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                if (round >= 2) {
                    co_await receive();
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                } else {
                    // Both peers consume independently while writing past QUIC's initial window.
                    readers.spawn(receive());
                }
                std::string payload_value(120003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    std::string owned = payload_value.substr(offset, count);
                    auto output = tunnel.write(std::string_view(owned));
                    owned.assign("mutated");
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                if (!(co_await wait_for_peer(worker_value, peer, [&] { return peer.client_end_observed(); }, 2s))) {
                    throw std::runtime_error("HTTP/3 peer did not observe tunnel FIN in round " +
                                             std::to_string(round) + " after " +
                                             std::to_string(peer.tunnel_bytes()) + " bytes");
                }
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), payload_value.size());
                peer.allow_final_part();
            } catch (...) {
                failure = std::current_exception();
                if (retired_tunnel) {
                    retired_tunnel->abort();
                }
                readers.request_stop();
            }
            try {
                co_await readers.join();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
            try {
                if (failure) {
                    std::rethrow_exception(failure);
                }
                auto& tunnel = *retired_tunnel;
                RUVIA_CHECK(greeting.starts_with(std::string(100003, 's')));
                RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().completed_requests_ == 1; }, 2s));
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().in_flight_requests_ == 0; }, 2s));
                // Normal bidirectional retirement must not undo a successfully
                // completed sending direction or make finish non-idempotent.
                co_await tunnel.finish();
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                RUVIA_CHECK(co_await write_after_finish_is_rejected(tunnel, "late"));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (!failure && retired_tunnel) {
                try {
                    co_await retired_tunnel->finish();
                    RUVIA_CHECK(retired_tunnel->header("x-tunnel") == "owned-metadata");
                    RUVIA_CHECK(co_await write_after_finish_is_rejected(*retired_tunnel, "late after shutdown"));
                } catch (...) {
                    failure = std::current_exception();
                }
            }
        };
        run_client_task(attachment, run());
        peer.rethrow_if_failed();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}

RUVIA_TEST(http3_client_udp_tunnel_negotiates_capsules_owns_packets_and_keeps_send_open_after_peer_fin) {
    test_identity_files identity;
    local_http3_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        std::optional<ruvia::http_udp_datagram> retained;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "localhost", .port_ = peer.port(), .connection_count_ = 1, .request_timeout_ = 5s, .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
            try {
                auto result_value = co_await client.open_udp_tunnel({.target_ = "/udp"}, {.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT-UDP rejected");
                }
                RUVIA_CHECK(result_value.tunnel()->header("capsule-protocol") == "?1");
                auto udp = std::move(*result_value.tunnel()).udp();
                retained = co_await udp.read();
                RUVIA_CHECK(retained && retained->payload().size() == 16003);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte byte) { return byte == std::byte{'s'}; }));
                auto empty = co_await udp.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await udp.read()));
                std::string bytes_value(16003, 't');
                auto output = udp.send(bytes_value);
                bytes_value.assign("mutated");
                co_await std::move(output);
                co_await udp.send("");
                co_await udp.finish();
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return peer.client_end_observed(); }, 2s));
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), std::size_t{16003});
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload().size() == 16003);
        retained.reset();
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
