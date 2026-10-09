#include <variant>

#include "ruvia/core/timer.h"

#include "http3_client_connection_fixture.h"

namespace {

void require_loopback_peer(ruvia::context& context_value) {
    const auto info = context_value.conn();
    if (info.remote().address() != "127.0.0.1" || info.remote().port() == 0 ||
        info.tls() == nullptr || info.scheme() != ruvia::http_scheme::https) {
        throw std::runtime_error("HTTP/3 tunnel lost its peer metadata during worker handoff");
    }
}

ruvia::task<void> byte_echo(void*, ruvia::context& context_value) {
    require_loopback_peer(context_value);
    auto& tunnel = context_value.tunnel();
    while (auto bytes = co_await tunnel.read()) {
        co_await tunnel.write(std::move(*bytes));
    }
    co_await tunnel.finish();
}

ruvia::task<void> native_datagram_echo(void*, ruvia::context& context_value) {
    require_loopback_peer(context_value);
    auto datagrams = context_value.tunnel().datagrams();
    while (auto packet = co_await datagrams.read()) {
        const auto payload_value = packet->payload();
        co_await datagrams.send(payload_value);
    }
    co_await datagrams.finish();
}

ruvia::task<void> native_udp_echo(void*, ruvia::context& context_value) {
    require_loopback_peer(context_value);
    ruvia::http_udp_tunnel datagrams(context_value.tunnel().datagrams());
    while (auto packet = co_await datagrams.read()) {
        const auto payload_value = packet->payload();
        co_await datagrams.send(payload_value);
    }
    co_await datagrams.finish();
}

}  // namespace

