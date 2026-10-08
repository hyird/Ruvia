#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_websocket_client_drives_extended_connect_dynamic_qpack_deflate_and_fin) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    const auto run = [&]() -> ruvia::Task<void> {
        ruvia::WebSocketClient client(attachment.loop(), {.scheme = ruvia::WebSocketScheme::kWss,
                                                             .protocol = ruvia::WebSocketClientProtocol::kHttp3,
                                                             .host = "127.0.0.1",
                                                             .port = peer.port(),
                                                             .subprotocols = {"chat"},
                                                             .deflate = {.enabled = true},
                                                             .connectTimeout = 5s,
                                                             .readTimeout = 2s,
                                                             .write_timeout = 2s,
                                                             .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification});
        std::exception_ptr failure;
        try {
            co_await client.connect();
            RUVIA_CHECK_EQ(client.subprotocol(), std::string_view("chat"));
            auto greeting = co_await client.read();
            RUVIA_CHECK(greeting.has_value());
            if (greeting) {
                RUVIA_CHECK_EQ(greeting->payload(), std::string(100000, 'w'));
            }
            const std::string payload(100000, 'c');
            co_await client.binary(payload, {.compress = false});
            co_await client.binary(payload, {.compress = true});
            RUVIA_CHECK(!(co_await client.read()).has_value());
            const bool finObserved = co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s);
            RUVIA_CHECK(finObserved);
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
        TestIdentityFiles identity;
        local_quic_response_peer peer(identity, false, true, false, false, {}, true);
        if (round >= 2) {
            peer.allow_final_part();
        }
        auto& io = ruvia::test::newTestIoContext();
        auto attachment = ruvia::attachEventLoop(io);
        std::exception_ptr failure;
        auto run = [&]() -> ruvia::Task<void> {
            const auto& worker = attachment.loop().handle();
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .requestTimeout = 5s, .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            std::optional<ruvia::HttpClientTunnel> retired_tunnel;
            try {
                const bool extended = (round & 1) != 0;
                auto result = co_await client.openTunnel({.authority = extended ? "proxy.test" : "target.test:443", .protocol = extended ? "test-protocol" : "", .target = extended ? "/tunnel" : ""});
                RUVIA_CHECK(result.tunnel() && !result.response());
                if (!result.tunnel()) {
                    throw std::runtime_error("missing CONNECT tunnel");
                }
                retired_tunnel.emplace(std::move(*result.tunnel()));
                auto& tunnel = *retired_tunnel;
                RUVIA_CHECK(tunnel.protocolVersion() == ruvia::HttpProtocolVersion::kHttp3);
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                std::string greeting;
                while (greeting.size() < 100003) {
                    const auto bytes = co_await tunnel.read();
                    if (!bytes) {
                        throw std::runtime_error("early tunnel FIN");
                    }
                    greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                }
                RUVIA_CHECK(greeting.starts_with(std::string(100003, 's')));
                if (round >= 2) {
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                }
                std::string payload(120003, 't');
                for (std::size_t offset = 0; offset != payload.size();) {
                    const auto count = std::min<std::size_t>(16384, payload.size() - offset);
                    std::string owned = payload.substr(offset, count);
                    auto output = tunnel.write(std::string_view(owned));
                    owned.assign("mutated");
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await tunnel.finish();
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s));
                RUVIA_CHECK_EQ(peer.tunnel_bytes(), payload.size());
                peer.allow_final_part();
                if (round < 2) {
                    while (auto bytes = co_await tunnel.read()) {
                        greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                    RUVIA_CHECK(greeting == std::string(100003, 's') + "ended");
                }
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().completedRequests == 1; }, 2s));
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return client.stats().inFlightRequests == 0; }, 2s));
                // Normal bidirectional retirement must not undo a successfully
                // completed sending direction or make finish non-idempotent.
                co_await tunnel.finish();
                RUVIA_CHECK(tunnel.header("x-tunnel") == "owned-metadata");
                RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)tunnel.write("late"); }));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (!failure && retired_tunnel) {
                try {
                    co_await retired_tunnel->finish();
                    RUVIA_CHECK(retired_tunnel->header("x-tunnel") == "owned-metadata");
                    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)retired_tunnel->write("late after shutdown"); }));
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
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, true, false, false, {}, false, true);
    peer.allow_final_part();
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        std::optional<ruvia::HttpUdpDatagram> retained;
        {
            ruvia::HttpClient client(attachment.loop(), {.scheme = ruvia::HttpScheme::kHttps, .host = "127.0.0.1", .port = peer.port(), .connectionCount = 1, .requestTimeout = 5s, .maxResponseBytes = 16384, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            try {
                auto result = co_await client.openUdpTunnel({.target = "/udp"}, {.maxChunkBytes = 1024});
                if (!result.tunnel()) {
                    throw std::runtime_error("CONNECT-UDP rejected");
                }
                RUVIA_CHECK(result.tunnel()->header("capsule-protocol") == "?1");
                auto udp = std::move(*result.tunnel()).udp();
                retained = co_await udp.read();
                RUVIA_CHECK(retained && retained->payload().size() == 16003);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte byte) { return byte == std::byte{'s'}; }));
                auto empty = co_await udp.read();
                RUVIA_CHECK(empty && empty->payload().empty());
                RUVIA_CHECK(!(co_await udp.read()));
                std::string bytes(16003, 't');
                auto output = udp.send(bytes);
                bytes.assign("mutated");
                co_await std::move(output);
                co_await udp.send("");
                co_await udp.finish();
                RUVIA_CHECK(co_await wait_for_peer(worker, peer, [&] { return peer.client_end_observed(); }, 2s));
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
