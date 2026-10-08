#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/http/quic_server.h"

#include "client/ClientTransport.h"
#include "http3/Http3QuicClientTlsContext.h"
#include "http3/Http3QuicClientTransport.h"
#include "http3/Http3QuicSocketAddress.h"
#include "http3/Http3QuicWireOwner.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_worker_runtime.h"
#include "http3/http3_worker_server.h"
#include "integration/WorkerCapabilities.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptions.h"
#include "test_harness.h"

namespace {
using namespace std::chrono_literals;
using Udp = asio::ip::udp;
using ruvia::detail::http3_quic_client_tls_context;
using ruvia::detail::http3_quic_client_transport;
using ruvia::detail::http3_quic_datagram_address;
using ruvia::detail::http3_quic_server_transport;
using ruvia::detail::http3_quic_tls_context;
using ruvia::detail::http3_worker_datagram_endpoint;
using ruvia::detail::Http3QuicWireOwner;

Http3QuicWireOwner::ProtocolPumpResult fail_protocol_pump(
    void*, http3_quic_server_transport&, http3_worker_datagram_endpoint&) noexcept {
    return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
}

class FailOnAllocationResource final : public std::pmr::memory_resource {
public:
    explicit FailOnAllocationResource(std::size_t failAt) noexcept
        : failAt_(failAt) {}
    [[nodiscard]] std::size_t allocationAttempts() const noexcept {
        return attempts_;
    }
    [[nodiscard]] std::size_t outstandingAllocations() const noexcept {
        return outstanding_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // MSVC debug containers allocate small iterator proxies in noexcept
        // constructors. Inject failure into payload storage, not those proxies.
        if (bytes >= 32 && ++attempts_ == failAt_) {
            throw std::bad_alloc();
        }
        ++outstanding_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* allocation, std::size_t bytes, std::size_t alignment) override {
        --outstanding_;
        std::pmr::new_delete_resource()->deallocate(allocation, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t failAt_{};
    std::size_t attempts_{};
    std::size_t outstanding_{};
};

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
struct version_negotiation_pump_state final {
    bool routed_{};
    bool sent_{};
    bool plan_consumed_{};
    std::exception_ptr failure_;
};

Http3QuicWireOwner::ProtocolPumpResult pump_version_negotiation(void* context,
    http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
    auto& state = *static_cast<version_negotiation_pump_state*>(context);
    try {
        const auto received = endpoint.receive_slot();
        if (!received) {
            return Http3QuicWireOwner::ProtocolPumpResult::kIdle;
        }
        const auto local = ruvia::detail::to_http3_quic_datagram_address(
            received->local_destination);
        const auto peer = ruvia::detail::to_http3_quic_datagram_address(received->peer);
        if (!local || !peer) {
            throw std::runtime_error("test QUIC datagram has invalid endpoint addresses");
        }
        auto route = transport.route_datagram(received->bytes, *local, *peer);
        state.routed_ = route.kind == ruvia::quic_server_route_kind::version_negotiation;
        if (state.routed_) {
            const auto result = ruvia::detail::send_http3_version_negotiation(
                transport.server(), route.version_negotiation, endpoint);
            state.sent_ = result == http3_worker_datagram_endpoint::pump_result::pending;
            state.plan_consumed_ = !route.version_negotiation.valid();
        }
        if (endpoint.consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
            throw std::system_error(endpoint.error(), "consume test QUIC datagram");
        }
        return Http3QuicWireOwner::ProtocolPumpResult::kProgress;
    } catch (...) {
        state.failure_ = std::current_exception();
        return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
    }
}

struct connection_pump_state final {
    std::optional<ruvia::quic_connection_token> connection;
    std::size_t full_drives{};
    std::size_t full_receives{};
    std::size_t full_expirations{};
    std::size_t full_expirations_with_deadline{};
    std::optional<ruvia::quic_timestamp> last_full_expiry;
    bool repeated_full_expiry{};
    bool full_idle_retired{};
    std::size_t packet_writes{};
    std::size_t packets_sent{};
    std::exception_ptr failure;
};

Http3QuicWireOwner::ProtocolPumpResult pump_connection(void* context,
    http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
    auto& state = *static_cast<connection_pump_state*>(context);
    try {
        const auto now = std::chrono::steady_clock::now();
        const bool full = endpoint.outbound_pending() && !endpoint.outbound_capacity();
        state.full_drives += full;
        const auto expiry = transport.server().next_expiry();
        const bool expired_while_full = full && expiry && *expiry <= now;
        (void)transport.server().handle_expiry(now);
        if (expired_while_full) {
            state.repeated_full_expiry |= state.last_full_expiry && *expiry <= *state.last_full_expiry;
            state.last_full_expiry = expiry;
            ++state.full_expirations;
            const auto next = transport.server().next_expiry();
            state.full_expirations_with_deadline += next.has_value();
            state.repeated_full_expiry |= next && *next <= *expiry;
            state.full_idle_retired |= state.connection &&
                                       transport.server().connection(*state.connection).info().state == ruvia::quic_connection_state::retired;
        }
        bool progress{};
        if (const auto received = endpoint.receive_slot()) {
            const auto local = ruvia::detail::to_http3_quic_datagram_address(received->local_destination);
            const auto peer = ruvia::detail::to_http3_quic_datagram_address(received->peer);
            if (!local || !peer) {
                throw std::runtime_error("test QUIC datagram has invalid endpoint addresses");
            }
            auto route = transport.route_datagram(received->bytes, *local, *peer);
            if (route.kind == ruvia::quic_server_route_kind::initial_offer && !state.connection) {
                const auto admitted = transport.admit_initial(route.offer, now);
                if (admitted.status == ruvia::quic_operation_status::accepted) {
                    state.connection = admitted.connection;
                }
            } else if (route.kind == ruvia::quic_server_route_kind::existing_connection) {
                const auto status = transport.server().receive(route.connection,
                    {received->bytes, ruvia::detail::to_quic_address(*local), ruvia::detail::to_quic_address(*peer)}, now);
                if (status == ruvia::quic_operation_status::accepted ||
                    status == ruvia::quic_operation_status::need_input) {
                    state.full_receives += full;
                }
            }
            if (endpoint.consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
                throw std::system_error(endpoint.error(), "consume test QUIC datagram");
            }
            progress = true;
        }
        if (state.connection) {
            const auto output = endpoint.packet_buffer();
            if (!output.empty()) {
                ++state.packet_writes;
                const auto packet = transport.server().connection(*state.connection).write_packet(output, now);
                if (packet.size == 0) {
                    endpoint.cancel_packet();
                } else {
                    const auto source = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.local));
                    const auto peer = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.peer));
                    if (!source || !peer) {
                        throw std::runtime_error("test QUIC packet has invalid endpoint addresses");
                    }
                    if (endpoint.send_datagram(output.first(packet.size), *source, *peer) != http3_worker_datagram_endpoint::pump_result::pending) {
                        throw std::runtime_error("test QUIC endpoint did not accept reserved packet");
                    }
                    ++state.packets_sent;
                    progress = true;
                }
            }
        }
        return progress ? Http3QuicWireOwner::ProtocolPumpResult::kProgress : Http3QuicWireOwner::ProtocolPumpResult::kIdle;
    } catch (...) {
        endpoint.cancel_packet();
        state.failure = std::current_exception();
        return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
    }
}

