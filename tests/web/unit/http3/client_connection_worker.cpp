#include "http3_client_connection_fixture.h"

RUVIA_TEST(http3_public_server_and_client_route_native_datagrams_capsules_and_verify_tls) {
    test_identity_files identity;
    local_http3_peer peer(identity, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    const auto worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_datagram> retained;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "localhost", .port_ = peer.port(), .request_timeout_ = 5s, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
            try {
                {
                    auto connected = co_await client.open_tunnel({.authority_ = "target.test:443"});
                    if (!connected.tunnel()) {
                        throw std::runtime_error("ordinary CONNECT rejected");
                    }
                    auto& bytes_value = *connected.tunnel();
                    co_await bytes_value.write("ordinary CONNECT");
                    co_await bytes_value.finish();
                    std::string echoed;
                    while (const auto chunk = co_await bytes_value.read()) {
                        echoed.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
                    }
                    RUVIA_CHECK_EQ(echoed, "ordinary CONNECT");
                }
                asio::ip::udp::socket reservation(io,
                    {asio::ip::address_v4::loopback(), 0});
                const auto candidate_value = reservation.local_endpoint();
                reservation.close();
                auto migration = client.start_quic_path_migration(candidate_value);
                const auto migration_deadline = std::chrono::steady_clock::now() + 5s;
                while (migration.status_ == ruvia::quic_migration_status::started &&
                       std::chrono::steady_clock::now() < migration_deadline) {
                    (void)co_await ruvia::sleep_for(worker_value, 1ms);
                    const auto observed_value = client.path_migration(migration.id_);
                    if (!observed_value) {
                        throw std::runtime_error("migration state retired before observation");
                    }
                    migration = *observed_value;
                }
                RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::validated);
                RUVIA_CHECK_EQ(migration.local_address_.port_, candidate_value.port());
                auto opened = co_await client.open_tunnel({.authority_ = "target.test:443", .protocol_ = "test-datagram", .target_ = "/datagrams"}, {.datagrams_ = true});
                if (!opened.tunnel()) {
                    throw std::runtime_error("native HTTP Datagram tunnel rejected");
                }
                auto datagrams = std::move(*opened.tunnel()).datagrams();
                std::string input(1000, 'n');
                auto send = datagrams.send(input);
                input.assign("changed");
                co_await std::move(send);
                retained = co_await datagrams.read();
                RUVIA_CHECK(retained && retained->payload().size() == 1000);
                RUVIA_CHECK(retained && retained->transport() == ruvia::http_datagram_transport::quic);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte b) { return b == std::byte{'n'}; }));
                co_await datagrams.send("");
                auto empty = co_await datagrams.read();
                RUVIA_CHECK(empty && empty->payload().empty() && empty->transport() == ruvia::http_datagram_transport::quic);
                co_await datagrams.send(std::string(16003, 'c'));
                auto reliable = co_await datagrams.read();
                RUVIA_CHECK(reliable && reliable->payload().size() == 16003 && reliable->transport() == ruvia::http_datagram_transport::capsule);
                co_await datagrams.finish();
                RUVIA_CHECK(!(co_await datagrams.read()));
                auto udp_opened = co_await client.open_udp_tunnel({.target_ = "/udp/target.test/443"});
                if (!udp_opened.tunnel()) {
                    throw std::runtime_error("native CONNECT-UDP rejected");
                }
                auto udp = std::move(*udp_opened.tunnel()).udp();
                co_await udp.send("native UDP");
                auto packet = co_await udp.read();
                RUVIA_CHECK(packet && packet->payload().size() == 10 && packet->transport() == ruvia::http_datagram_transport::quic);
                co_await udp.send("");
                auto udp_empty = co_await udp.read();
                RUVIA_CHECK(udp_empty && udp_empty->payload().empty());
                co_await udp.finish();
                RUVIA_CHECK(!(co_await udp.read()));
                auto pending_opened = co_await client.open_udp_tunnel({.target_ = "/udp/target.test/443"});
                if (!pending_opened.tunnel()) {
                    throw std::runtime_error("pending native CONNECT-UDP rejected");
                }
                auto pending = std::move(*pending_opened.tunnel()).udp();
                ruvia::task_scope reads(worker_value);
                bool cancelled{};
                auto read = [&]() -> ruvia::task<void> {
                    try {
                        (void)co_await pending.read();
                    } catch (const ruvia::http_client_error& error) {
                        if (error.code() != ruvia::http_client_error::code_type::cancelled) {
                            throw;
                        }
                        cancelled = true;
                    }
                };
                std::exception_ptr pending_failure;
                try {
                    reads.spawn(read());
                    (void)co_await ruvia::sleep_for(worker_value, 1ms);
                } catch (...) {
                    pending_failure = std::current_exception();
                }
                pending.abort();
                reads.request_stop();
                try {
                    co_await reads.join();
                } catch (...) {
                    if (!pending_failure) {
                        pending_failure = std::current_exception();
                    }
                }
                if (pending_failure) {
                    std::rethrow_exception(pending_failure);
                }
                RUVIA_CHECK(cancelled);
                // Retain the independent public-client cancellation/echo path.
                ruvia::http_client other(attachment.loop(), {.scheme_ = ruvia::http_scheme::https, .host_ = "localhost", .port_ = peer.port(), .request_timeout_ = 5s, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
                try {
                    auto opened_other = co_await other.open_udp_tunnel({.target_ = "/udp/target.test/443"});
                    if (!opened_other.tunnel()) {
                        throw std::runtime_error("second client UDP rejected");
                    }
                    auto packets = std::move(*opened_other.tunnel()).udp();
                    co_await packets.send("second client");
                    auto result_value = co_await packets.read();
                    RUVIA_CHECK(result_value && result_value->payload().size() == 13);
                    packets.abort();
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await other.shutdown();
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload().size() == 1000);
        retained.reset();
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

RUVIA_TEST(http3_client_worker_stop_cancels_pending_datagram_read_and_joins_shutdown) {
    test_identity_files identity;
    local_http3_peer peer(identity, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "localhost",
                                                         .port_ = peer.port(),
                                                         .request_timeout_ = 5s,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .ca_file_ = identity.certificate().string()});
        std::optional<ruvia::http_udp_tunnel> pending;
        ruvia::task_scope reads(worker);
        bool read_started{};
        bool read_completed{};
        std::exception_ptr failure;
        auto read = [&]() -> ruvia::task<void> {
            read_started = true;
            try {
                const auto packet = co_await pending->read();
                RUVIA_CHECK(!packet);
                read_completed = true;
            } catch (const ruvia::http_client_error& error) {
                if (error.code() != ruvia::http_client_error::code_type::cancelled) {
                    throw;
                }
                read_completed = true;
            }
        };
        try {
            auto opened = co_await client.open_udp_tunnel({.target_ = "/udp/target.test/443"});
            if (!opened.tunnel()) {
                throw std::runtime_error("worker-stop CONNECT-UDP rejected");
            }
            pending.emplace(std::move(*opened.tunnel()).udp());
            reads.spawn(read());
            (void)co_await ruvia::sleep_for(worker, 1ms);
            RUVIA_CHECK(read_started);
            // Exercise the public loop's client cleanup with an active consumer.
            attachment.stop();
        } catch (...) {
            failure = std::current_exception();
            if (pending) {
                pending->abort();
            }
            client.close();
        }
        reads.request_stop();
        try {
            co_await reads.join();
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
        pending.reset();
        if (failure) {
            std::rethrow_exception(failure);
        }
        RUVIA_CHECK(read_completed);
        RUVIA_CHECK(!worker.accepting());
    };
    run_client_task(attachment, run());
    peer.rethrow_if_failed();
}