RUVIA_TEST(http3_native_datagram_server_and_client_route_packets_capsules_verify_tls_and_retire_fixed_workers) {
    test_identity_files identity;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    routes_value.register_tunnel_route("", std::pmr::string("target.test:443"), {nullptr, byte_echo}, {}, {});
    routes_value.register_tunnel_route("test-datagram", std::pmr::string("/datagrams"), {nullptr, native_datagram_echo}, {}, {}, {.datagrams_ = true});
    routes_value.register_tunnel_route("connect-udp", std::pmr::string("/udp/:host/:port"), {nullptr, native_udp_echo}, {}, {});
    routes_value.finalize();
    ruvia::detail::http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = identity.certificate().string();
    tls.identity_.private_key_file_ = identity.private_key().string();
    const std::array listeners{ruvia::detail::http_server_listener_definition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::http3_listen_config{})};
    auto configuration = ruvia::detail::validate_http_server_configuration(listeners,
        {.worker_queue_capacity_ = 32, .max_connections_ = 4, .max_requests_per_connection_ = 64});
    ruvia::detail::web_worker_runtime first(configuration, routes_value.route_table(), {});
    ruvia::detail::web_worker_runtime second(configuration, routes_value.route_table(), {});
    first.prepare();
    second.prepare();
    const auto target = [](ruvia::detail::web_worker_runtime& worker_value) {
        return ruvia::detail::acceptor::worker_target{
            .submission_ = worker_value.network_submission(),
            .object_ = &worker_value,
            .available_ = [](void* raw) noexcept { return static_cast<ruvia::detail::web_worker_runtime*>(raw)->available_for_network_dispatch(); },
            .accept_ = [](void* raw, ruvia::detail::native_accepted_socket_ticket&& ticket) noexcept { static_cast<ruvia::detail::web_worker_runtime*>(raw)->accept_transferred_connection(std::move(ticket)); },
            .stage_quic_ = [](void* raw, ruvia::detail::http3_datagram_channel& channel,
                               asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) { static_cast<ruvia::detail::web_worker_runtime*>(raw)->stage_quic(channel, endpoint, partition); }};
    };
    const std::array targets{target(first), target(second)};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    first.launch();
    second.launch();
    first.wait_until_ready();
    second.wait_until_ready();
    first.request_serve();
    second.request_serve();
    RUVIA_CHECK(first.wait_until_serving());
    RUVIA_CHECK(second.wait_until_serving());
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    std::exception_ptr failure;
    const auto worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_datagram> retained;
        {
            ruvia::http_client client(attachment.loop(), {.host_ = "127.0.0.1", .port_ = network.local_endpoint(0).port(), .request_timeout_ = 5s, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
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
                auto read = [&]() -> ruvia::task<void> {try {static_cast<void>(co_await pending.read());} catch(const ruvia::http_client_error&) {cancelled=true;} };
                reads.spawn(read());
                co_await ruvia::sleep_for(worker_value, 1ms);
                pending.abort();
                co_await reads.join();
                RUVIA_CHECK(cancelled);
                // Retain the independent public-client cancellation/echo path.
                ruvia::http_client other(attachment.loop(), {.host_ = "127.0.0.1", .port_ = network.local_endpoint(0).port(), .request_timeout_ = 5s, .protocol_ = ruvia::http_client_protocol::http3_only, .ca_file_ = identity.certificate().string()});
                try {
                    auto opened_other = co_await other.open_udp_tunnel({.target_ = "/udp/target.test/443"});
                    if (!opened_other.tunnel()) {
                        throw std::runtime_error("second worker UDP rejected");
                    }
                    auto packets = std::move(*opened_other.tunnel()).udp();
                    co_await packets.send("second worker");
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
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    first.stop_admission();
    second.stop_admission();
    network.stop();
    network.join();
    first.finalize_after_network_quiesced();
    second.finalize_after_network_quiesced();
    first.join();
    second.join();
    network.rethrow_failure();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace {
ruvia::detail::acceptor::worker_target production_quic_target(ruvia::detail::web_worker_runtime& worker_value) {
    return {.submission_ = worker_value.network_submission(),
        .object_ = &worker_value,
        .available_ = [](void* raw) noexcept { return static_cast<ruvia::detail::web_worker_runtime*>(raw)->available_for_network_dispatch(); },
        .accept_ = [](void* raw, ruvia::detail::native_accepted_socket_ticket&& ticket) noexcept { static_cast<ruvia::detail::web_worker_runtime*>(raw)->accept_transferred_connection(std::move(ticket)); },
        .stage_quic_ = [](void* raw, ruvia::detail::http3_datagram_channel& channel,
                           asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) { static_cast<ruvia::detail::web_worker_runtime*>(raw)->stage_quic(channel, endpoint, partition); }};
}

class production_quic_workers final {
public:
    production_quic_workers(std::span<const ruvia::detail::http_server_listener_definition> listeners,
        const ruvia::detail::route_table& routes_value)
        : configuration_(ruvia::detail::validate_http_server_configuration(listeners,
              {.worker_queue_capacity_ = 32, .max_connections_ = 4, .max_requests_per_connection_ = 64})),
          first_(configuration_, routes_value, {}),
          second_(configuration_, routes_value, {}),
          targets_{production_quic_target(first_), production_quic_target(second_)},
          ingress_(listeners, targets_) {
        try {
            first_.prepare();
            second_.prepare();
            ingress_.prepare();
            ingress_.launch();
            ingress_.wait_until_ready();
            first_.launch();
            second_.launch();
            first_.wait_until_ready();
            second_.wait_until_ready();
        } catch (...) {
            stop();
            throw;
        }
    }
    ~production_quic_workers() {
        stop();
    }

    void serve() {
        first_.request_serve();
        second_.request_serve();
        if (!first_.wait_until_serving() || !second_.wait_until_serving()) {
            throw std::runtime_error("production QUIC worker stopped before serving");
        }
        ingress_.request_serve();
        if (!ingress_.wait_until_serving()) {
            throw std::runtime_error("production QUIC acceptor stopped before serving");
        }
    }
    void stop() noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        first_.stop_admission();
        second_.stop_admission();
        ingress_.stop();
        try {
            ingress_.join();
        } catch (...) {
            failure_ = std::current_exception();
        }
        first_.finalize_after_network_quiesced();
        second_.finalize_after_network_quiesced();
        try {
            first_.join();
        } catch (...) {
            failure_ = std::current_exception();
        }
        try {
            second_.join();
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
    void rethrow_failure() const {
        ingress_.rethrow_failure();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    ruvia::detail::validated_http_server_configuration configuration_;
    ruvia::detail::web_worker_runtime first_;
    ruvia::detail::web_worker_runtime second_;
    std::array<ruvia::detail::acceptor::worker_target, 2> targets_;
    ruvia::detail::acceptor ingress_;
    std::exception_ptr failure_;
    bool stopped_{};
};

std::thread::id production_worker_thread(ruvia::detail::web_worker_runtime& worker_value) {
    std::promise<std::thread::id> promise;
    auto result_value = promise.get_future();
    asio::post(worker_value.worker_executor(), [&] { promise.set_value(std::this_thread::get_id()); });
    if (result_value.wait_for(5s) != std::future_status::ready) {
        std::terminate();
    }
    return result_value.get();
}

struct partition_route_state final {
    std::mutex mutex_;
    std::array<std::thread::id, 2> threads_;
    std::array<std::vector<std::thread::id>, 2> requests_;
};

ruvia::task<ruvia::http_response> partition_identity_response(void* raw, ruvia::context& context_value) {
    require_loopback_peer(context_value);
    auto& state_value = *static_cast<partition_route_state*>(raw);
    const auto target = context_value.req().path() == "/partition/0" ? 0U : 1U;
    const auto current = std::this_thread::get_id();
    std::lock_guard lock(state_value.mutex_);
    state_value.requests_[target].push_back(current);
    const auto owner_value = current == state_value.threads_[0] ? 0U : current == state_value.threads_[1] ? 1U
                                                                                                          : 2U;
    constexpr std::string_view bodies[]{"worker-0", "worker-1", "worker-2"};
    co_return context_value.text(bodies[owner_value]);
}

class partition_quic_client final {
public:
    partition_quic_client(asio::io_context& io,
        ruvia::detail::http3_quic_client_tls_context& tls, asio::ip::udp::endpoint server,
        std::uint32_t partition, std::pmr::memory_resource* resource)
        : socket_(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          transport_(tls, configuration(socket_.local_endpoint(), server, partition),
              "127.0.0.1", std::chrono::steady_clock::now(), resource) {
        socket_.non_blocking(true);
    }
    ~partition_quic_client() {
        transport_.close();
    }

    static ruvia::quic_connection_config configuration(const asio::ip::udp::endpoint& local,
        const asio::ip::udp::endpoint& server, std::uint32_t partition) {
        ruvia::quic_connection_config config;
        config.local_address_ = ruvia::detail::to_quic_address(
            std::get<0>(ruvia::detail::to_http3_quic_datagram_address(local)));
        config.peer_address_ = ruvia::detail::to_quic_address(
            std::get<0>(ruvia::detail::to_http3_quic_datagram_address(server)));
        std::array<std::byte, 16> cid{};
        cid[3] = static_cast<std::byte>(partition);
        cid[15] = std::byte{0xa7};
        config.destination_connection_id_ = ruvia::quic_connection_id(cid);
        return config;
    }
    void pump() {
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = transport_.write_packet(buffer_, now);
            if (packet.size_ == 0) {
                break;
            }
            const auto destination = ruvia::detail::to_udp_endpoint(
                ruvia::detail::from_quic_address(packet.peer_));
            if ((destination.index() != 0)) {
                throw std::runtime_error("partition client lost packet destination");
            }
            socket_.send_to(asio::buffer(buffer_.data(), packet.size_), std::get<0>(destination));
        }
        for (std::size_t count = 0; count < 32; ++count) {
            asio::ip::udp::endpoint source;
            asio::error_code error;
            const auto size = socket_.receive_from(asio::buffer(buffer_), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                throw std::system_error(error, "receive partition client");
            }
            const auto local = ruvia::detail::to_quic_address(
                std::get<0>(ruvia::detail::to_http3_quic_datagram_address(socket_.local_endpoint())));
            const auto peer = ruvia::detail::to_quic_address(
                std::get<0>(ruvia::detail::to_http3_quic_datagram_address(source)));
            static_cast<void>(transport_.receive(
                {std::span<const std::byte>(buffer_).first(size), local, peer}, now));
        }
        if (const auto expiry = transport_.next_expiry(); expiry && *expiry <= now) {
            static_cast<void>(transport_.handle_expiry(now));
        }
        if (transport_.connection().info().state_ == ruvia::quic_connection_state::failed) {
            throw std::runtime_error("partition client QUIC/TLS failed");
        }
    }

    template <typename predicate_type>
    void drive_until(predicate_type predicate) {
        const auto deadline_value = std::chrono::steady_clock::now() + 5s;
        while (!predicate()) {
            pump();
            if (std::chrono::steady_clock::now() >= deadline_value) {
                throw std::runtime_error("production partition client timed out");
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    void handshake() {
        drive_until([&] { return transport_.connection().info().quic_handshake_complete_; });
        const auto prefixes = ruvia::http3_local_critical_streams::create();
        if ((prefixes.index() != 0)) {
            throw std::runtime_error("partition client critical stream encoding failed");
        }
        for (const auto prefix : {std::get<0>(prefixes).control_prefix(), std::get<0>(prefixes).qpack_encoder_prefix(),
                 std::get<0>(prefixes).qpack_decoder_prefix()}) {
            const auto opened = transport_.connection().open_stream(true);
            if (opened.status_ != ruvia::quic_operation_status::accepted) {
                throw std::runtime_error("partition client could not open critical stream");
            }
            write_stream(opened.stream_id_, prefix, false);
        }
    }
    void write_stream(std::uint64_t id, std::span<const char> bytes_value, bool fin) {
        std::size_t offset{};
        drive_until([&] {
            const auto written = write_quic_stream(transport_.connection(), id, bytes_value.subspan(offset), fin);
            offset += written.accepted_;
            return offset == bytes_value.size();
        });
    }
    std::uint64_t request(std::uint32_t partition) {
        const auto opened = transport_.connection().open_stream(false);
        if (opened.status_ != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("partition client could not open request stream");
        }
        const auto head = ruvia::encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "127.0.0.1", .path_ = partition == 0 ? "/partition/0" : "/partition/1"});
        if ((head.index() != 0)) {
            throw std::runtime_error("partition client could not encode request");
        }
        const auto wire = test_http3_frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
            std::get<0>(head).field_section_);
        write_stream(opened.stream_id_, wire, true);
        return opened.stream_id_;
    }
    std::string response(std::uint64_t id) {
        std::string wire;
        std::array<char, 4096> input{};
        drive_until([&] {
            const auto read = read_quic_stream(transport_.connection(), id, input);
            wire.append(input.data(), read.size_);
            if (read.status_ == ruvia::quic_stream_read_status::reset ||
                read.status_ == ruvia::quic_stream_read_status::closed) {
                throw std::runtime_error("partition response was reset");
            }
            return read.status_ == ruvia::quic_stream_read_status::fin;
        });
        return wire;
    }

    asio::ip::udp::socket socket_;
    ruvia::detail::http3_quic_client_transport transport_;
    std::array<std::byte, 65536> buffer_{};
};
}  // namespace

RUVIA_TEST(http3_production_cid_partitions_keep_multiple_streams_on_their_worker_and_stop_live_connections) {
    test_identity_files identity;
    partition_route_state observations;
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    for (const auto* path : {"/partition/0", "/partition/1"}) {
        routes_value.register_route(ruvia::http_known_method::get, std::pmr::string(path),
            {&observations, partition_identity_response}, ruvia::detail::request_body_mode::buffered, {}, {});
    }
    routes_value.finalize();
    ruvia::detail::http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = identity.certificate().string();
    tls.identity_.private_key_file_ = identity.private_key().string();
    const std::array listeners{ruvia::detail::http_server_listener_definition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::http3_listen_config{.drain_timeout_ = 250ms})};
    production_quic_workers server(listeners, routes_value.route_table());
    server.serve();
    {
        std::lock_guard lock(observations.mutex_);
        observations.threads_ = {production_worker_thread(server.first_), production_worker_thread(server.second_)};
    }
    RUVIA_CHECK(observations.threads_[0] != observations.threads_[1]);
    auto& io = ruvia::test::new_test_io_context();
    const auto certificate = identity.certificate().string();
    ruvia::detail::http3_quic_client_tls_context client_tls({.ca_file_ = certificate});
    counting_resource first_memory;
    counting_resource second_memory;
    {
        const asio::ip::udp::endpoint endpoint(asio::ip::address_v4::loopback(),
            server.ingress_.local_endpoint(0).port());
        partition_quic_client first(io, client_tls, endpoint, 0, &first_memory);
        partition_quic_client second(io, client_tls, endpoint, 1, &second_memory);
        first.handshake();
        second.handshake();
        const std::array first_streams{first.request(0), first.request(0)};
        const std::array second_streams{second.request(1), second.request(1)};
        for (const auto stream : first_streams) {
            RUVIA_CHECK(first.response(stream).find("worker-0") != std::string::npos);
        }
        for (const auto stream : second_streams) {
            RUVIA_CHECK(second.response(stream).find("worker-1") != std::string::npos);
        }
        {
            std::lock_guard lock(observations.mutex_);
            for (std::size_t partition = 0; partition < 2; ++partition) {
                RUVIA_CHECK_EQ(observations.requests_[partition].size(), std::size_t{2});
                RUVIA_CHECK(std::ranges::all_of(observations.requests_[partition],
                    [&](std::thread::id owner_value) { return owner_value == observations.threads_[partition]; }));
            }
        }
        // Peer-driven retirement must release its worker slot without killing
        // the worker; a new connection in that partition must still serve.
        RUVIA_CHECK_EQ(server.first_.stats().active_connections_, std::size_t{1});
        const auto closed = first.transport_.connection().close({
            .kind_ = ruvia::quic_close_kind::application,
            .code_ = 0,
        });
        RUVIA_CHECK(closed == ruvia::quic_operation_status::accepted ||
                    closed == ruvia::quic_operation_status::completed ||
                    closed == ruvia::quic_operation_status::closing);
        const auto retirement_deadline = std::chrono::steady_clock::now() + 5s;
        while (server.first_.stats().active_connections_ != 0 &&
               std::chrono::steady_clock::now() < retirement_deadline) {
            first.pump();
            second.pump();
            std::this_thread::sleep_for(1ms);
        }
        RUVIA_CHECK_EQ(server.first_.stats().active_connections_, std::size_t{0});
        server.rethrow_failure();
        partition_quic_client replacement(io, client_tls, endpoint, 0, &first_memory);
        replacement.handshake();
        RUVIA_CHECK(replacement.response(replacement.request(0)).find("worker-0") != std::string::npos);
        // Stop the real ingress while both authenticated QUIC connections are
        // still live, not after client shutdown has hidden outstanding borrows.
        server.stop();
        server.rethrow_failure();
    }
    RUVIA_CHECK_EQ(first_memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(first_memory.allocations_, first_memory.returns_);
    RUVIA_CHECK_EQ(second_memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(second_memory.allocations_, second_memory.returns_);
}

RUVIA_TEST(http3_production_workers_cancel_before_serve_and_rollback_tls_startup_failure) {
    test_identity_files identity;
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    routes_value.finalize();
    ruvia::detail::http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = identity.certificate().string();
    tls.identity_.private_key_file_ = identity.private_key().string();
    const std::array listeners{ruvia::detail::http_server_listener_definition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::http3_listen_config{})};
    {
        production_quic_workers server(listeners, routes_value.route_table());
        server.stop();
        server.rethrow_failure();
        RUVIA_CHECK_EQ(server.first_.stats().active_connections_, std::size_t{0});
        RUVIA_CHECK_EQ(server.second_.stats().active_connections_, std::size_t{0});
    }
    // Validation succeeds, but the worker's real TLS initialization fails.
    // The staged packet channel must still ACK and let ingress.join return.
    auto invalid_listeners = listeners;
    std::filesystem::remove(identity.private_key());
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        production_quic_workers server(invalid_listeners, routes_value.route_table());
    }));
}
