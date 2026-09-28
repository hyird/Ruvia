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

#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"
#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"
#include "ruvia/web/detail/server/HttpServerListener.h"

#include "test_harness.h"

namespace {
using namespace std::chrono_literals;
using Udp = asio::ip::udp;
using ruvia::detail::Http3QuicWireOwner;
using ruvia::detail::Http3QuicDatagramAddress;
using ruvia::detail::Http3QuicDatagramBridge;

class FailOnAllocationResource final : public std::pmr::memory_resource {
public:
    explicit FailOnAllocationResource(std::size_t failAt) noexcept
        : failAt_(failAt) {}

    [[nodiscard]] std::size_t allocationAttempts() const noexcept {
        return allocationAttempts_;
    }

    [[nodiscard]] std::size_t outstandingAllocations() const noexcept {
        return outstandingAllocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationAttempts_;
        if (allocationAttempts_ == failAt_) {
            throw std::bad_alloc();
        }
        void* const allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++outstandingAllocations_;
        return allocation;
    }

    void do_deallocate(void* allocation, std::size_t bytes, std::size_t alignment) override {
        --outstandingAllocations_;
        std::pmr::new_delete_resource()->deallocate(allocation, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t failAt_{};
    std::size_t allocationAttempts_{};
    std::size_t outstandingAllocations_{};
};

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
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

Http3QuicDatagramAddress quicAddress(const Udp::endpoint& endpoint) {
    const auto address = ruvia::detail::toHttp3QuicDatagramAddress(endpoint);
    if (!address) {
        throw std::runtime_error("test UDP endpoint cannot be converted to QUIC address");
    }
    return *address;
}

bool isLongHeaderType(std::span<const std::byte> packet, std::uint8_t type) {
    if (packet.size() < 5 ||
        (std::to_integer<std::uint8_t>(packet[0]) & 0x80U) == 0) {
        return false;
    }
    const std::uint32_t version =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[1])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[3])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[4]));
    return version == 1 && ((std::to_integer<std::uint8_t>(packet[0]) >> 4U) & 0x03U) == type;
}

std::size_t sendClientOutput(Http3QuicDatagramBridge& bridge, Udp::socket& socket) {
    std::size_t sent{};
    for (std::size_t count = 0; count < 32; ++count) {
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        const auto result = bridge.takeOutbound(outbound);
        if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
            return sent;
        }
        if (result != Http3QuicDatagramBridge::OutboundResult::kReady) {
            throw std::runtime_error("test QUIC client BIO did not yield an outbound packet");
        }
        const auto destination = ruvia::detail::toHttp3UdpEndpoint(outbound.destination);
        if (!destination) {
            bridge.completeOutbound();
            throw std::runtime_error("test QUIC client packet has invalid destination");
        }
        asio::error_code error;
        const auto size = socket.send_to(asio::buffer(outbound.bytes.data(), outbound.bytes.size()),
            *destination, 0, error);
        bridge.completeOutbound();
        if (error || size != outbound.bytes.size()) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                "send test QUIC client packet");
        }
        ++sent;
    }
    throw std::runtime_error("test QUIC client exceeded the outbound packet bound");
}

void drainOwner(asio::io_context& io, Http3QuicWireOwner& owner) {
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

void exerciseTimerHandlerAllocationFailure(ruvia::testing::TestContext& ruvia_ctx,
    std::size_t failAt, bool expectTimerExpiration) {
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());
    ruvia::detail::Http3QuicClientTlsContext clientTls({
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
    });

    FailOnAllocationResource allocationResource(failAt);
    asio::io_context io;
    Http3QuicWireOwner owner(io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls, {}, &allocationResource);
    bool startThrew{};
    try {
        owner.prepare();
        owner.start();
    } catch (...) {
        startThrew = true;
    }
    RUVIA_CHECK(!startThrew || owner.failure() != nullptr);

    if (!owner.failure()) {
        Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const auto peerAddress = quicAddress(peer.local_endpoint());
        const auto serverEndpoint = Udp::endpoint(
            asio::ip::address_v4::loopback(), owner.boundPort());
        const auto serverAddress = quicAddress(serverEndpoint);
        Http3QuicDatagramBridge clientBridge(peerAddress);
        ruvia::detail::Http3QuicClientTransport client(
            clientTls, clientBridge, serverAddress, "localhost");
        (void)client.startConnect();
        RUVIA_CHECK(sendClientOutput(clientBridge, peer) != 0);

        // The first Initial only elicits a stateless Retry. Advance the client
        // through that Retry so OpenSSL starts a timed QUIC handshake and the
        // owner actually submits the timer wait under test.
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
        RUVIA_CHECK(receivedRetry);
        if (receivedRetry) {
            RUVIA_CHECK(source == serverEndpoint);
            const std::span<const std::byte> retryPacket(packet.data(), packetSize);
            RUVIA_CHECK(isLongHeaderType(retryPacket, 3));
            if (source == serverEndpoint && isLongHeaderType(retryPacket, 3)) {
                RUVIA_CHECK(clientBridge.inject(retryPacket, serverAddress) ==
                            Http3QuicDatagramBridge::InjectResult::kAccepted);
                RUVIA_CHECK(client.handleEvents() ==
                            ruvia::detail::Http3QuicClientTransport::State::kConnecting);
                RUVIA_CHECK(sendClientOutput(clientBridge, peer) != 0);
            }
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
    if (expectTimerExpiration) {
        RUVIA_CHECK(owner.timerExpirations() > 0);
    } else {
        RUVIA_CHECK(owner.timerExpirations() == 0);
    }

    drainOwner(io, owner);
    const auto done = owner.stopStatus();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.socketDone);
    RUVIA_CHECK(done.timerHandlersRetired);
    RUVIA_CHECK(done.transportDestroyed);
    RUVIA_CHECK(done.bridgeLeaseReleased);
    RUVIA_CHECK(!done.sendInFlight);
    RUVIA_CHECK(done.failed);
    RUVIA_CHECK(allocationResource.outstandingAllocations() == 0);
}
#endif
}  // namespace

