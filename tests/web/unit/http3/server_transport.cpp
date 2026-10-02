#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "ruvia/http/Http3ConnectionError.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/quic_connection.h"
#include "ruvia/http/quic_server.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"
#include "ruvia/web/detail/http3/openssl_quic_crypto_provider.h"
#include "ruvia/web/detail/http3/openssl_quic_tls_session.h"

#include "test_harness.h"

namespace {

struct IdentityFiles final {
    IdentityFiles() {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() /
                    ("ruvia-http3-listener-" + std::to_string(random()) + "-" +
                        std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) {
            throw std::runtime_error("failed to create temporary TLS directory");
        }
        EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (rawContext == nullptr) {
            throw std::runtime_error("failed to create key generator");
        }
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(rawContext,
            EVP_PKEY_CTX_free);
        EVP_PKEY* rawKey = nullptr;
        if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(context.get(), &rawKey) <= 0) {
            throw std::runtime_error("failed to generate TLS key");
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
            throw std::runtime_error("failed to create self-signed test certificate");
        }
        certificate = directory / "cert.pem";
        privateKey = directory / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certBio(
            BIO_new_file(certificate.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
            BIO_new_file(privateKey.string().c_str(), "w"), BIO_free);
        if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), cert.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                nullptr) != 1) {
            throw std::runtime_error("failed to write test certificate");
        }
    }
    ~IdentityFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path privateKey;

    [[nodiscard]] ruvia::detail::HttpServerListenerDefinition::Tls server_tls_config() const {
        ruvia::detail::HttpServerListenerDefinition::Tls config;
        config.identity.certificateChainFile = certificate.string();
        config.identity.privateKeyFile = privateKey.string();
        return config;
    }
};

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
using TestPrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using TestCertificate = std::unique_ptr<X509, decltype(&X509_free)>;

TestPrivateKey generateTestKey() {
    EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (rawContext == nullptr) {
        throw std::runtime_error("failed to create mTLS test key generator");
    }
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        rawContext, EVP_PKEY_CTX_free);
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
        EVP_PKEY_keygen(context.get(), &rawKey) <= 0) {
        throw std::runtime_error("failed to generate mTLS test key");
    }
    return TestPrivateKey(rawKey, EVP_PKEY_free);
}

void addCertificateExtension(X509* certificate, X509* issuer, int extensionId,
    const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx(&context, issuer == nullptr ? certificate : issuer, certificate,
        nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_conf_nid(nullptr, &context, extensionId, const_cast<char*>(value)),
        X509_EXTENSION_free);
    if (!extension || X509_add_ext(certificate, extension.get(), -1) != 1) {
        throw std::runtime_error("failed to add mTLS test certificate extension");
    }
}

TestCertificate generateTestCertificate(EVP_PKEY* subjectKey, const char* commonName,
    long serial, X509* issuer, EVP_PKEY* issuerKey, bool isAuthority, bool isServer) {
    TestCertificate certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 86400) == nullptr ||
        X509_set_pubkey(certificate.get(), subjectKey) != 1) {
        throw std::runtime_error("failed to initialize mTLS test certificate");
    }

    X509_NAME* const subject = X509_get_subject_name(certificate.get());
    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(),
            issuer == nullptr ? subject : X509_get_subject_name(issuer)) != 1) {
        throw std::runtime_error("failed to set mTLS test certificate names");
    }

    if (isAuthority) {
        addCertificateExtension(certificate.get(), issuer, NID_basic_constraints,
            "critical,CA:TRUE,pathlen:0");
        addCertificateExtension(certificate.get(), issuer, NID_key_usage,
            "critical,keyCertSign,cRLSign");
    } else {
        addCertificateExtension(certificate.get(), issuer, NID_basic_constraints,
            "critical,CA:FALSE");
        addCertificateExtension(certificate.get(), issuer, NID_key_usage,
            isServer ? "critical,digitalSignature,keyEncipherment" : "critical,digitalSignature");
        addCertificateExtension(certificate.get(), issuer, NID_ext_key_usage,
            isServer ? "serverAuth" : "clientAuth");
        addCertificateExtension(certificate.get(), issuer, NID_subject_alt_name, "DNS:localhost");
    }

    EVP_PKEY* const signingKey = issuerKey == nullptr ? subjectKey : issuerKey;
    if (X509_sign(certificate.get(), signingKey, EVP_sha256()) <= 0) {
        throw std::runtime_error("failed to sign mTLS test certificate");
    }
    return certificate;
}

void writeTestCertificate(const std::filesystem::path& certificateFile, X509* certificate) {
    std::unique_ptr<BIO, decltype(&BIO_free)> certificateBio(
        BIO_new_file(certificateFile.string().c_str(), "w"), BIO_free);
    if (!certificateBio || PEM_write_bio_X509(certificateBio.get(), certificate) != 1) {
        throw std::runtime_error("failed to write mTLS test certificate");
    }
}