class wire_worker final {
public:
    wire_worker() {
        owner_.start();
    }
    ~wire_worker() {
        owner_.request_stop();
        owner_.join();
    }
    [[nodiscard]] ruvia::WorkerRuntimeContext& runtime() noexcept {
        return owner_.context();
    }
    template <typename function>
    auto invoke(function operation) {
        using result = decltype(operation());
        auto task = std::make_shared<std::packaged_task<result()>>(std::move(operation));
        auto completion = task->get_future();
        if (!runtime().submission().post([task] { (*task)(); }).accepted() ||
            completion.wait_for(5s) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }

private:
    ruvia::worker_runtime owner_{{.queue_capacity = 8}};
};

struct TestIdentityFiles final {
    TestIdentityFiles() {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() /
                    ("ruvia-http3-network-wire-" + std::to_string(random()) + "-" +
                        std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) {
            throw std::runtime_error("could not create QUIC test TLS directory");
        }
        certificate = directory / "certificate.pem";
        privateKey = directory / "private-key.pem";
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* rawKey = nullptr;
        if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(keyContext.get(), &rawKey) <= 0) {
            throw std::runtime_error("could not generate QUIC test TLS key");
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        if (!cert || X509_set_version(cert.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
            X509_gmtime_adj(X509_get_notBefore(cert.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(cert.get()), 86400) == nullptr ||
            X509_set_pubkey(cert.get(), key.get()) != 1 ||
            X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get())) != 1 ||
            X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
            throw std::runtime_error("could not create QUIC test TLS certificate");
        }
        std::unique_ptr<BIO, decltype(&BIO_free)> certBio(
            BIO_new_file(certificate.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
            BIO_new_file(privateKey.string().c_str(), "w"), BIO_free);
        if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), cert.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                nullptr) != 1) {
            throw std::runtime_error("could not write QUIC test TLS identity");
        }
    }
    ~TestIdentityFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path privateKey;
};

http3_quic_datagram_address quic_address(const Udp::endpoint& endpoint) {
    const auto address = ruvia::detail::to_http3_quic_datagram_address(endpoint);
    if (!address) {
        throw std::runtime_error("test UDP endpoint cannot be converted to QUIC address");
    }
    return *address;
}

bool is_long_header_type(std::span<const std::byte> packet, std::uint8_t type) {
    if (packet.size() < 5 || (std::to_integer<std::uint8_t>(packet[0]) & 0x80U) == 0) {
        return false;
    }
    const std::uint32_t version =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[1])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[3])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[4]));
    return version == 1 && ((std::to_integer<std::uint8_t>(packet[0]) >> 4U) & 0x03U) == type;
}

ruvia::quic_connection_config client_config(Udp::socket& peer, const Udp::endpoint& server) {
    ruvia::quic_connection_config config;
    config.local_address = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
    config.peer_address = ruvia::detail::to_quic_address(quic_address(server));
    return config;
}

std::size_t pump_peer(http3_quic_client_transport& client, Udp::socket& socket) {
    const auto now = std::chrono::steady_clock::now();
    const auto check_core_state = [&client] {
        const auto state = client.connection().info().state;
        if (state == ruvia::quic_connection_state::failed ||
            state == ruvia::quic_connection_state::retired) {
            throw std::runtime_error("test QUIC client entered a terminal state");
        }
    };
    std::array<std::byte, 2048> output{};
    std::size_t sent{};
    for (std::size_t count = 0; count < 32; ++count) {
        const auto packet = client.write_packet(output, now);
        check_core_state();
        if (packet.size == 0) {
            break;
        }
        const auto destination = ruvia::detail::to_udp_endpoint(
            ruvia::detail::from_quic_address(packet.peer));
        if (!destination) {
            throw std::runtime_error("test QUIC client packet has invalid destination");
        }
        asio::error_code error;
        const auto size = socket.send_to(asio::buffer(output.data(), packet.size),
            *destination, 0, error);
        if (error || size != packet.size) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                "send test QUIC client packet");
        }
        ++sent;
    }

    std::array<std::byte, 2048> input{};
    for (;;) {
        Udp::endpoint source;
        asio::error_code error;
        const auto size = socket.receive_from(asio::buffer(input), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            break;
        }
        if (error) {
            throw std::system_error(error, "receive test QUIC client packet");
        }
        const auto local = ruvia::detail::to_quic_address(quic_address(socket.local_endpoint()));
        const auto remote = ruvia::detail::to_quic_address(quic_address(source));
        const ruvia::quic_datagram_view datagram{
            .bytes = std::span<const std::byte>(input.data(), size),
            .local = local,
            .peer = remote,
        };
        (void)client.receive(datagram, std::chrono::steady_clock::now());
        check_core_state();
    }
    const auto expiryNow = std::chrono::steady_clock::now();
    if (const auto expiry = client.next_expiry(); expiry && *expiry <= expiryNow) {
        (void)client.handle_expiry(expiryNow);
        check_core_state();
    }
    return sent;
}

