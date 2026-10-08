#include "http3_client_connection_fixture.h"

namespace {

void require_loopback_peer(ruvia::Context& context) {
    const auto info = context.conn();
    if (info.remote().address() != "127.0.0.1" || info.remote().port() == 0 ||
        info.tls() == nullptr || info.scheme() != ruvia::HttpScheme::kHttps) {
        throw std::runtime_error("HTTP/3 tunnel lost its peer metadata during worker handoff");
    }
}

ruvia::Task<void> nativeDatagramEcho(void*, ruvia::Context& context) {
    require_loopback_peer(context);
    auto datagrams = context.tunnel().datagrams();
    while (auto packet = co_await datagrams.read()) {
        const auto payload = packet->payload();
        co_await datagrams.send(payload);
    }
    co_await datagrams.finish();
}

ruvia::Task<void> nativeUdpEcho(void*, ruvia::Context& context) {
    require_loopback_peer(context);
    ruvia::HttpUdpTunnel datagrams(context.tunnel().datagrams());
    while (auto packet = co_await datagrams.read()) {
        const auto payload = packet->payload();
        co_await datagrams.send(payload);
    }
    co_await datagrams.finish();
}

}  // namespace

RUVIA_TEST(http3_native_datagram_server_and_client_route_packets_capsules_verify_tls_and_retire_fixed_workers) {
    TestIdentityFiles identity;
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    routes.registerTunnelRoute("test-datagram", std::pmr::string("/datagrams"), {nullptr, nativeDatagramEcho}, {}, {}, {.datagrams = true});
    routes.registerTunnelRoute("connect-udp", std::pmr::string("/udp/:host/:port"), {nullptr, nativeUdpEcho}, {}, {});
    routes.finalize();
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = identity.certificate().string();
    tls.identity.privateKeyFile = identity.privateKey().string();
    const std::array listeners{ruvia::detail::HttpServerListenerDefinition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::Http3ListenConfig{})};
    auto configuration = ruvia::detail::validateHttpServerConfiguration(listeners,
        {.worker_queue_capacity = 32, .maxConnections = 4, .max_requests_per_connection = 64});
    ruvia::detail::WebWorkerRuntime first(configuration, routes.routeTable(), {});
    ruvia::detail::WebWorkerRuntime second(configuration, routes.routeTable(), {});
    first.prepare();
    second.prepare();
    const auto target = [](ruvia::detail::WebWorkerRuntime& worker) {
        return ruvia::detail::acceptor::worker_target{
            .submission = worker.networkSubmission(),
            .object = &worker,
            .available = [](void* raw) noexcept { return static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->availableForNetworkDispatch(); },
            .accept = [](void* raw, ruvia::detail::NativeAcceptedSocketTicket&& ticket) noexcept { static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->acceptTransferredConnection(std::move(ticket)); },
            .stage_quic = [](void* raw, ruvia::detail::http3_datagram_channel& channel,
                              asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) { static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->stage_quic(channel, endpoint, partition); }};
    };
    const std::array targets{target(first), target(second)};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    first.launch();
    second.launch();
    first.waitUntilReady();
    second.waitUntilReady();
    first.requestServe();
    second.requestServe();
    RUVIA_CHECK(first.waitUntilServing());
    RUVIA_CHECK(second.waitUntilServing());
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    std::exception_ptr failure;
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        std::optional<ruvia::HttpDatagram> retained;
        {
            ruvia::HttpClient client(attachment.loop(), {.host = "127.0.0.1", .port = network.local_endpoint(0).port(), .requestTimeout = 5s, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
            try {
                auto opened = co_await client.openTunnel({.authority = "target.test:443", .protocol = "test-datagram", .target = "/datagrams"}, {.datagrams = true});
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
                RUVIA_CHECK(retained && retained->transport() == ruvia::HttpDatagramTransport::kQuic);
                RUVIA_CHECK(retained && std::ranges::all_of(retained->payload(), [](std::byte b) { return b == std::byte{'n'}; }));
                co_await datagrams.send("");
                auto empty = co_await datagrams.read();
                RUVIA_CHECK(empty && empty->payload().empty() && empty->transport() == ruvia::HttpDatagramTransport::kQuic);
                co_await datagrams.send(std::string(16003, 'c'));
                auto reliable = co_await datagrams.read();
                RUVIA_CHECK(reliable && reliable->payload().size() == 16003 && reliable->transport() == ruvia::HttpDatagramTransport::kCapsule);
                co_await datagrams.finish();
                RUVIA_CHECK(!(co_await datagrams.read()));
                auto udpOpened = co_await client.openUdpTunnel({.target = "/udp/target.test/443"});
                if (!udpOpened.tunnel()) {
                    throw std::runtime_error("native CONNECT-UDP rejected");
                }
                auto udp = std::move(*udpOpened.tunnel()).udp();
                co_await udp.send("native UDP");
                auto packet = co_await udp.read();
                RUVIA_CHECK(packet && packet->payload().size() == 10 && packet->transport() == ruvia::HttpDatagramTransport::kQuic);
                co_await udp.send("");
                auto udpEmpty = co_await udp.read();
                RUVIA_CHECK(udpEmpty && udpEmpty->payload().empty());
                co_await udp.finish();
                RUVIA_CHECK(!(co_await udp.read()));
                auto pendingOpened = co_await client.openUdpTunnel({.target = "/udp/target.test/443"});
                if (!pendingOpened.tunnel()) {
                    throw std::runtime_error("pending native CONNECT-UDP rejected");
                }
                auto pending = std::move(*pendingOpened.tunnel()).udp();
                ruvia::TaskScope reads(worker);
                bool cancelled{};
                auto read = [&]() -> ruvia::Task<void> {try {static_cast<void>(co_await pending.read());} catch(const ruvia::HttpClientError&) {cancelled=true;} };
                reads.spawn(read());
                co_await ruvia::sleepFor(worker, 1ms);
                pending.abort();
                co_await reads.join();
                RUVIA_CHECK(cancelled);
                // Retain the independent public-client cancellation/echo path.
                ruvia::HttpClient other(attachment.loop(), {.host = "127.0.0.1", .port = network.local_endpoint(0).port(), .requestTimeout = 5s, .protocol = ruvia::HttpClientProtocol::kHttp3Only, .caFile = identity.certificate().string()});
                try {
                    auto openedOther = co_await other.openUdpTunnel({.target = "/udp/target.test/443"});
                    if (!openedOther.tunnel()) {
                        throw std::runtime_error("second worker UDP rejected");
                    }
                    auto packets = std::move(*openedOther.tunnel()).udp();
                    co_await packets.send("second worker");
                    auto result = co_await packets.read();
                    RUVIA_CHECK(result && result->payload().size() == 13);
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
    first.stopAdmission();
    second.stopAdmission();
    network.stop();
    network.join();
    first.finalizeAfterNetworkQuiesced();
    second.finalizeAfterNetworkQuiesced();
    first.join();
    second.join();
    network.rethrow_failure();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace {
ruvia::detail::acceptor::worker_target production_quic_target(ruvia::detail::WebWorkerRuntime& worker) {
    return {.submission = worker.networkSubmission(),
        .object = &worker,
        .available = [](void* raw) noexcept { return static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->availableForNetworkDispatch(); },
        .accept = [](void* raw, ruvia::detail::NativeAcceptedSocketTicket&& ticket) noexcept { static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->acceptTransferredConnection(std::move(ticket)); },
        .stage_quic = [](void* raw, ruvia::detail::http3_datagram_channel& channel,
                          asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) { static_cast<ruvia::detail::WebWorkerRuntime*>(raw)->stage_quic(channel, endpoint, partition); }};
}

class production_quic_workers final {
public:
    production_quic_workers(std::span<const ruvia::detail::HttpServerListenerDefinition> listeners,
        const ruvia::detail::RouteTable& routes)
        : configuration(ruvia::detail::validateHttpServerConfiguration(listeners,
              {.worker_queue_capacity = 32, .maxConnections = 4, .max_requests_per_connection = 64})),
          first(configuration, routes, {}),
          second(configuration, routes, {}),
          targets{production_quic_target(first), production_quic_target(second)},
          ingress(listeners, targets) {
        try {
            first.prepare();
            second.prepare();
            ingress.prepare();
            ingress.launch();
            ingress.wait_until_ready();
            first.launch();
            second.launch();
            first.waitUntilReady();
            second.waitUntilReady();
        } catch (...) {
            stop();
            throw;
        }
    }
    ~production_quic_workers() {
        stop();
    }

    void serve() {
        first.requestServe();
        second.requestServe();
        if (!first.waitUntilServing() || !second.waitUntilServing()) {
            throw std::runtime_error("production QUIC worker stopped before serving");
        }
        ingress.request_serve();
        if (!ingress.wait_until_serving()) {
            throw std::runtime_error("production QUIC acceptor stopped before serving");
        }
    }
    void stop() noexcept {
        if (stopped) {
            return;
        }
        stopped = true;
        first.stopAdmission();
        second.stopAdmission();
        ingress.stop();
        try {
            ingress.join();
        } catch (...) {
            failure = std::current_exception();
        }
        first.finalizeAfterNetworkQuiesced();
        second.finalizeAfterNetworkQuiesced();
        try {
            first.join();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            second.join();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    void rethrow_failure() const {
        ingress.rethrow_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::detail::ValidatedHttpServerConfiguration configuration;
    ruvia::detail::WebWorkerRuntime first;
    ruvia::detail::WebWorkerRuntime second;
    std::array<ruvia::detail::acceptor::worker_target, 2> targets;
    ruvia::detail::acceptor ingress;
    std::exception_ptr failure;
    bool stopped{};
};

std::thread::id production_worker_thread(ruvia::detail::WebWorkerRuntime& worker) {
    std::promise<std::thread::id> promise;
    auto result = promise.get_future();
    asio::post(worker.workerExecutor(), [&] { promise.set_value(std::this_thread::get_id()); });
    if (result.wait_for(5s) != std::future_status::ready) {
        std::terminate();
    }
    return result.get();
}

struct partition_route_state final {
    std::mutex mutex;
    std::array<std::thread::id, 2> threads;
    std::array<std::vector<std::thread::id>, 2> requests;
};

ruvia::Task<ruvia::HttpResponse> partition_identity_response(void* raw, ruvia::Context& context) {
    require_loopback_peer(context);
    auto& state = *static_cast<partition_route_state*>(raw);
    const auto target = context.req().path() == "/partition/0" ? 0U : 1U;
    const auto current = std::this_thread::get_id();
    std::lock_guard lock(state.mutex);
    state.requests[target].push_back(current);
    const auto owner = current == state.threads[0] ? 0U : current == state.threads[1] ? 1U
                                                                                      : 2U;
    constexpr std::string_view bodies[]{"worker-0", "worker-1", "worker-2"};
    co_return context.text(bodies[owner]);
}

class partition_quic_client final {
public:
    partition_quic_client(asio::io_context& io,
        ruvia::detail::http3_quic_client_tls_context& tls, asio::ip::udp::endpoint server,
        std::uint32_t partition, std::pmr::memory_resource* resource)
        : socket(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0)),
          transport(tls, configuration(socket.local_endpoint(), server, partition),
              "127.0.0.1", std::chrono::steady_clock::now(), resource) {
        socket.non_blocking(true);
    }
    ~partition_quic_client() {
        transport.close();
    }

    static ruvia::quic_connection_config configuration(const asio::ip::udp::endpoint& local,
        const asio::ip::udp::endpoint& server, std::uint32_t partition) {
        ruvia::quic_connection_config config;
        config.local_address = ruvia::detail::to_quic_address(
            *ruvia::detail::to_http3_quic_datagram_address(local));
        config.peer_address = ruvia::detail::to_quic_address(
            *ruvia::detail::to_http3_quic_datagram_address(server));
        std::array<std::byte, 16> cid{};
        cid[3] = static_cast<std::byte>(partition);
        cid[15] = std::byte{0xa7};
        config.destination_connection_id = ruvia::quic_connection_id(cid);
        return config;
    }
    void pump() {
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t count = 0; count < 32; ++count) {
            const auto packet = transport.write_packet(buffer, now);
            if (packet.size == 0) {
                break;
            }
            const auto destination = ruvia::detail::to_udp_endpoint(
                ruvia::detail::from_quic_address(packet.peer));
            if (!destination) {
                throw std::runtime_error("partition client lost packet destination");
            }
            socket.send_to(asio::buffer(buffer.data(), packet.size), *destination);
        }
        for (std::size_t count = 0; count < 32; ++count) {
            asio::ip::udp::endpoint source;
            asio::error_code error;
            const auto size = socket.receive_from(asio::buffer(buffer), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                throw std::system_error(error, "receive partition client");
            }
            const auto local = ruvia::detail::to_quic_address(
                *ruvia::detail::to_http3_quic_datagram_address(socket.local_endpoint()));
            const auto peer = ruvia::detail::to_quic_address(
                *ruvia::detail::to_http3_quic_datagram_address(source));
            static_cast<void>(transport.receive(
                {std::span<const std::byte>(buffer).first(size), local, peer}, now));
        }
        if (const auto expiry = transport.next_expiry(); expiry && *expiry <= now) {
            static_cast<void>(transport.handle_expiry(now));
        }
        if (transport.connection().info().state == ruvia::quic_connection_state::failed) {
            throw std::runtime_error("partition client QUIC/TLS failed");
        }
    }

    template <typename Predicate>
    void drive_until(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!predicate()) {
            pump();
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("production partition client timed out");
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    void handshake() {
        drive_until([&] { return transport.connection().info().quic_handshake_complete; });
        const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
        if (!prefixes) {
            throw std::runtime_error("partition client critical stream encoding failed");
        }
        for (const auto prefix : {prefixes->controlPrefix(), prefixes->qpackEncoderPrefix(),
                 prefixes->qpackDecoderPrefix()}) {
            const auto opened = transport.connection().open_stream(true);
            if (opened.status != ruvia::quic_operation_status::accepted) {
                throw std::runtime_error("partition client could not open critical stream");
            }
            write_stream(opened.stream_id, prefix, false);
        }
    }
    void write_stream(std::uint64_t id, std::span<const char> bytes, bool fin) {
        std::size_t offset{};
        drive_until([&] {
            const auto written = write_quic_stream(transport.connection(), id, bytes.subspan(offset), fin);
            offset += written.accepted;
            return offset == bytes.size();
        });
    }
    std::uint64_t request(std::uint32_t partition) {
        const auto opened = transport.connection().open_stream(false);
        if (opened.status != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("partition client could not open request stream");
        }
        const auto head = ruvia::encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "127.0.0.1", .path = partition == 0 ? "/partition/0" : "/partition/1"});
        if (!head) {
            throw std::runtime_error("partition client could not encode request");
        }
        const auto wire = test_http3_frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
            head->fieldSection);
        write_stream(opened.stream_id, wire, true);
        return opened.stream_id;
    }
    std::string response(std::uint64_t id) {
        std::string wire;
        std::array<char, 4096> input{};
        drive_until([&] {
            const auto read = read_quic_stream(transport.connection(), id, input);
            wire.append(input.data(), read.size);
            if (read.status == ruvia::quic_stream_read_status::reset ||
                read.status == ruvia::quic_stream_read_status::closed) {
                throw std::runtime_error("partition response was reset");
            }
            return read.status == ruvia::quic_stream_read_status::fin;
        });
        return wire;
    }

    asio::ip::udp::socket socket;
    ruvia::detail::http3_quic_client_transport transport;
    std::array<std::byte, 65536> buffer{};
};
}  // namespace

RUVIA_TEST(http3_production_cid_partitions_keep_multiple_streams_on_their_worker_and_stop_live_connections) {
    TestIdentityFiles identity;
    partition_route_state observations;
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    for (const auto* path : {"/partition/0", "/partition/1"}) {
        routes.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string(path),
            {&observations, partition_identity_response}, ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    }
    routes.finalize();
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = identity.certificate().string();
    tls.identity.privateKeyFile = identity.privateKey().string();
    const std::array listeners{ruvia::detail::HttpServerListenerDefinition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::Http3ListenConfig{.drainTimeout = 250ms})};
    production_quic_workers server(listeners, routes.routeTable());
    server.serve();
    {
        std::lock_guard lock(observations.mutex);
        observations.threads = {production_worker_thread(server.first), production_worker_thread(server.second)};
    }
    RUVIA_CHECK(observations.threads[0] != observations.threads[1]);
    auto& io = ruvia::test::newTestIoContext();
    const auto certificate = identity.certificate().string();
    ruvia::detail::http3_quic_client_tls_context client_tls({.caFile = certificate});
    CountingResource first_memory;
    CountingResource second_memory;
    {
        const asio::ip::udp::endpoint endpoint(asio::ip::address_v4::loopback(),
            server.ingress.local_endpoint(0).port());
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
            std::lock_guard lock(observations.mutex);
            for (std::size_t partition = 0; partition < 2; ++partition) {
                RUVIA_CHECK_EQ(observations.requests[partition].size(), std::size_t{2});
                RUVIA_CHECK(std::ranges::all_of(observations.requests[partition],
                    [&](std::thread::id owner) { return owner == observations.threads[partition]; }));
            }
        }
        // Peer-driven retirement must release its worker slot without killing
        // the worker; a new connection in that partition must still serve.
        RUVIA_CHECK_EQ(server.first.stats().activeConnections, std::size_t{1});
        const auto closed = first.transport.connection().close({
            .kind = ruvia::quic_close_kind::application,
            .code = 0,
        });
        RUVIA_CHECK(closed == ruvia::quic_operation_status::accepted ||
                    closed == ruvia::quic_operation_status::completed ||
                    closed == ruvia::quic_operation_status::closing);
        const auto retirement_deadline = std::chrono::steady_clock::now() + 5s;
        while (server.first.stats().activeConnections != 0 &&
               std::chrono::steady_clock::now() < retirement_deadline) {
            first.pump();
            second.pump();
            std::this_thread::sleep_for(1ms);
        }
        RUVIA_CHECK_EQ(server.first.stats().activeConnections, std::size_t{0});
        server.rethrow_failure();
        partition_quic_client replacement(io, client_tls, endpoint, 0, &first_memory);
        replacement.handshake();
        RUVIA_CHECK(replacement.response(replacement.request(0)).find("worker-0") != std::string::npos);
        // Stop the real ingress while both authenticated QUIC connections are
        // still live, not after client shutdown has hidden outstanding borrows.
        server.stop();
        server.rethrow_failure();
    }
    RUVIA_CHECK_EQ(first_memory.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(first_memory.allocations, first_memory.returns);
    RUVIA_CHECK_EQ(second_memory.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(second_memory.allocations, second_memory.returns);
}

RUVIA_TEST(http3_production_workers_cancel_before_serve_and_rollback_tls_startup_failure) {
    TestIdentityFiles identity;
    ruvia::detail::Router router;
    auto& routes = ruvia::detail::RouterImpl::from(router);
    routes.finalize();
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = identity.certificate().string();
    tls.identity.privateKeyFile = identity.privateKey().string();
    const std::array listeners{ruvia::detail::HttpServerListenerDefinition(
        {asio::ip::address_v4::loopback(), 0}, std::move(tls), ruvia::Http3ListenConfig{})};
    {
        production_quic_workers server(listeners, routes.routeTable());
        server.stop();
        server.rethrow_failure();
        RUVIA_CHECK_EQ(server.first.stats().activeConnections, std::size_t{0});
        RUVIA_CHECK_EQ(server.second.stats().activeConnections, std::size_t{0});
    }
    // Validation succeeds, but the worker's real TLS initialization fails.
    // The staged packet channel must still ACK and let ingress.join return.
    auto invalid_listeners = listeners;
    std::filesystem::remove(identity.privateKey());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        production_quic_workers server(invalid_listeners, routes.routeTable());
    }));
}
