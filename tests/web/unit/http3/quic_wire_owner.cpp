#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <new>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/http/quic_server.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/http3/Http3NetworkRuntime.h"
#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"
#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"
#include "ruvia/web/detail/server/HttpServerListener.h"

#include "test_harness.h"

namespace {
using namespace std::chrono_literals;
using Udp = asio::ip::udp;
using ruvia::detail::http3_quic_client_tls_context;
using ruvia::detail::http3_quic_client_transport;
using ruvia::detail::http3_quic_datagram_address;
using ruvia::detail::http3_quic_server_transport;
using ruvia::detail::http3_quic_tls_context;
using ruvia::detail::Http3DatagramEndpoint;
using ruvia::detail::Http3QuicWireOwner;

Http3QuicWireOwner::ProtocolPumpResult fail_protocol_pump(
    void*, http3_quic_server_transport&, ruvia::detail::Http3DatagramEndpoint&) noexcept {
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
    std::array<std::byte, ruvia::detail::Http3UdpSocket::kDatagramBufferSize> packet_buffer_{};
    bool routed_{};
    bool sent_{};
    bool plan_consumed_{};
    std::exception_ptr failure_;
};

Http3QuicWireOwner::ProtocolPumpResult pump_version_negotiation(void* context,
    http3_quic_server_transport& transport, Http3DatagramEndpoint& endpoint) noexcept {
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
                transport.server(), route.version_negotiation, endpoint, state.packet_buffer_);
            state.sent_ = result == Http3DatagramEndpoint::pump_result::pending;
            state.plan_consumed_ = !route.version_negotiation.valid();
            if (state.sent_) {
                std::ranges::fill(state.packet_buffer_, std::byte{0xa5});
            }
        }
        if (endpoint.consume_receive() == Http3DatagramEndpoint::pump_result::error) {
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
    std::array<std::byte, 2048> packet{};
};

Http3QuicWireOwner::ProtocolPumpResult pump_connection(void* context,
    http3_quic_server_transport& transport, Http3DatagramEndpoint& endpoint) noexcept {
    auto& state = *static_cast<connection_pump_state*>(context);
    try {
        const auto now = std::chrono::steady_clock::now();
        (void)transport.server().handle_expiry(now);
        bool progress{};
        if (const auto received = endpoint.receive_slot()) {
            const auto local = ruvia::detail::to_http3_quic_datagram_address(received->local_destination);
            const auto peer = ruvia::detail::to_http3_quic_datagram_address(received->peer);
            auto route = transport.route_datagram(received->bytes, *local, *peer);
            if (route.kind == ruvia::quic_server_route_kind::initial_offer && !state.connection) {
                const auto admitted = transport.admit_initial(route.offer, now);
                if (admitted.status == ruvia::quic_operation_status::accepted) {
                    state.connection = admitted.connection;
                }
            } else if (route.kind == ruvia::quic_server_route_kind::existing_connection) {
                (void)transport.server().receive(route.connection,
                    {received->bytes, ruvia::detail::to_quic_address(*local), ruvia::detail::to_quic_address(*peer)}, now);
            }
            (void)endpoint.consume_receive();
            progress = true;
        }
        if (state.connection && !endpoint.send_in_flight()) {
            const auto packet = transport.server().connection(*state.connection).write_packet(state.packet, now);
            if (packet.size != 0) {
                const auto source = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.local));
                const auto peer = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.peer));
                if (endpoint.send_datagram(std::span<const std::byte>(state.packet).first(packet.size), *source, *peer) == Http3DatagramEndpoint::pump_result::error) {
                    return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
                }
                progress = true;
            }
        }
        return progress ? Http3QuicWireOwner::ProtocolPumpResult::kProgress : Http3QuicWireOwner::ProtocolPumpResult::kIdle;
    } catch (...) {
        return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
    }
}

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

void drain_owner(asio::io_context& io, Http3QuicWireOwner& owner) {
    owner.requestStop();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(2ms);
        owner.pollStop();
        if (owner.stopStatus().complete()) {
            return;
        }
    }
    throw std::runtime_error("HTTP/3 network QUIC wire owner did not drain");
}