// Real Acceptor/channel/worker path shared by the wire-driver tests.
class native_wire_fixture final {
public:
    native_wire_fixture(ruvia::WorkerRuntimeContext& runtime, asio::io_context& io, Udp::endpoint bind, http3_quic_tls_context& tls,
        ruvia::quic_server_config config, std::pmr::memory_resource* resource,
        Http3QuicWireOwner::ProtocolPump pump)
        : io_(io),
          runtime_(runtime),
          notification_(runtime_),
          pool_(1 + channel::default_input_capacity + channel::default_output_window, channel::packet_capacity),
          packets_(pool_, notification_),
          native_(io, bind, {nullptr, native_notification}, pool_),
          timer_(io) {
        packets_.stage_worker(runtime_);
        packets_.worker_start();
        native_.prepare();
        (void)native_.start();
        owner_ = std::make_unique<Http3QuicWireOwner>(io, packets_,
            Udp::endpoint(bind.address(), native_.bound_port()), tls, config, resource, pump);
        arm();
    }
    ~native_wire_fixture() {
        owner_->requestStop();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while ((!owner_->stopStatus().complete() || !native_.endpoint_retired()) && std::chrono::steady_clock::now() < deadline) {
            poll();
            if (io_.stopped()) {
                io_.restart();
            }
            io_.run_for(1ms);
            owner_->pollStop();
        }
        if (!owner_->stopStatus().complete() || !native_.endpoint_retired()) {
            std::terminate();
        }
        owner_.reset();
        packets_.worker_close();
        packets_.acceptor_poll();
        if (!packets_.acceptor_finalize()) {
            std::terminate();
        }
        timer_.cancel();
        if (io_.stopped()) {
            io_.restart();
        }
        io_.poll();
        notification_.close();
    }
    Http3QuicWireOwner& owner() noexcept {
        return *owner_;
    }
    bool native_retired() const noexcept {
        return native_.endpoint_retired();
    }
    void poll() noexcept {
        packets_.acceptor_poll();
        if (owner_->stopStatus().stopping) {
            packets_.acceptor_close();
            native_.request_stop();
            while (packets_.acceptor_take_output()) {
            }
        } else {
            if (auto packet = native_.take_receive()) {
                (void)packets_.acceptor_push(std::move(*packet));
            }
            native_.poll_receive();
            if (native_.outbound_capacity()) {
                if (auto packet = packets_.acceptor_take_output()) {
                    (void)native_.send_owned_datagram(std::move(*packet));
                }
            }
        }
        owner_->poll_datagrams();
    }

private:
    using channel = ruvia::detail::http3_datagram_channel;
    static void native_notification(void*, ruvia::detail::http3_acceptor_datagram_endpoint::notification_kind) noexcept {}
    void arm() {
        timer_.expires_after(1ms);
        timer_.async_wait([this](const asio::error_code& error) {
            if (error) {
                return;
            }
            poll();
            arm();
        });
    }
    asio::io_context& io_;

    ruvia::WorkerRuntimeContext& runtime_;
    ruvia::WorkerNotification notification_;
    ruvia::buffer_pool pool_;
    channel packets_;
    ruvia::detail::http3_acceptor_datagram_endpoint native_;
    asio::steady_timer timer_;
    std::unique_ptr<Http3QuicWireOwner> owner_;
};

void drain_owner(asio::io_context& io, native_wire_fixture& fixture) {
    auto& owner = fixture.owner();
    owner.requestStop();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        fixture.poll();
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(2ms);
        owner.pollStop();
        if (owner.stopStatus().complete() && fixture.native_retired()) {
            return;
        }
    }
    throw std::runtime_error("HTTP/3 network QUIC wire owner did not drain");
}

void exercise_owner_allocation_failure(ruvia::testing::TestContext& ruvia_ctx,
    std::size_t failAt, ruvia::WorkerRuntimeContext& worker_context) {
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
    http3_quic_client_tls_context clientTls({
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
    });
    FailOnAllocationResource allocationResource(failAt);
    asio::io_context io;
    connection_pump_state pump;
    native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, &allocationResource, {&pump, &pump_connection});
    auto& owner = fixture.owner();
    try {
        owner.prepare();
        owner.start();
    } catch (...) {
        RUVIA_CHECK(owner.failure() != nullptr);
    }

    if (!owner.failure()) {
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const Udp::endpoint server(asio::ip::address_v4::loopback(), owner.boundPort());
        http3_quic_client_transport client(clientTls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        std::array<std::byte, 2048> packet{};
        Udp::endpoint source;
        std::size_t packetSize{};
        bool receivedRetry{};
        const auto retryDeadline = std::chrono::steady_clock::now() + 2s;
        while (!receivedRetry && !owner.failure() &&
               std::chrono::steady_clock::now() < retryDeadline) {
            asio::error_code error;
            packetSize = peer.receive_from(asio::buffer(packet), source, 0, error);
            if (!error) {
                receivedRetry = true;
                break;
            }
            RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
            if (error != asio::error::would_block && error != asio::error::try_again) {
                break;
            }
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(receivedRetry || owner.failure() != nullptr);
        if (receivedRetry) {
            RUVIA_CHECK(source == server);
            const std::span<const std::byte> retry(packet.data(), packetSize);
            RUVIA_CHECK(is_long_header_type(retry, 0));
            const auto local = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
            const auto remote = ruvia::detail::to_quic_address(quic_address(source));
            (void)client.receive({retry, local, remote}, std::chrono::steady_clock::now());
            RUVIA_CHECK(client.connection().info().state != ruvia::quic_connection_state::failed);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!owner.failure() && std::chrono::steady_clock::now() < deadline) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
    }
    RUVIA_CHECK(owner.failure() != nullptr);
    RUVIA_CHECK(allocationResource.allocationAttempts() == failAt);
    drain_owner(io, fixture);
    const auto done = owner.stopStatus();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.endpoint_retired);
    RUVIA_CHECK(done.timerHandlersRetired);
    RUVIA_CHECK(done.transportDestroyed);
    RUVIA_CHECK(!done.outbound_pending);
    RUVIA_CHECK(done.failed);
    RUVIA_CHECK(allocationResource.outstandingAllocations() == 0);
}
struct runtime_burst_observation final {
    std::optional<std::size_t> business_head;
    std::optional<std::size_t> timer_head;
    bool input_empty{};
    bool output_empty{};
    std::size_t active{};
    std::size_t refused{};
    std::exception_ptr failure;
};

