#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_websocket_client_drives_extended_connect_dynamic_qpack_deflate_and_fin) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, false, true, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::websocket_client client(attachment.loop(), {.scheme_ = ruvia::websocket_scheme::wss,
                                                              .protocol_ = ruvia::websocket_client_protocol::http3,
                                                              .host_ = "127.0.0.1",
                                                              .port_ = peer.port(),
                                                              .subprotocols_ = {"chat"},
                                                              .deflate_ = {.enabled_ = true},
                                                              .connect_timeout_ = 5s,
                                                              .read_timeout_ = 2s,
                                                              .write_timeout_ = 2s,
                                                              .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification});
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
            co_await client.binary(payload_value, {.compress_ = false});
            co_await client.binary(payload_value, {.compress_ = true});
            RUVIA_CHECK(!(co_await client.read()).has_value());
            const bool fin_observed = co_await wait_for_peer(worker_value, peer, [&] { return peer.client_end_observed(); }, 2s);
            RUVIA_CHECK(fin_observed);
            RUVIA_CHECK_EQ(peer.websocket_messages(), 2U);
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

RUVIA_TEST(http3_client_tunnel_preserves_metadata_inputs_and_both_half_close_orders) {
    for (unsigned round = 0; round != 4; ++round) {
        test_identity_files identity;
        local_quic_response_peer peer(identity, false, true, false, false, {}, true);
        if (round >= 2) {
            peer.allow_final_part();
        }
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::task<void> {
            const auto& worker_value = attachment.loop().handle();
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "127.0.0.1", .port_ = peer.port(), .connection_count_ = 1, .request_timeout_ = 5s, .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
            std::optional<ruvia::http_client_tunnel> retired_tunnel;
            try {
                const bool extended = (round & 1) != 0;
                auto result_value = co_await client.open_tunnel({.authority_ = extended ? "proxy.test" : "target.test:443", .protocol_ = extended ? "test-protocol" : "", .target_ = extended ? "/tunnel" : ""});
                RUVIA_CHECK(result_value.tunnel() && !result_value.response());
                if (!result_value.tunnel()) {
                    throw std::runtime_error("missing CONNECT tunnel");
                }
                retired_tunnel.emplace(std::move(*result_value.tunnel()));
                auto& tunnel = *retired_tunnel;
                RUVIA_CHECK(tunnel.protocol_version() == ruvia::http_protocol_version::http3);
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                std::string greeting;
                while (greeting.size() < 100003) {
                    const auto bytes_value = co_await tunnel.read();
                    if (!bytes_value) {
                        throw std::runtime_error("early tunnel FIN");
                    }
                    greeting.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                RUVIA_CHECK(greeting.starts_with(std::string(100003, 's')));
                if (round >= 2) {
                    while (auto bytes_value = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
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
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return peer.client_end_observed(); }, 2s));
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), payload_value.size());
                peer.allow_final_part();
                if (round < 2) {
                    while (auto bytes_value = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                }
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().completed_requests_ == 1; }, 2s));
                RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] { return client.stats().in_flight_requests_ == 0; }, 2s));
                // Normal bidirectional retirement must not undo a successfully
                // completed sending direction or make finish non-idempotent.
                co_await tunnel.finish();
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)tunnel.write("late"); }));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (!failure && retired_tunnel) {
                try {
                    co_await retired_tunnel->finish();
                    RUVIA_CHECK(retired_tunnel->header("x-tunnel") == "owned-metadata");
                    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)retired_tunnel->write("late after shutdown"); }));
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
        RUVIA_CHECK(peer.synchronize());
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}

RUVIA_TEST(http3_client_udp_tunnel_negotiates_capsules_owns_packets_and_keeps_send_open_after_peer_fin) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, false, true, false, false, {}, false, true);
    peer.allow_final_part();
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        std::optional<ruvia::http_udp_datagram> retained;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "127.0.0.1", .port_ = peer.port(), .connection_count_ = 1, .request_timeout_ = 5s, .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
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
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK(peer.synchronize());
    if (failure) {
        std::rethrow_exception(failure);
    }
}