void writeTestIdentity(const std::filesystem::path& certificateFile,
    const std::filesystem::path& privateKeyFile, X509* certificate, EVP_PKEY* privateKey) {
    std::unique_ptr<BIO, decltype(&BIO_free)> certificateBio(
        BIO_new_file(certificateFile.string().c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(
        BIO_new_file(privateKeyFile.string().c_str(), "w"), BIO_free);
    if (!certificateBio || !keyBio || PEM_write_bio_X509(certificateBio.get(), certificate) != 1 ||
        PEM_write_bio_PrivateKey(keyBio.get(), privateKey, nullptr, nullptr, 0, nullptr,
            nullptr) != 1) {
        throw std::runtime_error("failed to write mTLS test identity");
    }
}

struct MutualTlsFiles final {
    explicit MutualTlsFiles(const std::filesystem::path& directory) {
        trustAnchor = directory / "mtls-trust-anchor.pem";
        serverCertificate = directory / "mtls-server-certificate.pem";
        serverPrivateKey = directory / "mtls-server-private-key.pem";
        trustedClientCertificate = directory / "mtls-trusted-client-certificate.pem";
        trustedClientPrivateKey = directory / "mtls-trusted-client-private-key.pem";
        untrustedClientCertificate = directory / "mtls-untrusted-client-certificate.pem";
        untrustedClientPrivateKey = directory / "mtls-untrusted-client-private-key.pem";

        const TestPrivateKey trustedAuthorityKey = generateTestKey();
        const TestCertificate trustedAuthority = generateTestCertificate(
            trustedAuthorityKey.get(), "ruvia-mtls-test-root", 10, nullptr, nullptr, true, false);
        const TestPrivateKey serverKey = generateTestKey();
        const TestCertificate server = generateTestCertificate(serverKey.get(), "localhost", 11,
            trustedAuthority.get(), trustedAuthorityKey.get(), false, true);
        const TestPrivateKey trustedClientKey = generateTestKey();
        const TestCertificate trustedClient = generateTestCertificate(trustedClientKey.get(),
            "trusted-client", 12, trustedAuthority.get(), trustedAuthorityKey.get(), false, false);
        const TestPrivateKey untrustedAuthorityKey = generateTestKey();
        const TestCertificate untrustedAuthority = generateTestCertificate(
            untrustedAuthorityKey.get(), "ruvia-untrusted-mtls-root", 20, nullptr, nullptr, true,
            false);
        const TestPrivateKey untrustedClientKey = generateTestKey();
        const TestCertificate untrustedClient = generateTestCertificate(untrustedClientKey.get(),
            "untrusted-client", 21, untrustedAuthority.get(), untrustedAuthorityKey.get(), false,
            false);

        writeTestCertificate(trustAnchor, trustedAuthority.get());
        writeTestIdentity(serverCertificate, serverPrivateKey, server.get(), serverKey.get());
        writeTestIdentity(trustedClientCertificate, trustedClientPrivateKey, trustedClient.get(),
            trustedClientKey.get());
        writeTestIdentity(untrustedClientCertificate, untrustedClientPrivateKey,
            untrustedClient.get(), untrustedClientKey.get());
    }

    std::filesystem::path trustAnchor;
    std::filesystem::path serverCertificate;
    std::filesystem::path serverPrivateKey;
    std::filesystem::path trustedClientCertificate;
    std::filesystem::path trustedClientPrivateKey;
    std::filesystem::path untrustedClientCertificate;
    std::filesystem::path untrustedClientPrivateKey;
};
#endif

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::size_t deallocations() const noexcept {
        return deallocations_;
    }
    [[nodiscard]] std::size_t outstanding_bytes() const noexcept {
        return outstanding_bytes_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* const allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations_;
        outstanding_bytes_ += bytes;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        ++deallocations_;
        outstanding_bytes_ -= bytes;
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{};
    std::size_t deallocations_{};
    std::size_t outstanding_bytes_{};
};

class PlainUdpQuicServerFixture final {
public:
    using udp = asio::ip::udp;
    using clock = std::chrono::steady_clock;
    using client_transport = ruvia::detail::http3_quic_client_transport;
    using server_transport_type = ruvia::detail::http3_quic_server_transport;
    using socket_address = ruvia::detail::http3_quic_datagram_address;

    struct Client final {
        Client(udp::socket configured_socket, udp::endpoint configured_local,
            std::unique_ptr<client_transport> configured_transport)
            : socket(std::move(configured_socket)),
              local(std::move(configured_local)),
              transport(std::move(configured_transport)) {}

        udp::socket socket;
        udp::endpoint local;
        std::unique_ptr<client_transport> transport;
    };

    struct AdmittedInitial final {
        ruvia::quic_connection_token token;
        ruvia::quic_initial_offer offer;
    };

    PlainUdpQuicServerFixture(const ruvia::detail::HttpServerListenerDefinition::Tls& tls_config,
        ruvia::quic_server_config server_config = {},
        ruvia::detail::ClientTransportConfigView client_config = {
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : server_socket_(io_),
          server_tls_(tls_config, resource),
          client_tls_(client_config, resource),
          server_(server_tls_, server_config, resource),
          resource_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
        server_socket_.open(udp::v4());
        server_socket_.bind({asio::ip::address_v4::loopback(), 0});
        server_socket_.non_blocking(true);
        server_endpoint_ = server_socket_.local_endpoint();
        server_address_ = toAddress(server_endpoint_);
    }

    PlainUdpQuicServerFixture(const PlainUdpQuicServerFixture&) = delete;
    PlainUdpQuicServerFixture& operator=(const PlainUdpQuicServerFixture&) = delete;

    [[nodiscard]] Client& add_client(std::string_view host = "localhost",
        ruvia::quic_transport_parameters parameters = {}, ruvia::quic_limits limits = {}) {
        udp::socket socket(io_);
        socket.open(udp::v4());
        socket.bind({asio::ip::address_v4::loopback(), 0});
        socket.non_blocking(true);
        const auto local = socket.local_endpoint();
        const auto local_address = toAddress(local);
        ruvia::quic_connection_config config;
        config.local_address = to_quic_address(local_address);
        config.peer_address = to_quic_address(server_address_);
        config.local_transport_parameters = parameters;
        config.limits = limits;
        auto transport = std::make_unique<client_transport>(client_tls_, config, host,
            clock::now(), resource_);
        clients_.emplace_back(std::move(socket), local, std::move(transport));
        return clients_.back();
    }

    // With admit_initials=false, Initial packets are classified and retained as typed
    // offers, but no connection/TLS session is created until admit_pending() is called.
    std::size_t pump(bool admit_initials = true) {
        last_client_packets_ = 0;
        const auto now = clock::now();
        if (const auto expiry = server_.server().next_expiry(); expiry && *expiry <= now) {
            (void)server_.server().handle_expiry(now);
        }
        for (auto& client : clients_) {
            if (const auto expiry = client.transport->next_expiry(); expiry && *expiry <= now) {
                (void)client.transport->handle_expiry(now);
            }
            last_client_packets_ += write_client_packets(client, now);
        }
        receive_server_packets(now, admit_initials);
        if (admit_initials) {
            (void)admit_pending(pending_offers_.size(), now);
        }
        write_server_packets(now);
        receive_client_packets(now);
        return last_client_packets_;
    }

    [[nodiscard]] ruvia::quic_server_admit_result admit_pending(
        std::size_t limit, ruvia::quic_timestamp now = clock::now()) {
        ruvia::quic_server_admit_result last;
        std::size_t admitted{};
        for (auto iterator = pending_offers_.begin(); iterator != pending_offers_.end() &&
                                                      admitted < limit;) {
            last = server_.admit_initial(*iterator, now, "localhost");
            if (last.status == ruvia::quic_operation_status::accepted) {
                admissions_.push_back({last.connection, *iterator});
                iterator = pending_offers_.erase(iterator);
                ++admitted;
            } else if (last.status == ruvia::quic_operation_status::would_block) {
                ++iterator;
            } else {
                iterator = pending_offers_.erase(iterator);
            }
        }
        return last;
    }

    void drive_expiry(ruvia::quic_timestamp now) {
        (void)server_.server().handle_expiry(now);
        for (auto& client : clients_) {
            (void)client.transport->handle_expiry(now);
        }
    }

    template <typename Predicate>
    [[nodiscard]] bool run_until(Predicate&& predicate,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(8),
        bool admit_initials = true) {
        const auto deadline = clock::now() + timeout;
        while (!predicate() && clock::now() < deadline) {
            pump(admit_initials);
            if (!predicate()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return predicate();
    }

    [[nodiscard]] server_transport_type& server_transport() noexcept {
        return server_;
    }
    [[nodiscard]] ruvia::detail::http3_quic_tls_context& server_tls_context() noexcept {
        return server_tls_;
    }
    [[nodiscard]] ruvia::quic_server& protocol_server() noexcept {
        return server_.server();
    }
    [[nodiscard]] ruvia::quic_connection_state connection_state(
        bool server_initiates, const Client& client) {
        const auto token = admissions_.front().token;
        return server_initiates ? client.transport->connection().info().state
                                : server_.server().connection(token).info().state;
    }
    [[nodiscard]] Client& client(std::size_t index) {
        return clients_.at(index);
    }
    void reset_packet_observation() noexcept {
        last_client_packets_ = 0;
    }
    [[nodiscard]] std::size_t last_client_packets() const noexcept {
        return last_client_packets_;
    }
    [[nodiscard]] const std::vector<AdmittedInitial>& admissions() const noexcept {
        return admissions_;
    }
    [[nodiscard]] ruvia::quic_operation_status retire(ruvia::quic_connection_token token) {
        const auto result = server_.server().retire(token);
        if (result == ruvia::quic_operation_status::accepted ||
            result == ruvia::quic_operation_status::retired) {
            std::erase_if(admissions_, [token](const AdmittedInitial& admission) {
                return admission.token == token;
            });
        }
        return result;
    }
    [[nodiscard]] const std::vector<ruvia::quic_initial_offer>& pending_offers() const noexcept {
        return pending_offers_;
    }
    [[nodiscard]] std::optional<ruvia::quic_connection_token> token_for_offer(
        std::uint64_t offer_id) const noexcept {
        const auto found = std::find_if(admissions_.begin(), admissions_.end(),
            [offer_id](const AdmittedInitial& admission) {
                return admission.offer.offer_id == offer_id;
            });
        return found == admissions_.end()
                   ? std::nullopt
                   : std::optional<ruvia::quic_connection_token>(found->token);
    }
    [[nodiscard]] const udp::endpoint& server_endpoint() const noexcept {
        return server_endpoint_;
    }
    [[nodiscard]] std::optional<ruvia::quic_timestamp> next_expiry() const noexcept {
        auto result = server_.server().next_expiry();
        for (const auto& client : clients_) {
            const auto expiry = client.transport->next_expiry();
            if (expiry && (!result || *expiry < *result)) {
                result = expiry;
            }
        }
        return result;
    }
    [[nodiscard]] const socket_address& server_address() const noexcept {
        return server_address_;
    }

private:
    [[nodiscard]] static socket_address toAddress(const udp::endpoint& endpoint) {
        const auto address = ruvia::detail::to_http3_quic_datagram_address(endpoint);
        if (!address) {
            throw std::runtime_error("invalid plain UDP QUIC fixture endpoint");
        }
        return *address;
    }

    std::size_t write_client_packets(Client& client, ruvia::quic_timestamp now) {
        std::size_t packets_written{};
        for (unsigned count = 0; count < 32; ++count) {
            const auto packet = client.transport->write_packet(packet_buffer_, now);
            if (packet.size == 0) {
                return packets_written;
            }
            asio::error_code error;
            const auto sent = client.socket.send_to(
                asio::buffer(packet_buffer_.data(), packet.size), server_endpoint_, 0, error);
            if (error || sent != packet.size) {
                throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                    "send QUIC client UDP packet");
            }
            ++packets_written;
        }
        return packets_written;
    }

    void receive_server_packets(ruvia::quic_timestamp now, bool admit_initials) {
        for (unsigned count = 0; count < 128; ++count) {
            udp::endpoint peer;
            asio::error_code error;
            const auto size = server_socket_.receive_from(asio::buffer(packet_buffer_), peer, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "receive QUIC server UDP packet");
            }
            const auto peer_address = toAddress(peer);
            const auto route = server_.route_datagram(
                std::span<const std::byte>(packet_buffer_).first(size), server_address_, peer_address);
            if (route.kind == ruvia::quic_server_route_kind::initial_offer) {
                const auto found = std::find_if(pending_offers_.begin(), pending_offers_.end(),
                    [&route](const ruvia::quic_initial_offer& offer) {
                        return offer.offer_id == route.offer.offer_id;
                    });
                if (found == pending_offers_.end()) {
                    pending_offers_.push_back(route.offer);
                }
                if (admit_initials) {
                    (void)admit_pending(pending_offers_.size(), now);
                }
            } else if (route.kind == ruvia::quic_server_route_kind::existing_connection) {
                const ruvia::quic_datagram_view datagram{
                    std::span<const std::byte>(packet_buffer_).first(size),
                    to_quic_address(server_address_), to_quic_address(peer_address)};
                (void)server_.server().receive(route.connection, datagram, now);
            }
        }
    }

    void write_server_packets(ruvia::quic_timestamp now) {
        for (const auto& admission : admissions_) {
            auto& connection = server_.server().connection(admission.token);
            for (unsigned count = 0; count < 32; ++count) {
                const auto packet = connection.write_packet(packet_buffer_, now);
                if (packet.size == 0) {
                    break;
                }
                const auto destination = ruvia::detail::to_udp_endpoint(
                    ruvia::detail::from_quic_address(packet.peer));
                if (!destination) {
                    throw std::runtime_error("QUIC server output has invalid peer address");
                }
                asio::error_code error;
                const auto sent = server_socket_.send_to(
                    asio::buffer(packet_buffer_.data(), packet.size), *destination, 0, error);
                if (error || sent != packet.size) {
                    throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                        "send QUIC server UDP packet");
                }
            }
        }
    }

    void receive_client_packets(ruvia::quic_timestamp now) {
        for (auto& client : clients_) {
            for (unsigned count = 0; count < 128; ++count) {
                udp::endpoint peer;
                asio::error_code error;
                const auto size = client.socket.receive_from(asio::buffer(packet_buffer_), peer, 0, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    throw std::system_error(error, "receive QUIC client UDP packet");
                }
                const auto remote = toAddress(peer);
                const ruvia::quic_datagram_view datagram{
                    std::span<const std::byte>(packet_buffer_).first(size),
                    to_quic_address(toAddress(client.local)), to_quic_address(remote)};
                (void)client.transport->receive(datagram, now);
            }
        }
    }

    asio::io_context io_;
    udp::socket server_socket_;
    udp::endpoint server_endpoint_;
    socket_address server_address_;
    ruvia::detail::http3_quic_tls_context server_tls_;
    ruvia::detail::http3_quic_client_tls_context client_tls_;
    server_transport_type server_;
    std::pmr::memory_resource* resource_;
    std::vector<Client> clients_;
    std::vector<ruvia::quic_initial_offer> pending_offers_;
    std::vector<AdmittedInitial> admissions_;
    std::array<std::byte, 65536> packet_buffer_{};
    std::size_t last_client_packets_{};
};

#if OPENSSL_VERSION_NUMBER >= 0x30600000L
struct ClientCertificateObservation final {
    std::size_t callbackCount{};
    bool sawLeaf{};
    bool leafVerified{};
    bool allVerified{true};
    bool sawVerificationFailure{};
    int leafNameLength{};
    std::array<char, 128> leafCommonName{};
};

int clientCertificateObservationIndex() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int observeClientCertificate(int preverifyOk, X509_STORE_CTX* store) noexcept {
    SSL* const ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    SSL_CTX* const context = ssl == nullptr ? nullptr : SSL_get_SSL_CTX(ssl);
    const int index = clientCertificateObservationIndex();
    auto* const observation = context == nullptr || index < 0
                                  ? nullptr
                                  : static_cast<ClientCertificateObservation*>(
                                        SSL_CTX_get_ex_data(context, index));
    if (observation == nullptr) {
        return preverifyOk;
    }
    ++observation->callbackCount;
    if (preverifyOk != 1) {
        observation->allVerified = false;
        observation->sawVerificationFailure = true;
    }
    if (X509_STORE_CTX_get_error_depth(store) == 0) {
        observation->sawLeaf = true;
        observation->leafVerified = preverifyOk == 1;
        X509* const certificate = X509_STORE_CTX_get_current_cert(store);
        if (certificate != nullptr) {
            const int length = X509_NAME_get_text_by_NID(
                X509_get_subject_name(certificate), NID_commonName,
                observation->leafCommonName.data(),
                static_cast<int>(observation->leafCommonName.size()));
            if (length > 0 && length < static_cast<int>(observation->leafCommonName.size())) {
                observation->leafNameLength = length;
            }
        }
    }
    return preverifyOk;
}

class PlainUdpTlsClientPeer final {
public:
    using udp = asio::ip::udp;
    using clock = std::chrono::steady_clock;

    PlainUdpTlsClientPeer(ruvia::detail::ClientTransportConfigView tls_config,
        std::string_view host, const udp::endpoint& remote, std::uint16_t local_port,
        std::pmr::memory_resource* resource)
        : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
          tls_context_(tls_config, resource_),
          socket_(io_),
          crypto_(resource_),
          remote_(remote) {
        socket_.open(udp::v4());
        socket_.bind({asio::ip::address_v4::loopback(), local_port});
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        const auto local_address = ruvia::detail::to_http3_quic_datagram_address(local_);
        const auto peer_address = ruvia::detail::to_http3_quic_datagram_address(remote_);
        if (!local_address || !peer_address) {
            throw std::runtime_error("invalid plain UDP TLS peer endpoint");
        }

        constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
        tls_session_ = std::make_unique<ruvia::detail::openssl_quic_tls_session>(
            tls_context_.native_handle(), ruvia::quic_role::client, h3_alpn, host, resource_);
        tls_context_.prepare(tls_session_->native_handle(), host);

        ruvia::quic_connection_config config;
        config.role = ruvia::quic_role::client;
        config.local_address = ruvia::detail::to_quic_address(*local_address);
        config.peer_address = ruvia::detail::to_quic_address(*peer_address);
        std::array<std::byte, 8> destination_id{};
        std::array<std::byte, 8> source_id{};
        const auto provider = crypto_.view();
        provider.random_bytes(provider.context, destination_id);
        provider.random_bytes(provider.context, source_id);
        config.destination_connection_id = ruvia::quic_connection_id(destination_id);
        config.source_connection_id = ruvia::quic_connection_id(source_id);
        connection_ = std::make_unique<ruvia::quic_connection>(config, provider,
            tls_session_->driver_view(), resource_, clock::now());
    }

    ~PlainUdpTlsClientPeer() {
        if (tls_session_) {
            tls_session_->stop();
        }
        connection_.reset();
        tls_session_.reset();
    }
    PlainUdpTlsClientPeer(const PlainUdpTlsClientPeer&) = delete;
    PlainUdpTlsClientPeer& operator=(const PlainUdpTlsClientPeer&) = delete;

    [[nodiscard]] ruvia::quic_connection& connection() noexcept {
        return *connection_;
    }
    [[nodiscard]] SSL* native_tls_handle() const noexcept {
        return tls_session_->native_handle();
    }
    [[nodiscard]] const udp::endpoint& local_endpoint() const noexcept {
        return local_;
    }

    void send_packets() {
        const auto now = clock::now();
        for (unsigned count = 0; count < 32; ++count) {
            const auto packet = connection_->write_packet(packet_, now);
            if (packet.size == 0) {
                return;
            }
            asio::error_code error;
            const auto sent = socket_.send_to(asio::buffer(packet_.data(), packet.size), remote_, 0, error);
            if (error || sent != packet.size) {
                throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                    "send standalone QUIC TLS peer packet");
            }
        }
        throw std::runtime_error("standalone QUIC TLS peer packet bound exceeded");
    }

    void receive_packets() {
        for (unsigned count = 0; count < 128; ++count) {
            udp::endpoint source;
            asio::error_code error;
            const auto size = socket_.receive_from(asio::buffer(packet_), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "receive standalone QUIC TLS peer packet");
            }
            const auto local_address = ruvia::detail::to_http3_quic_datagram_address(local_);
            const auto peer_address = ruvia::detail::to_http3_quic_datagram_address(source);
            if (!local_address || !peer_address) {
                throw std::runtime_error("invalid received standalone QUIC peer endpoint");
            }
            const ruvia::quic_datagram_view datagram{
                std::span<const std::byte>(packet_).first(size),
                ruvia::detail::to_quic_address(*local_address),
                ruvia::detail::to_quic_address(*peer_address)};
            const auto status = connection_->receive(datagram, clock::now());
            if (status == ruvia::quic_operation_status::draining ||
                status == ruvia::quic_operation_status::retired) {
                saw_remote_close_ = true;
            }
        }
        throw std::runtime_error("standalone QUIC TLS peer receive bound exceeded");
    }

    [[nodiscard]] bool saw_remote_close() const noexcept {
        return saw_remote_close_;
    }
    [[nodiscard]] bool verifies_host(std::string_view host) const noexcept {
        X509* const raw_certificate = SSL_get1_peer_certificate(native_tls_handle());
        if (raw_certificate == nullptr) {
            return false;
        }
        std::unique_ptr<X509, decltype(&X509_free)> certificate(raw_certificate, X509_free);
        return X509_check_host(certificate.get(), host.data(), host.size(), 0, nullptr) == 1;
    }

private:
    std::pmr::memory_resource* resource_;
    ruvia::detail::http3_quic_client_tls_context tls_context_;
    asio::io_context io_;
    udp::socket socket_;
    ruvia::detail::openssl_quic_crypto_provider crypto_;
    udp::endpoint remote_;
    udp::endpoint local_;
    std::unique_ptr<ruvia::detail::openssl_quic_tls_session> tls_session_;
    std::unique_ptr<ruvia::quic_connection> connection_;
    std::array<std::byte, 65536> packet_{};
    bool saw_remote_close_{};
};