struct runtime_burst_fixture final {
    using channel = ruvia::detail::http3_datagram_channel;

    ruvia::WorkerMemory memory;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& routes{ruvia::detail::RouterImpl::from(router)};
    ruvia::detail::WorkerCapabilities capabilities;
    ruvia::ConnectionScanner scanner;
    ruvia::StopSource stop;
    ruvia::StopToken stop_token;
    std::atomic<std::size_t> active{};
    std::atomic<std::size_t> refused{};
    ruvia::detail::http3_worker_server server;
    std::exception_ptr failure;
    ruvia::detail::http3_worker_runtime protocol;
    ruvia::TaskScope runners;
    ruvia::WorkerSignal business_ready;
    asio::steady_timer business_timer;
    asio::steady_timer observation_timer;
    channel& packets;
    std::optional<std::size_t> business_head;
    std::optional<std::size_t> timer_head;

    runtime_burst_fixture(ruvia::WorkerRuntimeContext& worker, channel& input,
        const Udp::endpoint& local, const ruvia::detail::HttpServerListenerDefinition::Tls& tls)
        : capabilities(worker.ioContext(), worker.handle(), memory.resource(), {}, {}),
          scanner(worker.handle(), {}),
          stop_token(stop.token()),
          server(worker.handle(), memory, finalize_routes(), capabilities, scanner,
              worker.ioContext().get_executor(), options, stop_token, 1, 8, active, refused),
          protocol(worker, local, tls, ruvia::Http3ListenConfig{},
              {.server = &server, .max_connections = 1, .buffer_capacity = 8, .max_requests_per_connection = 8},
              input, {}, {this, [](void* context, std::exception_ptr error) noexcept {
                              static_cast<runtime_burst_fixture*>(context)->failure =
                                  std::move(error);
                          }}),
          runners(worker.handle(), {.resource = memory.resource()}),
          business_ready(worker.handle()),
          business_timer(worker.ioContext()),
          observation_timer(worker.ioContext()),
          packets(input) {
        protocol.stage();
    }

    [[nodiscard]] const ruvia::detail::RouteTable& finalize_routes() {
        routes.finalize();
        return routes.routeTable();
    }

    [[nodiscard]] std::optional<std::size_t> input_head() noexcept {
        const auto input = packets.worker_input();
        if (!input) {
            return std::nullopt;
        }
        return std::to_integer<std::size_t>(input->bytes[1]) |
               (std::to_integer<std::size_t>(input->bytes[2]) << 8U);
    }

    [[nodiscard]] ruvia::Task<void> run_business_task() {
        co_await business_ready.wait();
        business_head = input_head();
    }

    void release(std::promise<runtime_burst_observation>& observed) {
        runners.spawn(run_business_task());
        business_ready.notify();
        business_timer.expires_at(std::chrono::steady_clock::now());
        business_timer.async_wait([this](const asio::error_code& error) {
            if (!error) {
                timer_head = input_head();
            }
        });
        // This read-only snapshot precedes the runtime's 10ms monitor. Neither
        // observer pumps protocol work or publishes another datagram edge.
        observation_timer.expires_after(5ms);
        observation_timer.async_wait([this, &observed](const asio::error_code& error) {
            if (error) {
                observed.set_exception(std::make_exception_ptr(std::system_error(error)));
                return;
            }
            observed.set_value({business_head, timer_head, !packets.worker_input(),
                packets.worker_outbound_count() == 0, active.load(), refused.load(), failure});
        });
        runners.spawn(server.run());
        runners.spawn(protocol.run_datagrams());
        protocol.start();
    }

    [[nodiscard]] ruvia::Task<void> join_cold_retirement() {
        co_await protocol.join();
        co_await runners.join();
    }
};

#endif
}  // namespace