void exercise_owner_allocation_failure(ruvia::testing::TestContext& ruvia_ctx,
    std::size_t failAt) {
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
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, &allocationResource, {&pump, &pump_connection});
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
    drain_owner(io, owner);
    const auto done = owner.stopStatus();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.socketDone);
    RUVIA_CHECK(done.timerHandlersRetired);
    RUVIA_CHECK(done.transportDestroyed);
    RUVIA_CHECK(!done.sendInFlight);
    RUVIA_CHECK(done.failed);
    RUVIA_CHECK(allocationResource.outstandingAllocations() == 0);
}
#endif
}  // namespace

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
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tls_config;
    tls_config.identity.certificateChainFile = files.certificate.string();
    tls_config.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
    asio::io_context io;
    version_negotiation_pump_state pump_state;
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, nullptr, {&pump_state, &pump_version_negotiation});
    owner.prepare();
    owner.start();

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    std::array<std::byte, ruvia::detail::Http3UdpSocket::kDatagramBufferSize> response{};
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
    drain_owner(io, owner);
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(!owner.stopStatus().failed);
#endif
}

RUVIA_TEST(http3_network_quic_wire_owner_handles_initial_and_bounds_timer_progress) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
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
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, nullptr, {&pump, &pump_connection});
    owner.prepare();
    RUVIA_CHECK(owner.boundPort() != 0);
    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    peer.non_blocking(true);
    const Udp::endpoint server(asio::ip::address_v4::loopback(), owner.boundPort());
    http3_quic_client_transport client(clientTls, client_config(peer, server), "localhost",
        std::chrono::steady_clock::now());
    RUVIA_CHECK(pump_peer(client, peer) != 0);
    RUVIA_CHECK(client.connection().info().state == ruvia::quic_connection_state::connecting);

    RUVIA_CHECK(io.poll() == 0);
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
    drain_owner(io, owner);
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(!owner.stopStatus().failed);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerAsyncWaitSubmissionFailureDrainsHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    exercise_owner_allocation_failure(ruvia_ctx, 1);
#endif
}

RUVIA_TEST(http3_network_quic_wire_owner_allocation_failure_drains_handlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    exercise_owner_allocation_failure(ruvia_ctx, 4);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerFatalFailureWaitsForNetworkRetirementGate) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
    asio::io_context io;
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, nullptr, {&tls, &fail_protocol_pump});
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
        if (status.socketDone && status.timerHandlersRetired) {
            RUVIA_CHECK(!status.transportDestroyed);
            RUVIA_CHECK(owner.transport() != nullptr);
            break;
        }
    }
    RUVIA_CHECK(!owner.stopStatus().complete());
    owner.releaseTransportRetirement();
    drain_owner(io, owner);
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(owner.stopStatus().failed);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerStopWaitsForBorrowedDatagramSendAndHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
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
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, nullptr, {&pump, &pump_connection});
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
        stoppedDuringSend = owner.stopStatus().sendInFlight;
    }
    RUVIA_CHECK(stoppedDuringSend);
    if (stoppedDuringSend) {
        owner.requestStop();
        const auto pending = owner.stopStatus();
        RUVIA_CHECK(pending.stopping);
        RUVIA_CHECK(pending.sendInFlight);
        RUVIA_CHECK(!pending.transportDestroyed);
        owner.requestDrive();
        RUVIA_CHECK(owner.stopStatus().stopping);
    }
    drain_owner(io, owner);
    const auto done = owner.stopStatus();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.socketDone);
    RUVIA_CHECK(done.timerHandlersRetired);
    RUVIA_CHECK(done.transportDestroyed);
    RUVIA_CHECK(!done.sendInFlight);
    RUVIA_CHECK(!done.failed);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerConstructionFailureDrainsBeforeAllocatorRetires) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
    asio::io_context io;
    ruvia::quic_server_config invalidConfig;
    invalidConfig.local_transport_parameters.max_udp_payload_size = 1199;
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        invalidConfig);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.prepare(); }));
    owner.pollStop();
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(owner.stopStatus().failed);
    RUVIA_CHECK(owner.failure() != nullptr);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.rethrowFailure(); }));
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerPreparedListenerStopsWithoutServing) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
    asio::io_context io;
    Http3QuicWireOwner owner(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.start(); }));
    RUVIA_CHECK(!owner.failure());
    owner.prepare();
    const auto port = owner.boundPort();
    RUVIA_CHECK(port != 0);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.prepare(); }));
    RUVIA_CHECK(io.poll() == 0);
    owner.requestStop();
    owner.pollStop();
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(!owner.failure());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.start(); }));
    Udp::socket rebound(io, Udp::endpoint(asio::ip::address_v4::loopback(), port));
    RUVIA_CHECK(rebound.local_endpoint().port() == port);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerBindFailureReleasesPreparedResources) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    http3_quic_tls_context tls(tlsConfig, std::pmr::get_default_resource());
    asio::io_context io;
    Udp::socket occupied(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    Http3QuicWireOwner owner(io, occupied.local_endpoint(), tls);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { owner.prepare(); }));
    owner.pollStop();
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(owner.failure() != nullptr);
    RUVIA_CHECK(io.poll() == 0);
#endif
}