struct MutualTlsHandshake final {
    bool acceptedByServer{};
    bool clientReady{};
    bool clientSawRemoteClose{};
    bool clientVerifiesLocalhost{};
    bool serverHandshakeComplete{};
    bool serverH3Negotiated{};
    bool cryptoFailureObserved{};
    std::string_view cryptoFailureStage;
    std::uint8_t serverTlsFailureAlert{};
    long clientVerifyResult{X509_V_ERR_UNSPECIFIED};
    ClientCertificateObservation clientCertificate;
};

MutualTlsHandshake performRequiredMutualTlsHandshake(const MutualTlsFiles& files,
    std::uint16_t local_port, const std::filesystem::path* clientCertificate,
    const std::filesystem::path* clientPrivateKey) {
    CountingMemoryResource memory;
    MutualTlsHandshake result;
    {
        ruvia::detail::HttpServerListenerDefinition::Tls server_config;
        server_config.identity.certificateChainFile = files.serverCertificate.string();
        server_config.identity.privateKeyFile = files.serverPrivateKey.string();
        ruvia::detail::HttpServerListenerDefinition::TlsClientCertificatePolicy client_policy;
        client_policy.verifyFile = files.trustAnchor.string();
        client_policy.requirement = ruvia::TlsClientCertificateRequirement::kRequired;
        server_config.clientCertificates = client_policy;
        PlainUdpQuicServerFixture fixture(server_config, {}, {}, &memory);

        const int observation_index = clientCertificateObservationIndex();
        SSL_CTX* const server_context = fixture.server_tls_context().default_context();
        if (observation_index < 0 || server_context == nullptr ||
            SSL_CTX_set_ex_data(server_context, observation_index, &result.clientCertificate) != 1) {
            throw std::runtime_error("failed to attach server client-certificate observer");
        }
        SSL_CTX_set_verify(server_context, SSL_CTX_get_verify_mode(server_context),
            &observeClientCertificate);

        const std::string client_certificate_file = clientCertificate == nullptr
                                                        ? std::string{}
                                                        : clientCertificate->string();
        const std::string client_private_key_file = clientPrivateKey == nullptr
                                                        ? std::string{}
                                                        : clientPrivateKey->string();
        const auto trust_anchor_file = files.trustAnchor.string();
        const ruvia::detail::ClientTransportConfigView client_config{
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kVerify,
            .caFile = trust_anchor_file,
            .certificateChainFile = client_certificate_file,
            .privateKeyFile = client_private_key_file};
        PlainUdpTlsClientPeer peer(client_config, "localhost", fixture.server_endpoint(),
            local_port, &memory);

        const bool expects_valid_client_certificate =
            clientCertificate == &files.trustedClientCertificate;
        auto drive_stage = [&](std::string_view phase, auto&& operation) -> bool {
            try {
                operation();
                return true;
            } catch (const ruvia::quic_error& error) {
                if (error.code() == ruvia::quic_error_code::crypto_failure &&
                    !expects_valid_client_certificate && !fixture.admissions().empty()) {
                    auto& server = fixture.protocol_server().connection(
                        fixture.admissions().front().token);
                    if (server.tls_handshake().failed()) {
                        result.cryptoFailureObserved = true;
                        result.cryptoFailureStage = phase;
                        result.serverTlsFailureAlert = static_cast<std::uint8_t>(
                            server.tls_handshake().failure_alert());
                        return false;
                    }
                }
                std::throw_with_nested(std::runtime_error(
                    "mTLS handshake driver failed during " + std::string(phase) + ": " + error.what()));
            } catch (const std::exception& error) {
                std::throw_with_nested(std::runtime_error(
                    "mTLS handshake driver failed during " + std::string(phase) + ": " + error.what()));
            }
        };

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!drive_stage("client packet generation/transmit", [&] { peer.send_packets(); })) {
                break;
            }
            if (!drive_stage("server packet processing", [&] {
                    fixture.pump(false);
                    if (fixture.admissions().empty() && !fixture.pending_offers().empty()) {
                        (void)fixture.admit_pending(1);
                    }
                })) {
                break;
            }
            if (!drive_stage("client packet receive/processing", [&] { peer.receive_packets(); })) {
                break;
            }

            const auto client_info = peer.connection().info();
            const bool server_ready = !fixture.admissions().empty() &&
                                      fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete;
            if ((client_info.quic_handshake_complete && server_ready) || peer.saw_remote_close() ||
                client_info.state == ruvia::quic_connection_state::failed ||
                client_info.state == ruvia::quic_connection_state::draining) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (result.cryptoFailureObserved && !fixture.admissions().empty()) {
            auto& server = fixture.protocol_server().connection(fixture.admissions().front().token);
            (void)drive_stage("server TLS-failure close submission", [&] {
                (void)server.close({.kind = ruvia::quic_close_kind::transport,
                    .code = 0x100U + result.serverTlsFailureAlert});
            });
            (void)drive_stage("server TLS-failure close transmission", [&] { fixture.pump(false); });
            (void)drive_stage("client TLS-failure close reception", [&] { peer.receive_packets(); });
        }

        result.acceptedByServer = !fixture.admissions().empty();
        result.clientReady = peer.connection().info().quic_handshake_complete;
        result.clientSawRemoteClose = peer.saw_remote_close();
        result.clientVerifiesLocalhost = peer.verifies_host("localhost");
        result.clientVerifyResult = SSL_get_verify_result(peer.native_tls_handle());
        if (result.acceptedByServer) {
            const auto token = fixture.admissions().front().token;
            auto& server_connection = fixture.protocol_server().connection(token);
            const auto info = server_connection.info();
            result.serverHandshakeComplete = info.quic_handshake_complete && info.tls_handshake_complete;
            const auto alpn = server_connection.tls_handshake().info().negotiated_alpn;
            result.serverH3Negotiated = alpn.size() == 2 &&
                                        std::to_integer<char>(alpn[0]) == 'h' && std::to_integer<char>(alpn[1]) == '3';
        }
    }
    if (memory.allocations() != memory.deallocations() || memory.outstanding_bytes() != 0) {
        throw std::runtime_error("mTLS QUIC fixture leaked PMR allocations");
    }
    return result;
}
#endif

}  // namespace