RUVIA_TEST(http3_worker_runtime_coalesced_drop_burst_drains_without_external_edges_and_yields) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using channel = ruvia::detail::http3_datagram_channel;
    constexpr std::size_t burst_size = 256;
    constexpr std::size_t output_window = 16;
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = files.certificate.string();
    tls.identity.privateKeyFile = files.privateKey.string();
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor_runtime(acceptor_io, 8);
    ruvia::WorkerNotification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(burst_size + output_window, channel::packet_capacity);
    wire_worker worker;
    channel packets(pool, acceptor_notification, nullptr, burst_size, output_window);
    packets.stage_worker(worker.runtime());
    const Udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
    const Udp::endpoint peer(asio::ip::address_v4::loopback(), 43210);
    std::unique_ptr<runtime_burst_fixture> fixture;
    RUVIA_CHECK(worker.invoke([&] {
        fixture = std::make_unique<runtime_burst_fixture>(
            worker.runtime(), packets, local, tls);
        return fixture->server.install();
    }));
    std::promise<runtime_burst_observation> observed;
    auto observation = observed.get_future();
    // No runner exists yet. Every notification therefore belongs to one
    // coalesced RX burst; there is no follow-up UDP or completion-credit edge.
    for (std::size_t index = 0; index < burst_size; ++index) {
        auto storage = pool.try_acquire();
        RUVIA_CHECK(storage.has_value());
        storage->bytes()[0] = std::byte{0};
        storage->bytes()[1] = static_cast<std::byte>(index & 0xffU);
        storage->bytes()[2] = static_cast<std::byte>(index >> 8U);
        RUVIA_CHECK(packets.acceptor_push({std::move(*storage), 3, local, peer}));
    }
    worker.invoke([&] { fixture->release(observed); });
    // Waiting is Acceptor-only. Do not poll credits or post worker commands
    // until the read-only owner snapshot proves the complete burst was consumed.
    const bool observed_in_time = observation.wait_for(2s) == std::future_status::ready;
    RUVIA_CHECK(observed_in_time);
    if (!observed_in_time) {
        std::terminate();
    }
    const auto snapshot = observation.get();
    RUVIA_CHECK(snapshot.business_head.has_value());
    RUVIA_CHECK(snapshot.timer_head.has_value());
    if (snapshot.business_head) {
        RUVIA_CHECK(*snapshot.business_head > 0 && *snapshot.business_head < burst_size);
    }
    if (snapshot.timer_head) {
        RUVIA_CHECK(*snapshot.timer_head > 0 && *snapshot.timer_head < burst_size);
    }
    RUVIA_CHECK(snapshot.input_empty);
    RUVIA_CHECK(snapshot.output_empty);
    RUVIA_CHECK_EQ(snapshot.active, std::size_t{0});
    RUVIA_CHECK_EQ(snapshot.refused, std::size_t{0});
    RUVIA_CHECK(!snapshot.failure);
    RUVIA_CHECK(!packets.acceptor_take_output());
    RUVIA_CHECK_EQ(pool.outstanding(), burst_size + output_window);
    // Aggregate credit returns are bounded per poll, independently of the
    // already-drained RX queue. Only now may Acceptor replenish worker credits.
    for (std::size_t turn = 0;
        turn < burst_size && pool.outstanding() > output_window; ++turn) {
        packets.acceptor_poll();
    }
    RUVIA_CHECK_EQ(pool.outstanding(), output_window);
    packets.acceptor_close();
    std::promise<void> joined;
    auto completion = joined.get_future();
    worker.invoke([&] {
        fixture->protocol.stop();
        asio::co_spawn(worker.runtime().ioContext(),
            ruvia::asAwaitable(fixture->runners.join()),
            [&joined](std::exception_ptr error) {
                if (error) {
                    joined.set_exception(std::move(error));
                } else {
                    joined.set_value();
                }
            });
    });
    const auto join_deadline = std::chrono::steady_clock::now() + 2s;
    while (completion.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < join_deadline) {
        // Keep the real Acceptor owner advancing unused TX returns and final
        // completion credits while the worker's structured runners retire.
        packets.acceptor_poll();
        (void)completion.wait_for(1ms);
    }
    const bool joined_in_time = completion.wait_for(0s) == std::future_status::ready;
    RUVIA_CHECK(joined_in_time);
    if (!joined_in_time) {
        std::terminate();
    }
    completion.get();
    packets.acceptor_poll();
    const bool drained = worker.invoke([&] {
        const bool done = fixture->server.drained() && fixture->protocol.drained() &&
                          !fixture->failure;
        fixture.reset();
        return done;
    });
    RUVIA_CHECK(drained);
    RUVIA_CHECK(packets.worker_closed());
    RUVIA_CHECK(packets.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
#endif
}

RUVIA_TEST(http3_worker_server_cold_protocol_retirement_joins_started_handler_and_returns_channel_loans) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using channel = ruvia::detail::http3_datagram_channel;
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile = files.certificate.string();
    tls.identity.privateKeyFile = files.privateKey.string();
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor_runtime(acceptor_io, 8);
    ruvia::WorkerNotification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(8, channel::packet_capacity);
    wire_worker worker;
    auto packets = std::make_unique<channel>(pool, acceptor_notification, nullptr, 2, 2);
    packets->stage_worker(worker.runtime());
    const Udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
    std::unique_ptr<runtime_burst_fixture> fixture;
    std::promise<void> joined;
    auto completion = joined.get_future();
    worker.invoke([&] {
        fixture = std::make_unique<runtime_burst_fixture>(
            worker.runtime(), *packets, local, tls);
        RUVIA_CHECK(fixture->server.install());
        fixture->runners.spawn(fixture->server.run());
        RUVIA_CHECK(fixture->server.run_started());
        RUVIA_CHECK(!fixture->protocol.runner_started());
        fixture->protocol.abandon_before_launch();
        asio::co_spawn(worker.runtime().ioContext(),
            ruvia::asAwaitable(fixture->join_cold_retirement()),
            [&joined](std::exception_ptr error) {
                if (error) {
                    joined.set_exception(std::move(error));
                } else {
                    joined.set_value();
                }
            });
    });
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{2});
    packets->acceptor_close();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (completion.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        packets->acceptor_poll();
        (void)completion.wait_for(1ms);
    }
    const bool completed = completion.wait_for(0s) == std::future_status::ready;
    RUVIA_CHECK(completed);
    if (!completed) {
        std::terminate();
    }
    completion.get();
    RUVIA_CHECK(packets->worker_closed());
    RUVIA_CHECK(packets->acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    // App destroys Acceptor channels before phase-two worker finalization.
    // Repeated lifecycle requests and protocol destruction must not borrow them.
    packets.reset();
    RUVIA_CHECK(worker.invoke([&] {
        fixture->protocol.stop();
        fixture->protocol.abandon_before_launch();
        const bool drained = fixture->server.drained() && fixture->protocol.drained() &&
                             !fixture->failure;
        fixture.reset();
        return drained;
    }));
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
#endif
}