RUVIA_TEST(http3NetworkQuicWireOwnerHandlesRealInitialRetryAndBoundsTimerProgress) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());
    ruvia::detail::Http3QuicClientTlsContext clientTls({
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
    });

    asio::io_context io;
    Http3QuicWireOwner owner(io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls);
    owner.prepare();
    RUVIA_CHECK(owner.boundPort() != 0);

    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    peer.non_blocking(true);
    const auto peerAddress = quicAddress(peer.local_endpoint());
    const auto serverEndpoint = Udp::endpoint(asio::ip::address_v4::loopback(), owner.boundPort());
    const auto serverAddress = quicAddress(serverEndpoint);
    Http3QuicDatagramBridge clientBridge(peerAddress);
    ruvia::detail::Http3QuicClientTransport client(
        clientTls, clientBridge, serverAddress, "localhost");
    RUVIA_CHECK(client.startConnect() ==
                ruvia::detail::Http3QuicClientTransport::State::kConnecting);
    RUVIA_CHECK(sendClientOutput(clientBridge, peer) != 0);

    // A real Initial may already be queued by the OS, but preparation must
    // not start a handshake before the application releases its serving barrier.
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
    RUVIA_CHECK(source == serverEndpoint);
    const std::span<const std::byte> retryPacket(packet.data(), packetSize);
    RUVIA_CHECK(isLongHeaderType(retryPacket, 3));
    if (receivedRetry && isLongHeaderType(retryPacket, 3)) {
        RUVIA_CHECK(packetSize > 21);
        RUVIA_CHECK(clientBridge.inject(retryPacket, serverAddress) ==
                    Http3QuicDatagramBridge::InjectResult::kAccepted);
        RUVIA_CHECK(client.handleEvents() ==
                    ruvia::detail::Http3QuicClientTransport::State::kConnecting);
        RUVIA_CHECK(sendClientOutput(clientBridge, peer) != 0);
        io.run_for(1200ms);
        RUVIA_CHECK(owner.timerExpirations() > 0);
        RUVIA_CHECK(owner.timerExpirations() < 1600);
    }
    RUVIA_CHECK(!owner.failure());
    drainOwner(io, owner);
    RUVIA_CHECK(owner.stopStatus().complete());
    RUVIA_CHECK(!owner.stopStatus().failed);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerAsyncWaitSubmissionFailureDrainsHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    exerciseTimerHandlerAllocationFailure(ruvia_ctx, 1, false);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerRetirementPostSubmissionFailureDrainsHandlers) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    // QUIC deadline updates cancel the first wait, so its retirement post and
    // the replacement wait precede the expiry whose retirement post is failed.
    exerciseTimerHandlerAllocationFailure(ruvia_ctx, 4, true);
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
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());
    ruvia::detail::Http3QuicClientTlsContext clientTls({
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification,
    });

    asio::io_context io;
    Http3QuicWireOwner owner(io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls);
    owner.prepare();
    owner.start();
    Udp::socket peer(io, Udp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto peerAddress = quicAddress(peer.local_endpoint());
    const auto serverAddress = quicAddress(
        Udp::endpoint(asio::ip::address_v4::loopback(), owner.boundPort()));
    Http3QuicDatagramBridge clientBridge(peerAddress);
    ruvia::detail::Http3QuicClientTransport client(
        clientTls, clientBridge, serverAddress, "localhost");
    (void)client.startConnect();
    RUVIA_CHECK(sendClientOutput(clientBridge, peer) != 0);

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
        RUVIA_CHECK(!pending.bridgeLeaseReleased);
    }
    drainOwner(io, owner);
    const auto done = owner.stopStatus();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.socketDone);
    RUVIA_CHECK(done.timerHandlersRetired);
    RUVIA_CHECK(done.transportDestroyed);
    RUVIA_CHECK(done.bridgeLeaseReleased);
    RUVIA_CHECK(!done.failed);
#endif
}

RUVIA_TEST(http3NetworkQuicWireOwnerConstructionFailureDrainsBeforeReleasingBridge) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles files;
    ruvia::detail::HttpServerListenerDefinition::Tls tlsConfig;
    tlsConfig.identity.certificateChainFile = files.certificate.string();
    tlsConfig.identity.privateKeyFile = files.privateKey.string();
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());

    asio::io_context io;
    ruvia::detail::Http3QuicServerTransportConfig invalidConfig;
    invalidConfig.handshakeTimeout = 0ms;
    Http3QuicWireOwner owner(io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls, invalidConfig);
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
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());
    asio::io_context io;
    Http3QuicWireOwner owner(io,
        Udp::endpoint(asio::ip::address_v4::loopback(), 0), tls);
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
    ruvia::detail::Http3QuicTlsContext tls(tlsConfig, std::pmr::get_default_resource());
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