RUVIA_TEST(http3QuicServerTransportConstructsAndReleasesRepeatedly) {
    IdentityFiles files;
    auto config = files.server_tls_config();
    for (int iteration = 0; iteration != 2; ++iteration) {
        PlainUdpQuicServerFixture fixture(config);
        RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
        RUVIA_CHECK_EQ(fixture.protocol_server().pending_connection_count(), std::size_t{0});
        RUVIA_CHECK(!fixture.next_expiry());
    }
}
RUVIA_TEST(http3QuicServerTransportBoundsPendingInitialsBeforeAccept) {
    IdentityFiles files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections = 2;
    server_config.max_pending_connections = 3;
    PlainUdpQuicServerFixture fixture(config, server_config);
    for (std::size_t index = 0; index != 5; ++index) {
        (void)fixture.add_client();
    }

    fixture.pump(false);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{3});
    RUVIA_CHECK_EQ(fixture.protocol_server().pending_connection_count(), std::size_t{3});
    RUVIA_CHECK(fixture.admissions().empty());
    const auto admitted = fixture.admit_pending(3);
    RUVIA_CHECK_EQ(admitted.status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{2});

    const auto first_token = fixture.admissions().front().token;
    RUVIA_CHECK_EQ(fixture.retire(first_token), ruvia::quic_operation_status::retired);
    const auto resumed = fixture.admit_pending(1);
    RUVIA_CHECK_EQ(resumed.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK(fixture.pending_offers().empty());
}
RUVIA_TEST(http3QuicServerTransportKeepsInterleavedInitialPeerPathsPaired) {
    IdentityFiles files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_pending_connections = 4;
    PlainUdpQuicServerFixture fixture(config, server_config);
    (void)fixture.add_client();
    (void)fixture.add_client();

    fixture.pump(false);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});
    const auto first_offer = fixture.pending_offers()[0];
    const auto second_offer = fixture.pending_offers()[1];
    RUVIA_CHECK(first_offer.offer_id != second_offer.offer_id);
    RUVIA_CHECK(first_offer.peer_address.port != second_offer.peer_address.port);
    RUVIA_CHECK_EQ(fixture.admit_pending(2).status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(fixture.run_until([&] {
        const auto first_token = fixture.token_for_offer(first_offer.offer_id);
        const auto second_token = fixture.token_for_offer(second_offer.offer_id);
        return first_token && second_token &&
               fixture.protocol_server().connection(*first_token).info().quic_handshake_complete &&
               fixture.protocol_server().connection(*second_token).info().quic_handshake_complete &&
               fixture.client(0).transport->connection().info().quic_handshake_complete &&
               fixture.client(1).transport->connection().info().quic_handshake_complete;
    }));

    const auto first_token = fixture.token_for_offer(first_offer.offer_id);
    const auto second_token = fixture.token_for_offer(second_offer.offer_id);
    RUVIA_CHECK(first_token.has_value() && second_token.has_value());
    if (first_token && second_token) {
        RUVIA_CHECK(*first_token != *second_token);
        const auto first_info = fixture.protocol_server().connection(*first_token).info();
        const auto second_info = fixture.protocol_server().connection(*second_token).info();
        RUVIA_CHECK_EQ(first_info.peer_address.port, first_offer.peer_address.port);
        RUVIA_CHECK_EQ(second_info.peer_address.port, second_offer.peer_address.port);
        const auto first_alpn = fixture.protocol_server().connection(*first_token).tls_handshake().info().negotiated_alpn;
        const auto second_alpn = fixture.protocol_server().connection(*second_token).tls_handshake().info().negotiated_alpn;
        RUVIA_CHECK_EQ(first_alpn.size(), std::size_t{2});
        RUVIA_CHECK_EQ(second_alpn.size(), std::size_t{2});
        if (first_alpn.size() == 2 && second_alpn.size() == 2) {
            RUVIA_CHECK_EQ(std::to_integer<char>(first_alpn[0]), 'h');
            RUVIA_CHECK_EQ(std::to_integer<char>(first_alpn[1]), '3');
            RUVIA_CHECK_EQ(std::to_integer<char>(second_alpn[0]), 'h');
            RUVIA_CHECK_EQ(std::to_integer<char>(second_alpn[1]), '3');
        }
    }
}
RUVIA_TEST(http3QuicServerTransportReportsNegotiatedIdleExpiryWithoutActiveStreams) {
    IdentityFiles files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.local_transport_parameters.idle_timeout_ms = 120;
    PlainUdpQuicServerFixture fixture(config, server_config);
    ruvia::quic_transport_parameters client_parameters;
    client_parameters.idle_timeout_ms = 120;
    (void)fixture.add_client("localhost", client_parameters);
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete;
    }));

    const auto token = fixture.admissions().front().token;
    auto& connection = fixture.protocol_server().connection(token);
    RUVIA_CHECK_EQ(connection.info().negotiated_idle_timeout_ms, std::uint64_t{120});
    const auto expiry = connection.next_expiry();
    RUVIA_CHECK(expiry.has_value());
    RUVIA_CHECK(fixture.run_until([&] {
        const auto state = connection.info().state;
        return state == ruvia::quic_connection_state::closing ||
               state == ruvia::quic_connection_state::draining ||
               state == ruvia::quic_connection_state::retired;
    },
        std::chrono::seconds(2)));
}
RUVIA_TEST(http3QuicServerTransportInheritsListenerIdleTimeoutForDeferredPendingHandshakes) {
    const auto verifyNegotiatedTimeout = [&ruvia_ctx](std::uint64_t server_timeout,
                                             std::uint64_t client_timeout,
                                             std::uint64_t expected_timeout) {
        IdentityFiles files;
        auto config = files.server_tls_config();
        ruvia::quic_server_config server_config;
        server_config.local_transport_parameters.idle_timeout_ms = server_timeout;
        PlainUdpQuicServerFixture fixture(config, server_config);
        ruvia::quic_transport_parameters client_parameters;
        client_parameters.idle_timeout_ms = client_timeout;
        (void)fixture.add_client("localhost", client_parameters);

        fixture.pump(false);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK(fixture.admissions().empty());
        RUVIA_CHECK_EQ(fixture.admit_pending(0).status, ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.admit_pending(1).status, ruvia::quic_operation_status::accepted);
        RUVIA_CHECK(fixture.run_until([&] {
            return fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete;
        }));
        RUVIA_CHECK_EQ(fixture.protocol_server().connection(fixture.admissions().front().token).info().negotiated_idle_timeout_ms,
            expected_timeout);
    };

    verifyNegotiatedTimeout(20'000, 90'000, 20'000);
    verifyNegotiatedTimeout(75'000, 120'000, 75'000);
    verifyNegotiatedTimeout(0, 0, 0);
}
RUVIA_TEST(http3QuicServerTransportRequiresPerCallAcceptCredits) {
    IdentityFiles files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections = 2;
    server_config.max_pending_connections = 4;
    PlainUdpQuicServerFixture fixture(config, server_config);
    for (std::size_t index = 0; index != 4; ++index) {
        (void)fixture.add_client();
    }
    fixture.pump(false);

    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{4});
    RUVIA_CHECK(fixture.admissions().empty());
    RUVIA_CHECK_EQ(fixture.admit_pending(0).status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(fixture.admissions().empty());
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{4});

    RUVIA_CHECK_EQ(fixture.admit_pending(2).status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});

    const auto first = fixture.admissions().front().token;
    RUVIA_CHECK_EQ(fixture.retire(first), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
}
RUVIA_TEST(http3QuicTransportBoundsLifetimePeerAdmissionsDespiteRepeatedClose) {
    constexpr std::size_t lifetime_limit = 8;
    const auto exercise_direction = [&ruvia_ctx](bool server_initiates) {
        IdentityFiles files;
        auto config = files.server_tls_config();
        ruvia::quic_server_config server_config;
        server_config.limits.max_lifetime_peer_streams = lifetime_limit;
        server_config.local_transport_parameters.initial_max_streams_bidi = 0;
        server_config.local_transport_parameters.initial_max_streams_uni = lifetime_limit;
        CountingMemoryResource memory;
        {
            const ruvia::detail::ClientTransportConfigView client_tls_config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            PlainUdpQuicServerFixture fixture(config, server_config, client_tls_config, &memory);
            ruvia::quic_transport_parameters client_parameters;
            client_parameters.initial_max_streams_bidi = 0;
            client_parameters.initial_max_streams_uni = lifetime_limit;
            ruvia::quic_limits client_limits;
            client_limits.max_lifetime_peer_streams = lifetime_limit;
            auto& client = fixture.add_client("localhost", client_parameters, client_limits);
            RUVIA_CHECK(fixture.run_until([&] {
                return !fixture.admissions().empty() &&
                       fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete &&
                       client.transport->connection().info().quic_handshake_complete;
            }));

            auto& server = fixture.protocol_server().connection(fixture.admissions().front().token);
            std::size_t admitted{};
            for (std::size_t index = 0; index < lifetime_limit; ++index) {
                auto& sender = server_initiates ? server : client.transport->connection();
                auto& receiver = server_initiates ? client.transport->connection() : server;
                const auto opened = sender.open_stream(true);
                RUVIA_CHECK_EQ(opened.status, ruvia::quic_operation_status::accepted);
                constexpr std::array payload{std::byte{0x21}};
                const auto write = sender.write_stream(opened.stream_id, payload, true);
                RUVIA_CHECK_EQ(write.status, ruvia::quic_operation_status::accepted);
                RUVIA_CHECK_EQ(write.accepted, payload.size());
                bool stream_seen = false;
                const auto stream_ready = fixture.run_until([&] {
                    auto accepted = receiver.accept_streams();
                    stream_seen |= std::ranges::any_of(
                        std::span(accepted.streams).first(accepted.size),
                        [&opened](const ruvia::quic_stream_metadata& item) {
                            return item.stream_id == opened.stream_id;
                        });
                    return stream_seen;
                });
                RUVIA_CHECK(stream_ready);
                if (!stream_ready) {
                    break;
                }

                std::array<std::byte, 8> output{};
                std::size_t bytes_read{};
                bool fin{};
                RUVIA_CHECK(fixture.run_until([&] {
                    const auto read = receiver.read_stream(opened.stream_id, output);
                    if (read.status == ruvia::quic_stream_read_status::data) {
                        bytes_read += read.size;
                    } else if (read.status == ruvia::quic_stream_read_status::fin) {
                        fin = true;
                    }
                    return fin;
                }));
                RUVIA_CHECK(fin);
                RUVIA_CHECK_EQ(bytes_read, payload.size());
                ++admitted;
                const auto sender_close = sender.close_stream(opened.stream_id);
                const auto receiver_close = receiver.close_stream(opened.stream_id);
                RUVIA_CHECK(sender_close == ruvia::quic_operation_status::accepted ||
                            sender_close == ruvia::quic_operation_status::completed);
                RUVIA_CHECK(receiver_close == ruvia::quic_operation_status::accepted ||
                            receiver_close == ruvia::quic_operation_status::completed);
            }
            RUVIA_CHECK_EQ(admitted, lifetime_limit);

            auto& sender = server_initiates ? server : client.transport->connection();
            const auto beyond_limit = sender.open_stream(true);
            RUVIA_CHECK_EQ(beyond_limit.status, ruvia::quic_operation_status::accepted);
            constexpr std::array payload{std::byte{0x21}};
            const auto write = sender.write_stream(beyond_limit.stream_id, payload, true);
            RUVIA_CHECK_EQ(write.status, ruvia::quic_operation_status::accepted);
            bool rejected = false;
            try {
                (void)fixture.run_until([&] {
                    return fixture.connection_state(server_initiates, client) ==
                           ruvia::quic_connection_state::failed;
                });
            } catch (const ruvia::quic_error& error) {
                rejected = error.code() == ruvia::quic_error_code::resource_limit;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(fixture.connection_state(server_initiates, client),
                ruvia::quic_connection_state::failed);
        }
        RUVIA_CHECK_EQ(memory.allocations(), memory.deallocations());
        RUVIA_CHECK_EQ(memory.outstanding_bytes(), std::size_t{0});
    };
    exercise_direction(false);
    exercise_direction(true);
}
RUVIA_TEST(http3QuicServerTransportGracefullyFlushesAnOpenStreamBeforeNoErrorClose) {
    using namespace ruvia::detail;
    IdentityFiles files;
    auto tls_config = files.server_tls_config();
    PlainUdpQuicServerFixture fixture(tls_config);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete &&
               fixture.client(0).transport->connection().info().quic_handshake_complete;
    }));
    const auto token = fixture.admissions().front().token;
    auto& server = fixture.protocol_server().connection(token);
    auto& client = fixture.client(0).transport->connection();

    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        return;
    }
    Http3CriticalStreamDriver critical(*prefixes);
    const auto drive_critical = [&] {
        return critical.drive(
            [&](Http3CriticalStreamDriver::Kind) { return server.open_stream(true); },
            [&](std::uint64_t stream_id, std::span<const char> bytes) {
                return server.write_stream(stream_id, std::as_bytes(bytes));
            });
    };
    RUVIA_CHECK(fixture.run_until([&] {
        const auto result = drive_critical();
        if (result == Http3CriticalStreamDriver::Result::kFatal) {
            throw std::runtime_error("QUIC critical stream setup failed");
        }
        return critical.complete();
    }));
    RUVIA_CHECK(critical.queueGoaway(0));
    RUVIA_CHECK(fixture.run_until([&] {
        const auto result = drive_critical();
        if (result == Http3CriticalStreamDriver::Result::kFatal) {
            throw std::runtime_error("QUIC GOAWAY write failed");
        }
        return critical.complete();
    }));

    constexpr std::array kinds{Http3CriticalStreamDriver::Kind::control,
        Http3CriticalStreamDriver::Kind::qpack_encoder,
        Http3CriticalStreamDriver::Kind::qpack_decoder};
    std::array<std::uint64_t, 4> stream_ids{};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        const auto stream_id = critical.streamId(kinds[index]);
        RUVIA_CHECK(stream_id.has_value());
        if (!stream_id) {
            return;
        }
        stream_ids[index] = *stream_id;
    }
    const auto application = server.open_stream(true);
    RUVIA_CHECK_EQ(application.status, ruvia::quic_operation_status::accepted);
    stream_ids.back() = application.stream_id;

    ruvia::http3_critical_stream_output expected_output(*prefixes);
    RUVIA_CHECK(expected_output.queue_goaway(0));
    std::array<std::string, 4> expected{
        std::string{},
        std::string(prefixes->qpackEncoderPrefix().begin(), prefixes->qpackEncoderPrefix().end()),
        std::string(prefixes->qpackDecoderPrefix().begin(), prefixes->qpackDecoderPrefix().end()),
        "graceful close flushes this stream"};
    auto goaway = expected_output.next(ruvia::http3_critical_stream_output::stream_kind::control);
    expected[0].append(goaway.data(), goaway.size());
    RUVIA_CHECK(expected_output.acknowledge(ruvia::http3_critical_stream_output::stream_kind::control,
        goaway.size()));
    goaway = expected_output.next(ruvia::http3_critical_stream_output::stream_kind::control);
    expected[0].append(goaway.data(), goaway.size());

    const auto payload = std::as_bytes(std::span(expected.back().data(), expected.back().size()));
    const auto write = server.write_stream(application.stream_id, payload, true);
    RUVIA_CHECK_EQ(write.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(write.accepted, payload.size());

    std::array<bool, 4> accepted_streams{};
    std::array<std::string, 4> received{};
    bool application_fin{};
    std::array<std::byte, 128> read_buffer{};
    RUVIA_CHECK(fixture.run_until([&] {
        const auto accepted = client.accept_streams();
        for (std::size_t item = 0; item < accepted.size; ++item) {
            const auto found = std::find(stream_ids.begin(), stream_ids.end(),
                accepted.streams[item].stream_id);
            if (found != stream_ids.end()) {
                accepted_streams[static_cast<std::size_t>(found - stream_ids.begin())] = true;
            }
        }
        for (std::size_t index = 0; index < stream_ids.size(); ++index) {
            if (!accepted_streams[index]) {
                continue;
            }
            const auto read = client.read_stream(stream_ids[index], read_buffer);
            if (read.status == ruvia::quic_stream_read_status::data) {
                received[index].append(reinterpret_cast<const char*>(read_buffer.data()), read.size);
            } else if (read.status == ruvia::quic_stream_read_status::fin && index == 3) {
                application_fin = true;
            }
        }
        return std::ranges::all_of(accepted_streams, [](bool stream_accepted) { return stream_accepted; }) &&
               std::ranges::equal(received, expected) && application_fin;
    },
        std::chrono::seconds(8)));
    RUVIA_CHECK(std::ranges::all_of(accepted_streams, [](bool stream_accepted) { return stream_accepted; }));
    RUVIA_CHECK(std::ranges::equal(received, expected));
    RUVIA_CHECK(application_fin);

    // Drive pending acknowledgments without requiring a new ACK after the receive loop.
    fixture.pump();
    constexpr std::array<char, 0> no_reason{};
    const ruvia::quic_close_reason_view no_error_close{
        .kind = ruvia::quic_close_kind::application,
        .code = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kNoError),
        .frame_type = 0,
        .reason = no_reason};
    RUVIA_CHECK_EQ(server.close(no_error_close), ruvia::quic_operation_status::accepted);
    const auto close_code = no_error_close.code;
    RUVIA_CHECK(fixture.run_until([&] {
        const auto info = client.info();
        return info.state == ruvia::quic_connection_state::draining ||
               info.state == ruvia::quic_connection_state::closing ||
               info.close_error_code == close_code;
    }));
    RUVIA_CHECK_EQ(client.info().close_error_code, close_code);
    RUVIA_CHECK_EQ(fixture.retire(token), ruvia::quic_operation_status::retired);
}
RUVIA_TEST(http3QuicServerTransportRequestsWireCloseBeforeExplicitLocalRetirement) {
    IdentityFiles files;
    auto tls_config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections = 1;
    CountingMemoryResource memory;
    {
        PlainUdpQuicServerFixture fixture(tls_config, server_config,
            {.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification}, &memory);
        (void)fixture.add_client();
        RUVIA_CHECK(fixture.run_until([&] {
            return !fixture.admissions().empty() &&
                   fixture.protocol_server().connection(fixture.admissions().front().token).info().confirmed &&
                   fixture.client(0).transport->connection().info().confirmed;
        }));
        const auto token = fixture.admissions().front().token;
        auto& server = fixture.protocol_server().connection(token);
        auto& first_client = fixture.client(0).transport->connection();

        (void)fixture.add_client();
        fixture.pump(false);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.admit_pending(1).status,
            ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});

        const auto stream = server.open_stream(true);
        RUVIA_CHECK_EQ(stream.status, ruvia::quic_operation_status::accepted);
        std::vector<std::byte> owned_input(4U * 1024U * 1024U, std::byte{'x'});
        std::size_t accepted_bytes{};
        bool backpressured{};
        for (std::size_t attempt = 0; attempt < 64 && !backpressured; ++attempt) {
            const auto result = server.write_stream(stream.stream_id,
                std::span<const std::byte>(owned_input).subspan(accepted_bytes));
            if (result.status == ruvia::quic_operation_status::would_block) {
                backpressured = true;
            } else {
                RUVIA_CHECK_EQ(result.status, ruvia::quic_operation_status::accepted);
                RUVIA_CHECK(result.accepted > 0);
                accepted_bytes += result.accepted;
            }
        }
        RUVIA_CHECK(backpressured);
        RUVIA_CHECK(accepted_bytes > 0);
        owned_input.clear();
        owned_input.shrink_to_fit();

        constexpr std::array<char, 0> no_reason{};
        const ruvia::quic_close_reason_view close_reason{
            .kind = ruvia::quic_close_kind::application,
            .code = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestRejected),
            .frame_type = 0,
            .reason = no_reason};
        RUVIA_CHECK_EQ(server.close(close_reason), ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(server.close(close_reason), ruvia::quic_operation_status::accepted);
        constexpr std::array<char, 0> conflicting_reason{};
        const ruvia::quic_close_reason_view conflict{
            .kind = ruvia::quic_close_kind::application,
            .code = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kGeneralProtocolError),
            .frame_type = 0,
            .reason = conflicting_reason};
        RUVIA_CHECK_EQ(server.close(conflict), ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(server.info().state, ruvia::quic_connection_state::closing);
        RUVIA_CHECK_EQ(server.info().close_error_code, close_reason.code);

        RUVIA_CHECK(fixture.run_until([&] {
            const auto info = first_client.info();
            return info.state == ruvia::quic_connection_state::draining ||
                   info.state == ruvia::quic_connection_state::closing ||
                   info.close_error_code == close_reason.code;
        }));
        RUVIA_CHECK_EQ(first_client.info().close_error_code, close_reason.code);
        RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.retire(token), ruvia::quic_operation_status::retired);
        RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)fixture.protocol_server().connection(token); }));

        RUVIA_CHECK_EQ(fixture.admit_pending(1).status, ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{1});
        RUVIA_CHECK(fixture.run_until([&] {
            return fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete;
        }));
    }
    RUVIA_CHECK_EQ(memory.allocations(), memory.deallocations());
    RUVIA_CHECK_EQ(memory.outstanding_bytes(), std::size_t{0});
}
RUVIA_TEST(http3QuicServerTransportLocallyRetiresWithUnsentUdpPacketWithoutPumping) {
    IdentityFiles files;
    auto tls_config = files.server_tls_config();
    PlainUdpQuicServerFixture fixture(tls_config);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token).info().quic_handshake_complete;
    }));
    const auto token = fixture.admissions().front().token;
    auto& connection = fixture.protocol_server().connection(token);
    constexpr std::array<char, 0> reason{};
    const ruvia::quic_close_reason_view close_reason{
        .kind = ruvia::quic_close_kind::application,
        .code = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kInternalError),
        .frame_type = 0,
        .reason = reason};
    RUVIA_CHECK_EQ(connection.close(close_reason), ruvia::quic_operation_status::accepted);

    std::array<std::byte, 65536> packet_storage{};
    const auto packet = connection.write_packet(packet_storage, PlainUdpQuicServerFixture::clock::now());
    RUVIA_CHECK(packet.size > 0);
    const auto retained = std::vector<std::byte>(packet_storage.begin(),
        packet_storage.begin() + static_cast<std::ptrdiff_t>(packet.size));
    RUVIA_CHECK_EQ(fixture.retire(token), ruvia::quic_operation_status::retired);
    RUVIA_CHECK(std::equal(retained.begin(), retained.end(), packet_storage.begin()));
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)fixture.protocol_server().connection(token); }));
}
RUVIA_TEST(http3QuicServerTransportZeroCapacityAndRepeatedCloseAreSafe) {
    IdentityFiles files;
    auto tls_config = files.server_tls_config();
    ruvia::quic_server_config zero_config;
    zero_config.max_active_connections = 0;
    PlainUdpQuicServerFixture fixture(tls_config, zero_config);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] { return !fixture.pending_offers().empty(); },
        std::chrono::seconds(8), false));
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
    ruvia::detail::http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());

    ruvia::quic_server_config valid_config;
    valid_config.max_active_connections = 1;
    ruvia::detail::http3_quic_server_transport transport(tls, valid_config);
    const ruvia::quic_connection_token stale{1};
    RUVIA_CHECK_EQ(transport.server().retire(stale), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(transport.server().retire(stale), ruvia::quic_operation_status::retired);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)transport.server().connection(stale); }));
}
RUVIA_TEST(http3QuicServerTransportRejectsInvalidConfigurationWithoutTakingSocketOwnership) {
    IdentityFiles files;
    auto tls_config = files.server_tls_config();
    ruvia::detail::http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
    asio::io_context io;
    asio::ip::udp::socket owned_socket(io);
    owned_socket.open(asio::ip::udp::v4());
    owned_socket.bind({asio::ip::address_v4::loopback(), 0});

    std::array<ruvia::quic_server_config, 4> invalid_configs{};
    invalid_configs[0].local_transport_parameters.max_udp_payload_size = 1199;
    invalid_configs[1].local_transport_parameters.active_connection_id_limit = 1;
    invalid_configs[2].limits.max_datagram_size = 0;
    invalid_configs[3].limits.max_lifetime_peer_streams = 0;
    for (const auto& config : invalid_configs) {
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            ruvia::detail::http3_quic_server_transport invalid(tls, config);
        }));
        RUVIA_CHECK(owned_socket.is_open());
        RUVIA_CHECK(owned_socket.local_endpoint().port() != 0);
    }
    ruvia::detail::http3_quic_server_transport valid(tls);
    RUVIA_CHECK(owned_socket.is_open());
}
RUVIA_TEST(http3QuicServerTransportRejectsInvalidUdpAddressWithoutTakingSocketOwnership) {
    asio::io_context io;
    asio::ip::udp::socket socket(io);
    socket.open(asio::ip::udp::v4());
    socket.bind({asio::ip::address_v4::loopback(), 0});
    const auto owned_endpoint = socket.local_endpoint();
    RUVIA_CHECK(owned_endpoint.port() != 0);

    const asio::ip::udp::endpoint zero_port(asio::ip::address_v4::loopback(), 0);
    const auto invalid_peer = ruvia::detail::to_http3_quic_datagram_address(zero_port);
    RUVIA_CHECK(!invalid_peer.has_value());
    RUVIA_CHECK_EQ(invalid_peer.error(),
        ruvia::detail::http3_quic_socket_address_error::zero_port);

    ruvia::detail::http3_quic_datagram_address invalid_address;
    invalid_address.port = 0;
    const auto invalid_output = ruvia::detail::to_udp_endpoint(invalid_address);
    RUVIA_CHECK(!invalid_output.has_value());
    RUVIA_CHECK_EQ(invalid_output.error(),
        ruvia::detail::http3_quic_socket_address_error::zero_port);
    RUVIA_CHECK(socket.is_open());
    RUVIA_CHECK_EQ(socket.local_endpoint(), owned_endpoint);
}
#if OPENSSL_VERSION_NUMBER >= 0x30600000L
RUVIA_TEST(http3QuicServerTransportRequiresAndValidatesMutualTlsCertificates) {
    IdentityFiles temporaryFiles;
    MutualTlsFiles files(temporaryFiles.directory);

    const auto trusted = performRequiredMutualTlsHandshake(files, 15440,
        &files.trustedClientCertificate, &files.trustedClientPrivateKey);
    RUVIA_CHECK(trusted.acceptedByServer);
    RUVIA_CHECK(trusted.clientReady);
    RUVIA_CHECK(trusted.clientVerifiesLocalhost);
    RUVIA_CHECK(trusted.serverHandshakeComplete);
    RUVIA_CHECK(trusted.serverH3Negotiated);
    RUVIA_CHECK_EQ(trusted.clientVerifyResult, X509_V_OK);
    RUVIA_CHECK(trusted.clientCertificate.callbackCount > 0);
    RUVIA_CHECK(trusted.clientCertificate.sawLeaf);
    RUVIA_CHECK(trusted.clientCertificate.leafVerified);
    RUVIA_CHECK(trusted.clientCertificate.allVerified);
    RUVIA_CHECK(!trusted.clientCertificate.sawVerificationFailure);
    RUVIA_CHECK_EQ(std::string_view(trusted.clientCertificate.leafCommonName.data(),
                       static_cast<std::size_t>(trusted.clientCertificate.leafNameLength)),
        std::string_view("trusted-client"));

    const auto missing = performRequiredMutualTlsHandshake(files, 15441, nullptr, nullptr);
    RUVIA_CHECK(missing.acceptedByServer);
    RUVIA_CHECK(missing.clientVerifiesLocalhost);
    RUVIA_CHECK(missing.clientSawRemoteClose);
    RUVIA_CHECK(!missing.serverHandshakeComplete);
    RUVIA_CHECK_EQ(missing.clientVerifyResult, X509_V_OK);
    RUVIA_CHECK(!missing.clientCertificate.sawLeaf);
    RUVIA_CHECK_EQ(missing.clientCertificate.callbackCount, std::size_t{0});

    const auto untrusted = performRequiredMutualTlsHandshake(files, 15442,
        &files.untrustedClientCertificate, &files.untrustedClientPrivateKey);
    RUVIA_CHECK(untrusted.acceptedByServer);
    RUVIA_CHECK(untrusted.clientVerifiesLocalhost);
    RUVIA_CHECK(untrusted.clientSawRemoteClose);
    RUVIA_CHECK(!untrusted.serverHandshakeComplete);
    RUVIA_CHECK_EQ(untrusted.clientVerifyResult, X509_V_OK);
    RUVIA_CHECK(untrusted.clientCertificate.sawLeaf);
    RUVIA_CHECK(!untrusted.clientCertificate.allVerified);
    RUVIA_CHECK(untrusted.clientCertificate.sawVerificationFailure);
    RUVIA_CHECK_EQ(std::string_view(untrusted.clientCertificate.leafCommonName.data(),
                       static_cast<std::size_t>(untrusted.clientCertificate.leafNameLength)),
        std::string_view("untrusted-client"));
}
#endif