RUVIA_TEST(http3_network_queue_deduplicates_initial_offers_and_preserves_capacity) {
    std::pmr::vector<ruvia::quic_initial_offer> pending;
    pending.reserve(2);
    ruvia::quic_initial_offer offer{.offer_id = 1};
    RUVIA_CHECK(ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK(!ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK_EQ(pending.size(), std::size_t{1});

    std::uint64_t next_id = 2;
    while (pending.size() < pending.capacity()) {
        offer.offer_id = next_id++;
        RUVIA_CHECK(ruvia::detail::queue_http3_initial_offer(pending, offer));
    }
    offer.offer_id = next_id;
    RUVIA_CHECK(!ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK_EQ(pending.size(), pending.capacity());
}

RUVIA_TEST(http3_network_sends_version_negotiation_from_owned_udp_slot_without_admission) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tls_config;
        tls_config.identity.certificateChainFile = files.certificate.string();
        tls_config.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        asio::io_context io;
        version_negotiation_pump_state pump_state;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump_state, &pump_version_negotiation});
        auto& owner = fixture.owner();
        owner.prepare();
        owner.start();

        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        std::array<std::byte, ruvia::detail::http3_udp_socket::datagram_buffer_size> response{};
        Udp::endpoint responseSource;
        asio::error_code receive_error;
        std::size_t response_size{};
        bool receive_complete{};
        peer.async_receive_from(asio::buffer(response), responseSource,
            [&](const asio::error_code& error, std::size_t size) noexcept {
                receive_error = error;
                response_size = size;
                receive_complete = true;
            });

        std::array<std::byte, 1200> unsupported_version{};
        unsupported_version[0] = std::byte{0xc0};
        unsupported_version[4] = std::byte{0x02};
        unsupported_version[5] = std::byte{8};
        unsupported_version[14] = std::byte{8};
        for (std::size_t i = 0; i < 8; ++i) {
            unsupported_version[6 + i] = static_cast<std::byte>(0x10 + i);
            unsupported_version[15 + i] = static_cast<std::byte>(0x20 + i);
        }
        const Udp::endpoint server(asio::ip::address_v4::loopback(), owner.boundPort());
        asio::error_code send_error;
        RUVIA_CHECK(peer.send_to(asio::buffer(unsupported_version), server, 0, send_error) ==
                    unsupported_version.size());
        RUVIA_CHECK(!send_error);

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!receive_complete && std::chrono::steady_clock::now() < deadline) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(receive_complete);
        RUVIA_CHECK(!receive_error);
        RUVIA_CHECK(pump_state.routed_);
        RUVIA_CHECK(pump_state.sent_);
        RUVIA_CHECK(pump_state.plan_consumed_);
        RUVIA_CHECK(!pump_state.failure_);
        RUVIA_CHECK(response_size > 5);
        if (response_size > 5) {
            RUVIA_CHECK_EQ(response[1], std::byte{});
            RUVIA_CHECK_EQ(response[2], std::byte{});
            RUVIA_CHECK_EQ(response[3], std::byte{});
            RUVIA_CHECK_EQ(response[4], std::byte{});
        }
        RUVIA_CHECK(responseSource == server);
        RUVIA_CHECK(owner.transport() != nullptr);
        if (owner.transport() != nullptr) {
            RUVIA_CHECK_EQ(owner.transport()->server().connection_count(), std::size_t{0});
            RUVIA_CHECK_EQ(owner.transport()->server().pending_connection_count(), std::size_t{0});
        }
        RUVIA_CHECK(!owner.failure());
        drain_owner(io, fixture);
        RUVIA_CHECK(owner.stopStatus().complete());
        RUVIA_CHECK(!owner.stopStatus().failed);
    });
#endif
}

RUVIA_TEST(http3_network_quic_wire_owner_handles_initial_and_bounds_timer_progress) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
        tlsConfig.identity.certificateChainFile = files.certificate.string();
        tlsConfig.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
        http3_quic_client_tls_context clientTls({
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
        });
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner = fixture.owner();
        owner.prepare();
        RUVIA_CHECK(owner.boundPort() != 0);
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const Udp::endpoint server(asio::ip::address_v4::loopback(), owner.boundPort());
        http3_quic_client_transport client(clientTls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        RUVIA_CHECK(client.connection().info().state == ruvia::quic_connection_state::connecting);

        io.poll();
        std::array<std::byte, 2048> beforeStart{};
        Udp::endpoint beforeStartSource;
        asio::error_code beforeStartError;
        (void)peer.receive_from(asio::buffer(beforeStart), beforeStartSource, 0, beforeStartError);
        RUVIA_CHECK(beforeStartError == asio::error::would_block ||
                    beforeStartError == asio::error::try_again);
        RUVIA_CHECK(owner.timerExpirations() == 0);
        io.restart();
        owner.start();

        std::array<std::byte, 2048> packet{};
        Udp::endpoint source;
        std::size_t packetSize{};
        bool receivedRetry{};
        const auto receiveDeadline = std::chrono::steady_clock::now() + 2s;
        while (!receivedRetry && std::chrono::steady_clock::now() < receiveDeadline) {
            asio::error_code error;
            packetSize = peer.receive_from(asio::buffer(packet), source, 0, error);
            if (!error) {
                receivedRetry = true;
                break;
            }
            RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
            if (error != asio::error::would_block && error != asio::error::try_again) {
                break;
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(receivedRetry);
        RUVIA_CHECK(source == server);
        const std::span<const std::byte> retry(packet.data(), packetSize);
        RUVIA_CHECK(is_long_header_type(retry, 0));
        if (receivedRetry && is_long_header_type(retry, 0)) {
            RUVIA_CHECK(packetSize > 21);
            const auto local = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
            const auto remote = ruvia::detail::to_quic_address(quic_address(source));
            const auto received = client.receive({retry, local, remote}, std::chrono::steady_clock::now());
            RUVIA_CHECK(received == ruvia::quic_operation_status::accepted || received == ruvia::quic_operation_status::need_input);
            RUVIA_CHECK(client.connection().info().state == ruvia::quic_connection_state::connecting);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
            io.run_for(1200ms);
            RUVIA_CHECK(owner.timerExpirations() > 0);
            RUVIA_CHECK(owner.timerExpirations() < 1600);
            constexpr std::array<char, 4> closeReason{'d', 'o', 'n', 'e'};
            const auto closeStatus = client.connection().close({
                .kind = ruvia::quic_close_kind::application,
                .code = 0,
                .frame_type = 0,
                .reason = closeReason,
            });
            RUVIA_CHECK(closeStatus == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
            io.run_for(10ms);
        }
        RUVIA_CHECK(!owner.failure());
        drain_owner(io, fixture);
        RUVIA_CHECK(owner.stopStatus().complete());
        RUVIA_CHECK(!owner.stopStatus().failed);
    });
#endif
}

RUVIA_TEST(http3_forwarded_wire_owner_drives_input_and_timer_while_udp_output_window_is_full) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using channel = ruvia::detail::http3_datagram_channel;
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tls_config;
    tls_config.identity.certificateChainFile = files.certificate.string();
    tls_config.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
    http3_quic_client_tls_context client_tls({
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
    });
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor_runtime(acceptor_io, 8);
    ruvia::WorkerNotification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(8, channel::packet_capacity);
    wire_worker worker;
    constexpr std::size_t output_window = 1;
    channel packets(pool, acceptor_notification, nullptr, 2, output_window);
    packets.stage_worker(worker.runtime());
    ruvia::detail::http3_acceptor_datagram_endpoint acceptor(acceptor_io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0),
        {nullptr, [](void*, ruvia::detail::http3_acceptor_datagram_endpoint::notification_kind) noexcept {}}, pool);
    acceptor.prepare();
    RUVIA_CHECK(acceptor.start() == http3_worker_datagram_endpoint::pump_result::pending);
    const Udp::endpoint server(asio::ip::address_v4::loopback(), acceptor.bound_port());
    connection_pump_state pump;
    std::unique_ptr<Http3QuicWireOwner> owner;
    ruvia::quic_server_config server_config;
    // ngtcp2 applies its authoritative max(idle_timeout, 3*PTO) minimum.
    // Even with TX held, its independent idle deadline must retire the peer.
    server_config.local_transport_parameters.idle_timeout_ms = 1;
    worker.invoke([&] {
        packets.worker_start();
        owner = std::make_unique<Http3QuicWireOwner>(worker.runtime().ioContext(), packets,
            server, tls, server_config, nullptr,
            Http3QuicWireOwner::ProtocolPump{&pump, &pump_connection});
        owner->prepare();
        owner->start();
    });
    Udp::socket peer(acceptor_io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_transport client(client_tls, client_config(peer, server), "localhost",
        std::chrono::steady_clock::now());
    std::array<std::byte, 2048> initial_bytes{};
    const auto initial = client.write_packet(initial_bytes, std::chrono::steady_clock::now());
    RUVIA_CHECK(initial.size != 0);
    RUVIA_CHECK(is_long_header_type(std::span<const std::byte>(initial_bytes).first(initial.size), 0));
    const auto send_initial = [&] {
        asio::error_code error;
        const auto sent = peer.send_to(asio::buffer(initial_bytes.data(), initial.size), server, 0, error);
        RUVIA_CHECK(!error);
        RUVIA_CHECK_EQ(sent, initial.size);
    };
    const auto forward_input = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            acceptor.poll_receive();
            if (acceptor_io.stopped()) {
                acceptor_io.restart();
            }
            acceptor_io.run_for(2ms);
            if (auto input = acceptor.take_receive()) {
                return packets.acceptor_push(std::move(*input));
            }
        }
        return false;
    };
    send_initial();
    RUVIA_CHECK(forward_input());
    worker.invoke([&] { owner->poll_datagrams(); });
    auto held_output = packets.acceptor_take_output();
    RUVIA_CHECK(held_output.has_value());
    if (held_output) {
        RUVIA_CHECK(is_long_header_type(held_output->view().bytes, 0));
        RUVIA_CHECK(held_output->local_destination == server);
        RUVIA_CHECK(held_output->peer == peer.local_endpoint());
    }
    std::array<std::byte, channel::packet_capacity> held_bytes{};
    if (held_output) {
        std::ranges::copy(held_output->view().bytes, held_bytes.begin());
    }
    const auto before_input = worker.invoke([&] {
        RUVIA_CHECK(pump.connection.has_value());
        RUVIA_CHECK(owner->stopStatus().outbound_pending);
        RUVIA_CHECK_EQ(packets.worker_outbound_count(), output_window);
        RUVIA_CHECK(!packets.worker_outbound_capacity());
        RUVIA_CHECK(owner->transport()->server().next_expiry().has_value());
        return pump;
    });

    // Replay a real client Initial while Acceptor still holds the server's UDP
    // completion lease. RX routing/receive must run even without a TX credit.
    send_initial();
    RUVIA_CHECK(forward_input());
    const auto after_input = worker.invoke([&] {
        owner->requestDrive();
        RUVIA_CHECK(!packets.worker_input());
        RUVIA_CHECK(owner->stopStatus().outbound_pending);
        RUVIA_CHECK(!packets.worker_outbound_capacity());
        return pump;
    });
    RUVIA_CHECK(after_input.full_drives > before_input.full_drives);
    RUVIA_CHECK_EQ(after_input.full_receives, before_input.full_receives + 1);
    RUVIA_CHECK_EQ(after_input.packet_writes, before_input.packet_writes);
    RUVIA_CHECK_EQ(after_input.packets_sent, before_input.packets_sent);
    packets.acceptor_poll();

    struct timer_observation final {
        connection_pump_state pump;
        std::size_t expirations{};
        std::chrono::steady_clock::time_point observed_at{};
        bool deadline{};
        bool full{};
        bool failed{};
    };
    const auto observe_timer = [&] {
        return worker.invoke([&] {
            return timer_observation{
                pump, owner->timerExpirations(), std::chrono::steady_clock::now(),
                owner->transport() && owner->transport()->server().next_expiry().has_value(),
                owner->stopStatus().outbound_pending && !packets.worker_outbound_capacity(),
                owner->failure() != nullptr || pump.failure != nullptr};
        });
    };
    const auto before_timer = observe_timer();
    auto after_timer = before_timer;
    const auto timer_deadline = std::chrono::steady_clock::now() + 6s;
    // Only read worker state: no requestDrive/poll_datagrams can masquerade
    // as a timer callback. Native PTO deadlines must advance while production
    // waits for credit, and the independent idle expiry must still retire.
    while (!after_timer.pump.full_idle_retired &&
           std::chrono::steady_clock::now() < timer_deadline && !after_timer.failed) {
        acceptor.poll_receive();
        if (acceptor_io.stopped()) {
            acceptor_io.restart();
        }
        acceptor_io.run_for(2ms);
        after_timer = observe_timer();
    }
    // RX can process the first due native deadline before before_timer.
    RUVIA_CHECK(after_timer.expirations > before_timer.expirations);
    RUVIA_CHECK(after_timer.pump.full_expirations >= 2);
    RUVIA_CHECK_EQ(after_timer.pump.full_expirations_with_deadline + 1,
        after_timer.pump.full_expirations);
    RUVIA_CHECK(!after_timer.pump.repeated_full_expiry);
    RUVIA_CHECK(after_timer.pump.full_idle_retired);
    RUVIA_CHECK(!after_timer.deadline);
    RUVIA_CHECK(after_timer.full);
    RUVIA_CHECK(!after_timer.failed);
    RUVIA_CHECK_EQ(after_timer.pump.packet_writes, before_input.packet_writes);
    RUVIA_CHECK_EQ(after_timer.pump.packets_sent, before_input.packets_sent);
    if (held_output) {
        RUVIA_CHECK(std::ranges::equal(held_output->view().bytes,
            std::span<const std::byte>(held_bytes).first(held_output->size)));
    }

    packets.acceptor_close();
    const auto stopping = worker.invoke([&] {
        owner->requestStop();
        owner->pollStop();
        return owner->stopStatus();
    });
    RUVIA_CHECK(stopping.stopping);
    RUVIA_CHECK(stopping.outbound_pending);
    RUVIA_CHECK(!stopping.transportDestroyed);
    RUVIA_CHECK(!stopping.complete());
    RUVIA_CHECK(!packets.acceptor_finalize());
    // This models the Acceptor UDP completion, not a QUIC ACK: the client has
    // never received this output and sends no acknowledgement during the test.
    held_output.reset();
    packets.acceptor_poll();
    acceptor.request_stop();
    Http3QuicWireOwner::StopStatus done;
    const auto stop_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        packets.acceptor_poll();
        if (acceptor_io.stopped()) {
            acceptor_io.restart();
        }
        acceptor_io.run_for(2ms);
        done = worker.invoke([&] {
            owner->poll_datagrams();
            owner->pollStop();
            return owner->stopStatus();
        });
    } while ((!done.complete() || acceptor.status() == http3_worker_datagram_endpoint::stop_status::pending) &&
             std::chrono::steady_clock::now() < stop_deadline);
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(!done.outbound_pending);
    RUVIA_CHECK(!done.failed);
    RUVIA_CHECK(acceptor.status() == http3_worker_datagram_endpoint::stop_status::done);
    worker.invoke([&] {
        owner.reset();
        packets.worker_close();
    });
    RUVIA_CHECK(packets.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerAsyncWaitSubmissionFailureDrainsHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        exercise_owner_allocation_failure(ruvia_ctx, 1, worker_context);
    });
#endif
}

RUVIA_TEST(http3_network_quic_wire_owner_allocation_failure_drains_handlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        exercise_owner_allocation_failure(ruvia_ctx, 4, worker_context);
    });
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerFatalFailureWaitsForNetworkRetirementGate) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
        tlsConfig.identity.certificateChainFile = files.certificate.string();
        tlsConfig.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
        asio::io_context io;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&tls, &fail_protocol_pump});
        auto& owner = fixture.owner();
        owner.prepare();
        owner.deferTransportRetirement();
        try {
            owner.start();
        } catch (...) {
        }
        RUVIA_CHECK(owner.failure() != nullptr);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
            owner.pollStop();
            const auto status = owner.stopStatus();
            if (status.endpoint_retired && status.timerHandlersRetired) {
                RUVIA_CHECK(!status.transportDestroyed);
                RUVIA_CHECK(owner.transport() != nullptr);
                break;
            }
        }
        RUVIA_CHECK(!owner.stopStatus().complete());
        owner.releaseTransportRetirement();
        drain_owner(io, fixture);
        RUVIA_CHECK(owner.stopStatus().complete());
        RUVIA_CHECK(owner.stopStatus().failed);
    });
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerStopWaitsForBorrowedDatagramSendAndHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
        tlsConfig.identity.certificateChainFile = files.certificate.string();
        tlsConfig.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
        http3_quic_client_tls_context clientTls({
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
        });
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner = fixture.owner();
        owner.prepare();
        owner.start();
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const Udp::endpoint server(asio::ip::address_v4::loopback(), owner.boundPort());
        http3_quic_client_transport client(clientTls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        bool stoppedDuringSend{};
        for (std::size_t turn = 0; turn < 64 && !stoppedDuringSend; ++turn) {
            if (io.stopped()) {
                io.restart();
            }
            if (io.run_one() == 0) {
                break;
            }
            stoppedDuringSend = owner.stopStatus().outbound_pending;
        }
        RUVIA_CHECK(stoppedDuringSend);
        if (stoppedDuringSend) {
            owner.requestStop();
            const auto pending = owner.stopStatus();
            RUVIA_CHECK(pending.stopping);
            RUVIA_CHECK(pending.outbound_pending);
            RUVIA_CHECK(!pending.transportDestroyed);
            owner.requestDrive();
            RUVIA_CHECK(owner.stopStatus().stopping);
        }
        drain_owner(io, fixture);
        const auto done = owner.stopStatus();
        RUVIA_CHECK(done.complete());
        RUVIA_CHECK(done.endpoint_retired);
        RUVIA_CHECK(done.timerHandlersRetired);
        RUVIA_CHECK(done.transportDestroyed);
        RUVIA_CHECK(!done.outbound_pending);
        RUVIA_CHECK(!done.failed);
    });
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerConstructionFailureDrainsBeforeAllocatorRetires) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
        tlsConfig.identity.certificateChainFile = files.certificate.string();
        tlsConfig.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
        asio::io_context io;
        ruvia::quic_server_config invalidConfig;
        invalidConfig.local_transport_parameters.max_udp_payload_size = 1199;
        connection_pump_state pump;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            invalidConfig, nullptr, {&pump, &pump_connection});
        auto& owner = fixture.owner();
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.prepare(); }));
        drain_owner(io, fixture);
        RUVIA_CHECK(owner.stopStatus().complete());
        RUVIA_CHECK(owner.stopStatus().failed);
        RUVIA_CHECK(owner.failure() != nullptr);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.rethrowFailure(); }));
    });
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerPreparedListenerStopsWithoutServing) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        TestIdentityFiles files;
        ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
        tlsConfig.identity.certificateChainFile = files.certificate.string();
        tlsConfig.identity.privateKeyFile = files.privateKey.string();
        http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture(worker_context, io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner = fixture.owner();
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.start(); }));
        RUVIA_CHECK(!owner.failure());
        owner.prepare();
        const auto port = owner.boundPort();
        RUVIA_CHECK(port != 0);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.prepare(); }));
        owner.requestStop();
        drain_owner(io, fixture);
        RUVIA_CHECK(owner.stopStatus().complete());
        RUVIA_CHECK(!owner.failure());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.start(); }));
        Udp::socket rebound(io, Udp::endpoint(asio::ip::address_v4::loopback(), port));
        RUVIA_CHECK(rebound.local_endpoint().port() == port);
    });
#endif
}
