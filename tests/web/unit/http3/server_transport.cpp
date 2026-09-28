#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <thread>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

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

ruvia::detail::Http3QuicDatagramAddress address() {
    ruvia::detail::Http3QuicDatagramAddress result;
    result.address[0] = 127;
    result.address[3] = 1;
    result.port = 4433;
    return result;
}

class ToggleFailingMemoryResource final : public std::pmr::memory_resource {
public:
    bool failAllocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (failAllocations) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class DefaultMemoryResourceScope final {
public:
    explicit DefaultMemoryResourceScope(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}
    ~DefaultMemoryResourceScope() {
        std::pmr::set_default_resource(previous_);
    }
    DefaultMemoryResourceScope(const DefaultMemoryResourceScope&) = delete;
    DefaultMemoryResourceScope& operator=(const DefaultMemoryResourceScope&) = delete;

private:
    std::pmr::memory_resource* previous_;
};

class DirectQuicPeer final {
public:
    DirectQuicPeer(ruvia::detail::Http3QuicClientTlsContext& tls,
        ruvia::detail::Http3QuicDatagramAddress remote,
        ruvia::detail::Http3QuicDatagramAddress local)
        : remote_(remote),
          local_(local),
          bridge_(local) {
        connection_ = SSL_new(tls.nativeHandle());
        if (connection_ == nullptr) {
            throw std::runtime_error("failed to create direct QUIC test peer");
        }
        try {
            tls.prepare(connection_, "localhost");
            std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)> destination(
                BIO_ADDR_new(), BIO_ADDR_free);
            if (!destination || !ruvia::detail::makeHttp3QuicBioAddress(remote_, destination.get()) ||
                SSL_set1_initial_peer_addr(connection_, destination.get()) != 1 ||
                SSL_set_default_stream_mode(connection_, SSL_DEFAULT_STREAM_MODE_NONE) != 1 ||
                SSL_set_incoming_stream_policy(connection_, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0) != 1) {
                throw std::runtime_error("failed to configure direct QUIC test peer");
            }
            BIO* const bio = bridge_.releaseSslBio();
            if (bio == nullptr) {
                throw std::runtime_error("direct QUIC test peer has no SSL-side BIO");
            }
            SSL_set_bio(connection_, bio, bio);
            if (SSL_set_blocking_mode(connection_, 0) != 1) {
                throw std::runtime_error("failed to configure nonblocking direct QUIC test peer");
            }
        } catch (...) {
            SSL_free(connection_);
            connection_ = nullptr;
            throw;
        }
    }

    ~DirectQuicPeer() {
        SSL_free(connection_);
    }
    DirectQuicPeer(const DirectQuicPeer&) = delete;
    DirectQuicPeer& operator=(const DirectQuicPeer&) = delete;

    [[nodiscard]] ruvia::detail::Http3QuicDatagramBridge& bridge() noexcept {
        return bridge_;
    }
    [[nodiscard]] const ruvia::detail::Http3QuicDatagramAddress& remote() const noexcept {
        return remote_;
    }
    [[nodiscard]] const ruvia::detail::Http3QuicDatagramAddress& local() const noexcept {
        return local_;
    }
    [[nodiscard]] bool ready() const noexcept {
        return SSL_is_init_finished(connection_) == 1;
    }
    [[nodiscard]] long verifyResult() const noexcept {
        return SSL_get_verify_result(connection_);
    }
    [[nodiscard]] bool verifiesHost(std::string_view host) const noexcept {
        X509_VERIFY_PARAM* const parameters = SSL_get0_param(connection_);
        if (parameters == nullptr) {
            return false;
        }
        char* const configuredHost = X509_VERIFY_PARAM_get0_host(parameters, 0);
        return configuredHost != nullptr && std::string_view(configuredHost) == host;
    }

    void handleEvents() {
        ERR_clear_error();
        const int eventResult = SSL_handle_events(connection_);
        if (eventResult != 1) {
            const int error = SSL_get_error(connection_, eventResult);
            ERR_clear_error();
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE &&
                error != SSL_ERROR_WANT_ACCEPT) {
                SSL_CONN_CLOSE_INFO close{};
                if (!connectionCloseInfo(close)) {
                    throw std::runtime_error("direct QUIC peer event handling failed");
                }
                return;
            }
        } else {
            ERR_clear_error();
        }
        if (!ready()) {
            ERR_clear_error();
            const int connectResult = SSL_connect(connection_);
            if (connectResult != 1) {
                const int error = SSL_get_error(connection_, connectResult);
                ERR_clear_error();
                if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE &&
                    error != SSL_ERROR_WANT_ACCEPT) {
                    SSL_CONN_CLOSE_INFO close{};
                    if (!connectionCloseInfo(close)) {
                        throw std::runtime_error("direct QUIC peer handshake failed");
                    }
                }
            } else {
                ERR_clear_error();
            }
        }
    }

    [[nodiscard]] SSL* acceptPeerStream() const {
        ERR_clear_error();
        SSL* const stream = SSL_accept_stream(connection_, SSL_ACCEPT_STREAM_NO_BLOCK);
        if (stream == nullptr) {
            ERR_clear_error();
            return nullptr;
        }
        ERR_clear_error();
        return stream;
    }

    [[nodiscard]] bool connectionCloseInfo(SSL_CONN_CLOSE_INFO& close) const {
        close = {};
        ERR_clear_error();
        const int result = SSL_get_conn_close_info(connection_, &close, sizeof(close));
        ERR_clear_error();
        return result == 1;
    }

private:
    ruvia::detail::Http3QuicDatagramAddress remote_;
    ruvia::detail::Http3QuicDatagramAddress local_;
    ruvia::detail::Http3QuicDatagramBridge bridge_;
    SSL* connection_{};
};

std::size_t relayDatagrams(ruvia::detail::Http3QuicDatagramBridge& from,
    ruvia::detail::Http3QuicDatagramBridge& to,
    const ruvia::detail::Http3QuicDatagramAddress& source) {
    std::size_t packets{};
    for (unsigned packet = 0; packet < 128; ++packet) {
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        const auto result = from.takeOutbound(outbound);
        if (result == ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty) {
            return packets;
        }
        if (result != ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kReady ||
            to.inject(outbound.bytes, source) !=
                ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted) {
            throw std::runtime_error("direct QUIC test datagram relay failed");
        }
        from.completeOutbound();
        ++packets;
    }
    throw std::runtime_error("direct QUIC test exceeded its datagram relay bound");
}

std::size_t relayDatagramsToPeer(ruvia::detail::Http3QuicDatagramBridge& from,
    DirectQuicPeer& peer) {
    std::size_t packets{};
    for (unsigned packet = 0; packet < 128; ++packet) {
        ruvia::detail::Http3QuicOutboundDatagram outbound;
        const auto result = from.takeOutbound(outbound);
        if (result == ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kEmpty) {
            return packets;
        }
        if (result != ruvia::detail::Http3QuicDatagramBridge::OutboundResult::kReady) {
            throw std::runtime_error("direct QUIC test server outbound BIO failed");
        }
        bool delivered{};
        for (unsigned retry = 0; retry < 128 && !delivered; ++retry) {
            const auto injected = peer.bridge().inject(outbound.bytes, peer.remote());
            if (injected == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kAccepted) {
                delivered = true;
            } else if (injected == ruvia::detail::Http3QuicDatagramBridge::InjectResult::kFull) {
                peer.handleEvents();
            } else {
                throw std::runtime_error("direct QUIC test peer rejected server datagram");
            }
        }
        if (!delivered) {
            throw std::runtime_error("direct QUIC test peer BIO stayed full");
        }
        from.completeOutbound();
        ++packets;
        peer.handleEvents();
    }
    throw std::runtime_error("direct QUIC test exceeded its peer relay bound");
}

std::pair<std::size_t, std::size_t> pumpDirectPair(ruvia::detail::Http3QuicServerTransport& server,
    ruvia::detail::Http3QuicDatagramBridge& serverBridge, DirectQuicPeer& peer,
    std::optional<ruvia::detail::Http3QuicServerTransport::ConnectionId>& connectionId,
    bool tolerateServerFatal = false) {
    const auto toServer = relayDatagrams(peer.bridge(), serverBridge, peer.local());
    if (server.handleEvents() ==
            ruvia::detail::Http3QuicServerTransport::EventResult::kFatal &&
        !tolerateServerFatal) {
        throw std::runtime_error("direct QUIC test server event handling failed");
    }
    if (!connectionId) {
        const auto accepted = server.acceptConnections(1);
        if (accepted.size != 0) {
            connectionId = accepted.ids[0];
        }
    }
    const auto toPeer = relayDatagramsToPeer(serverBridge, peer);
    peer.handleEvents();
    return {toServer, toPeer};
}

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

struct MutualTlsHandshake final {
    bool acceptedByServer{};
    bool clientReady{};
    bool clientSawRemoteClose{};
    bool clientVerifiesLocalhost{};
    bool serverHandshakeComplete{};
    bool serverH3Negotiated{};
    long clientVerifyResult{X509_V_ERR_UNSPECIFIED};
    ClientCertificateObservation clientCertificate;
};

MutualTlsHandshake performRequiredMutualTlsHandshake(const MutualTlsFiles& files,
    std::uint16_t localPort, const std::filesystem::path* clientCertificate,
    const std::filesystem::path* clientPrivateKey) {
    using namespace ruvia::detail;
    using Clock = std::chrono::steady_clock;

    ClientCertificateObservation observation;
    const std::string serverCertificate = files.serverCertificate.string();
    const std::string serverPrivateKey = files.serverPrivateKey.string();
    const std::string trustAnchor = files.trustAnchor.string();
    HttpServerListenerDefinition::Tls serverConfig;
    serverConfig.identity.certificateChainFile = serverCertificate;
    serverConfig.identity.privateKeyFile = serverPrivateKey;
    HttpServerListenerDefinition::TlsClientCertificatePolicy clientPolicy;
    clientPolicy.verifyFile = trustAnchor;
    clientPolicy.requirement = ruvia::TlsClientCertificateRequirement::kRequired;
    serverConfig.clientCertificates = clientPolicy;
    Http3QuicTlsContext serverTls(serverConfig, std::pmr::get_default_resource());

    const int observationIndex = clientCertificateObservationIndex();
    SSL_CTX* const serverContext = serverTls.defaultContext();
    if (observationIndex < 0 || serverContext == nullptr ||
        SSL_CTX_set_ex_data(serverContext, observationIndex, &observation) != 1) {
        throw std::runtime_error("failed to install server mTLS certificate observer");
    }
    SSL_CTX_set_verify(serverContext, SSL_CTX_get_verify_mode(serverContext),
        &observeClientCertificate);

    const std::string clientCertificateFile = clientCertificate == nullptr
                                                  ? std::string{}
                                                  : clientCertificate->string();
    const std::string clientPrivateKeyFile = clientPrivateKey == nullptr
                                                 ? std::string{}
                                                 : clientPrivateKey->string();
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{
        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kVerify,
        .caFile = trustAnchor,
        .certificateChainFile = clientCertificateFile,
        .privateKeyFile = clientPrivateKeyFile,
    });

    Http3QuicDatagramBridge serverBridge(address());
    Http3QuicServerTransport server(serverTls, serverBridge);
    auto local = address();
    local.port = localPort;
    DirectQuicPeer peer(clientTls, address(), local);
    std::optional<Http3QuicServerTransport::ConnectionId> connectionId;
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    for (std::size_t step = 0; Clock::now() < deadline && step < 2000; ++step) {
        const auto info = connectionId ? server.connectionInfo(*connectionId) : std::nullopt;
        if (peer.ready() && info && info->handshakeComplete) {
            break;
        }
        SSL_CONN_CLOSE_INFO close{};
        if (peer.connectionCloseInfo(close)) {
            break;
        }
        (void)pumpDirectPair(server, serverBridge, peer, connectionId, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    MutualTlsHandshake result;
    result.acceptedByServer = connectionId.has_value();
    result.clientReady = peer.ready();
    result.clientVerifiesLocalhost = peer.verifiesHost("localhost");
    SSL_CONN_CLOSE_INFO close{};
    result.clientSawRemoteClose = peer.connectionCloseInfo(close) &&
                                  (close.flags & SSL_CONN_CLOSE_FLAG_LOCAL) == 0;
    result.clientVerifyResult = peer.verifyResult();
    result.clientCertificate = observation;
    if (connectionId) {
        const auto info = server.connectionInfo(*connectionId);
        result.serverHandshakeComplete = info && info->handshakeComplete;
        result.serverH3Negotiated = info && info->h3Negotiated;
        (void)server.retireConnectionLocally(*connectionId);
    }
    return result;
}
#endif

}  // namespace

RUVIA_TEST(http3QuicServerTransportConstructsAndReleasesRepeatedly) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext tls(config, std::pmr::get_default_resource());

    for (int iteration = 0; iteration != 2; ++iteration) {
        Http3QuicDatagramBridge bridge(address());
        Http3QuicServerTransport transport(tls, bridge);
        RUVIA_CHECK(transport.acceptConnections(1).empty());
        const auto deadline = transport.eventTimeout();
        RUVIA_CHECK(!deadline || *deadline >= Http3QuicServerTransport::Duration::zero());
        const auto result = transport.handleEvents();
        RUVIA_CHECK(result != Http3QuicServerTransport::EventResult::kFatal);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportBoundsPendingInitialsBeforeAccept) {
#if !defined(SSL_VALUE_QUIC_MAX_PENDING_CONNS) || OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext tls(config, std::pmr::get_default_resource());
    Http3QuicDatagramBridge serverBio(address());
    Http3QuicServerTransport server(
        tls, serverBio, {.idleTimeout = std::chrono::seconds(25)});
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    struct Peer final {
        Peer(Http3QuicClientTlsContext& tls, std::uint16_t port)
            : local(address()),
              bridge(withPort(port)),
              transport(tls, bridge, address(), "localhost") {
            local.port = port;
        }
        static Http3QuicDatagramAddress withPort(std::uint16_t port) {
            auto value = address();
            value.port = port;
            return value;
        }
        Http3QuicDatagramAddress local;
        Http3QuicDatagramBridge bridge;
        Http3QuicClientTransport transport;
    };
    std::vector<std::unique_ptr<Peer>> peers;
    Http3QuicServerTransport::AcceptedBatch earlyAdmission;
    const auto initiate = [&](std::uint16_t port, bool completeHandshake = false,
                              bool acceptBeforeHandshakeProgress = false) {
        auto peer = std::make_unique<Peer>(clientTls, port);
        (void)peer->transport.startConnect();
        // Normally stop after Initial/Retry/token-bearing Initial, leaving the
        // queued peer unauthenticated. The final peer completes its handshake.
        for (unsigned flight = 0; flight < (completeHandshake ? 12U : 2U); ++flight) {
            for (unsigned packet = 0; packet < 16; ++packet) {
                Http3QuicOutboundDatagram output;
                if (peer->bridge.takeOutbound(output) !=
                    Http3QuicDatagramBridge::OutboundResult::kReady) {
                    break;
                }
                RUVIA_CHECK(serverBio.inject(output.bytes, peer->local) ==
                            Http3QuicDatagramBridge::InjectResult::kAccepted);
                peer->bridge.completeOutbound();
            }
            RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);
            if (acceptBeforeHandshakeProgress && flight == 1) {
                earlyAdmission = server.acceptConnections(1);
                RUVIA_CHECK_EQ(earlyAdmission.size, std::size_t{1});
            }
            for (unsigned packet = 0; packet < 32; ++packet) {
                Http3QuicOutboundDatagram output;
                if (serverBio.takeOutbound(output) !=
                    Http3QuicDatagramBridge::OutboundResult::kReady) {
                    break;
                }
                if ((flight == 0 || completeHandshake) && output.destination.port == port) {
                    RUVIA_CHECK(peer->bridge.inject(output.bytes, address()) ==
                                Http3QuicDatagramBridge::InjectResult::kAccepted);
                }
                serverBio.completeOutbound();
            }
            if (flight == 0 || completeHandshake) {
                (void)peer->transport.handleEvents();
            }
        }
        peers.push_back(std::move(peer));
    };
    for (std::size_t i = 0; i < Http3QuicServerTransport::kMaxPendingConnections + 4; ++i) {
        initiate(static_cast<std::uint16_t>(15000 + i));
    }
    const auto beforeAccept = Http3QuicServerTransport::Clock::now();
    RUVIA_CHECK(server.acceptConnections(0).empty());
    RUVIA_CHECK(server.acceptConnections(0).empty());
    const auto firstCredit = server.acceptConnections(1);
    const auto secondCredit = server.acceptConnections(1);
    RUVIA_CHECK_EQ(firstCredit.size, std::size_t{1});
    RUVIA_CHECK_EQ(secondCredit.size, std::size_t{1});
    const auto remaining = server.acceptConnections(Http3QuicServerTransport::kMaxPendingConnections);
    RUVIA_CHECK_EQ(firstCredit.size + secondCredit.size + remaining.size,
        Http3QuicServerTransport::kMaxPendingConnections);
    Http3QuicServerTransport::AcceptedBatch admitted;
    for (const auto* batch : {&firstCredit, &secondCredit, &remaining}) {
        for (std::size_t index = 0; index < batch->size; ++index) {
            admitted.ids[admitted.size++] = batch->ids[index];
        }
    }
    RUVIA_CHECK(server.acceptConnections(1).empty());
    for (std::size_t i = 0; i < admitted.size; ++i) {
        const auto pending = server.connectionInfo(admitted.ids[i]);
        RUVIA_CHECK(pending && !pending->handshakeComplete);
        RUVIA_CHECK(pending->remoteAddress.empty());
        RUVIA_CHECK_EQ(pending->remotePort, std::uint16_t{0});
    }
    RUVIA_CHECK_EQ(server.retireExpiredHandshakes(beforeAccept), std::size_t{0});
    const auto timeout = server.eventTimeout();
    RUVIA_CHECK(timeout && *timeout <= std::chrono::seconds(10));
    RUVIA_CHECK_EQ(server.retireExpiredHandshakes(Http3QuicServerTransport::Clock::time_point::max()),
        admitted.size);
    for (std::size_t i = 0; i < admitted.size; ++i) {
        RUVIA_CHECK(!server.connectionInfo(admitted.ids[i]));
    }
    // Verification is disabled only for this self-signed unit-test handshake.
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    initiate(16000, true, true);
    const auto resumed = earlyAdmission;
    RUVIA_CHECK_EQ(resumed.size, std::size_t{1});
    auto& livePeer = *peers.back();
    for (unsigned tick = 0; tick < 200; ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void)livePeer.transport.handleEvents();
        for (unsigned packet = 0; packet < 16; ++packet) {
            Http3QuicOutboundDatagram output;
            if (livePeer.bridge.takeOutbound(output) != Http3QuicDatagramBridge::OutboundResult::kReady) {
                break;
            }
            RUVIA_CHECK(serverBio.inject(output.bytes, livePeer.local) == Http3QuicDatagramBridge::InjectResult::kAccepted);
            livePeer.bridge.completeOutbound();
        }
        RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);
        for (unsigned packet = 0; packet < 32; ++packet) {
            Http3QuicOutboundDatagram output;
            if (serverBio.takeOutbound(output) != Http3QuicDatagramBridge::OutboundResult::kReady) {
                break;
            }
            if (output.destination.port == livePeer.local.port) {
                RUVIA_CHECK(livePeer.bridge.inject(output.bytes, address()) == Http3QuicDatagramBridge::InjectResult::kAccepted);
            }
            serverBio.completeOutbound();
        }
    }
    RUVIA_CHECK(livePeer.transport.connectionInfo() == Http3QuicClientTransport::State::kH3Ready);
    for (std::size_t i = 0; i < resumed.size; ++i) {
        const auto connected = server.connectionInfo(resumed.ids[i]);
        RUVIA_CHECK(connected && connected->handshakeComplete && connected->h3Negotiated);
        // The accepted-child BIO in this queued-admission loopback has no
        // concrete endpoint, so publish the Initial-observed path after the
        // completed handshake confirms that it belongs to this connection.
        RUVIA_CHECK(connected.has_value());
        if (connected) {
            RUVIA_CHECK_EQ(connected->remoteAddress, std::string_view("127.0.0.1"));
            RUVIA_CHECK_EQ(connected->remotePort, std::uint16_t{16000});
        }
        RUVIA_CHECK_EQ(connected->negotiatedIdleTimeoutMilliseconds,
            std::uint64_t{25'000});
        RUVIA_CHECK_EQ(server.retireExpiredHandshakes(Http3QuicServerTransport::Clock::time_point::max()),
            std::size_t{0});
        RUVIA_CHECK(server.connectionInfo(resumed.ids[i]).has_value());
        RUVIA_CHECK(server.retireConnectionLocally(resumed.ids[i]) == Http3QuicServerTransport::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportKeepsInterleavedInitialPeerPathsPaired) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace std::chrono_literals;
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    const auto serverAddress = address();
    Http3QuicDatagramBridge serverBridge(serverAddress);
    Http3QuicServerTransport server(serverTls, serverBridge);
    auto firstAddress = address();
    auto secondAddress = address();
    firstAddress.port = 16101;
    secondAddress.port = 16102;
    DirectQuicPeer first(clientTls, serverAddress, firstAddress);
    DirectQuicPeer second(clientTls, serverAddress, secondAddress);
    std::array<DirectQuicPeer*, 2> peers{&first, &second};
    std::array<std::optional<Http3QuicServerTransport::ConnectionId>, 2> connections{};
    std::size_t connectionCount{};

    const auto relayClientToServer = [&](DirectQuicPeer& peer) {
        const auto packets = relayDatagrams(peer.bridge(), serverBridge, peer.local());
        if (packets != 0 &&
            server.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
            throw std::runtime_error("interleaved QUIC server event handling failed");
        }
    };
    const auto relayServerToClients = [&] {
        for (unsigned packet = 0; packet < 128; ++packet) {
            Http3QuicOutboundDatagram outbound;
            const auto result = serverBridge.takeOutbound(outbound);
            if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                return;
            }
            if (result != Http3QuicDatagramBridge::OutboundResult::kReady) {
                throw std::runtime_error("interleaved QUIC server outbound BIO failed");
            }
            DirectQuicPeer* destination{};
            for (auto* peer : peers) {
                if (outbound.destination.port == peer->local().port) {
                    destination = peer;
                    break;
                }
            }
            if (destination == nullptr ||
                destination->bridge().inject(outbound.bytes, serverAddress) !=
                    Http3QuicDatagramBridge::InjectResult::kAccepted) {
                serverBridge.completeOutbound();
                throw std::runtime_error("interleaved QUIC response had no matching client");
            }
            serverBridge.completeOutbound();
            destination->handleEvents();
        }
        throw std::runtime_error("interleaved QUIC test exceeded its server packet bound");
    };

    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline && connectionCount != connections.size()) {
        // Advance both clients before alternating their datagrams through the
        // server, which drives after each sender to retain the matching source.
        first.handleEvents();
        second.handleEvents();
        relayClientToServer(first);
        relayClientToServer(second);
        if (server.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
            throw std::runtime_error("interleaved QUIC server event handling failed");
        }
        relayServerToClients();

        const auto accepted = server.acceptConnections(connections.size() - connectionCount);
        for (std::size_t i = 0; i < accepted.size; ++i) {
            connections[connectionCount++] = accepted.ids[i];
        }
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK_EQ(connectionCount, connections.size());
    if (connectionCount != connections.size()) {
        throw std::runtime_error("interleaved QUIC client handshake deadline");
    }
    RUVIA_CHECK(connections[0].has_value() && connections[1].has_value());

    const auto handshakeDeadline = std::chrono::steady_clock::now() + 8s;
    bool handshakesComplete{};
    while (std::chrono::steady_clock::now() < handshakeDeadline && !handshakesComplete) {
        first.handleEvents();
        second.handleEvents();
        relayClientToServer(first);
        relayClientToServer(second);
        if (server.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
            throw std::runtime_error("interleaved QUIC server handshake drive failed");
        }
        relayServerToClients();
        handshakesComplete = first.ready() && second.ready();
        for (const auto connection : connections) {
            const auto info = server.connectionInfo(*connection);
            handshakesComplete = handshakesComplete && info && info->handshakeComplete &&
                                 info->h3Negotiated;
        }
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(handshakesComplete);
    for (std::size_t i = 0; i < connections.size(); ++i) {
        const auto info = server.connectionInfo(*connections[i]);
        RUVIA_CHECK(info.has_value());
        if (info) {
            RUVIA_CHECK_EQ(info->remoteAddress, std::string_view("127.0.0.1"));
            RUVIA_CHECK_EQ(info->remotePort, i == 0 ? firstAddress.port : secondAddress.port);
        }
        RUVIA_CHECK(server.retireConnectionLocally(*connections[i]) ==
                    Http3QuicServerTransport::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportReportsNegotiatedIdleExpiryWithoutActiveStreams) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace std::chrono_literals;
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    Http3QuicDatagramBridge serverBridge(address());
    Http3QuicServerTransport server(serverTls, serverBridge, {.idleTimeout = 120ms});
    auto local = address();
    local.port = 16001;
    DirectQuicPeer peer(clientTls, address(), local);
    std::optional<Http3QuicServerTransport::ConnectionId> connectionId;
    const auto handshakeDeadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < handshakeDeadline) {
        (void)pumpDirectPair(server, serverBridge, peer, connectionId);
        const auto info = connectionId ? server.connectionInfo(*connectionId) : std::nullopt;
        if (peer.ready() && info && info->handshakeComplete) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(connectionId.has_value());
    const auto connected = connectionId ? server.connectionInfo(*connectionId) : std::nullopt;
    RUVIA_CHECK(connected && connected->handshakeComplete && connected->h3Negotiated);
    if (connected) {
        RUVIA_CHECK_EQ(connected->negotiatedIdleTimeoutMilliseconds, std::uint64_t{120});
    }

    bool terminated = false;
    const auto idleDeadline = std::chrono::steady_clock::now() + 2s;
    while (connectionId && std::chrono::steady_clock::now() < idleDeadline) {
        std::this_thread::sleep_for(5ms);
        RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);
        const auto info = server.connectionInfo(*connectionId);
        if (info && info->terminated) {
            terminated = true;
            break;
        }
    }
    RUVIA_CHECK(terminated);
    if (connectionId) {
        RUVIA_CHECK(server.retireConnectionLocally(*connectionId) ==
                    Http3QuicServerTransport::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportRequiresPerCallAcceptCredits) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    Http3QuicDatagramBridge serverBridge(address());
    ToggleFailingMemoryResource connectionMemory;
    DefaultMemoryResourceScope defaultMemory(connectionMemory);
    Http3QuicServerTransport server(serverTls, serverBridge, {.maxActiveConnections = 2});

    auto localOne = address();
    localOne.port = 15450;
    auto localTwo = address();
    localTwo.port = 15451;
    auto localThree = address();
    localThree.port = 15452;
    auto localFour = address();
    localFour.port = 15453;
    DirectQuicPeer firstPeer(clientTls, address(), localOne);
    DirectQuicPeer secondPeer(clientTls, address(), localTwo);
    DirectQuicPeer thirdPeer(clientTls, address(), localThree);
    DirectQuicPeer fourthPeer(clientTls, address(), localFour);
    const auto enqueuePeer = [&](DirectQuicPeer& peer) {
        peer.handleEvents();
        for (unsigned flight = 0; flight < 2; ++flight) {
            (void)relayDatagrams(peer.bridge(), serverBridge, peer.local());
            RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);
            for (unsigned packet = 0; packet < 32; ++packet) {
                Http3QuicOutboundDatagram output;
                const auto result = serverBridge.takeOutbound(output);
                if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                    break;
                }
                RUVIA_CHECK(result == Http3QuicDatagramBridge::OutboundResult::kReady);
                if (flight == 0 && output.destination.port == peer.local().port) {
                    RUVIA_CHECK(peer.bridge().inject(output.bytes, address()) ==
                                Http3QuicDatagramBridge::InjectResult::kAccepted);
                }
                serverBridge.completeOutbound();
            }
            if (flight == 0) {
                peer.handleEvents();
            }
        }
    };
    enqueuePeer(firstPeer);
    enqueuePeer(secondPeer);
    enqueuePeer(thirdPeer);
    enqueuePeer(fourthPeer);

    RUVIA_CHECK(server.acceptConnections(0).empty());
    RUVIA_CHECK(server.acceptConnections(0).empty());
    connectionMemory.failAllocations = true;
    const bool allocationFailed = ruvia::testing::throwsOn([&] {
        (void)server.acceptConnections(1);
    });
    connectionMemory.failAllocations = false;
    RUVIA_CHECK(allocationFailed);
    RUVIA_CHECK(!server.connectionInfo(1));
    RUVIA_CHECK(server.retireConnectionLocally(1) == Http3QuicServerTransport::Error::kNoConnection);

    const auto first = server.acceptConnections(1);
    const auto second = server.acceptConnections(1);
    RUVIA_CHECK_EQ(first.size, std::size_t{1});
    RUVIA_CHECK_EQ(second.size, std::size_t{1});
    if (first.size != 0 && second.size != 0) {
        RUVIA_CHECK(first.ids[0] != second.ids[0]);
    }
    // A fourth queued Initial remains untouched while active capacity is exhausted.
    RUVIA_CHECK(server.acceptConnections(1).empty());
    if (first.size != 0) {
        RUVIA_CHECK(server.retireConnectionLocally(first.ids[0]) ==
                    Http3QuicServerTransport::Error::kNone);
    }
    // A later zero-credit call neither reuses nor consumes a revoked credit.
    RUVIA_CHECK(server.acceptConnections(0).empty());
    const auto third = server.acceptConnections(1);
    RUVIA_CHECK_EQ(third.size, std::size_t{1});
    if (second.size != 0) {
        RUVIA_CHECK(server.retireConnectionLocally(second.ids[0]) ==
                    Http3QuicServerTransport::Error::kNone);
    }
    if (third.size != 0) {
        RUVIA_CHECK(server.retireConnectionLocally(third.ids[0]) ==
                    Http3QuicServerTransport::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicTransportBoundsLifetimePeerAdmissionsDespiteRepeatedClose) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    using Clock = std::chrono::steady_clock;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    constexpr std::size_t kServerLifetimeLimit = 8;
    for (const bool serverInitiates : {false, true}) {
        auto clientAddress = address();
        clientAddress.port = 15001;
        Http3QuicDatagramBridge serverBio(address()), clientBio(clientAddress);
        Http3QuicServerTransport server(serverTls, serverBio,
            {.maxLifetimePeerStreams = kServerLifetimeLimit});
        Http3QuicClientTransport client(clientTls, clientBio, address(), "localhost");
        std::optional<std::uint64_t> connection;
        const auto relay = [](Http3QuicDatagramBridge& from, Http3QuicDatagramBridge& to,
                               Http3QuicDatagramAddress source) {
            for (unsigned packet = 0; packet < 64; ++packet) {
                Http3QuicOutboundDatagram output;
                if (from.takeOutbound(output) != Http3QuicDatagramBridge::OutboundResult::kReady) {
                    return;
                }
                if (to.inject(output.bytes, source) != Http3QuicDatagramBridge::InjectResult::kAccepted) {
                    throw std::runtime_error("QUIC test datagram queue filled");
                }
                from.completeOutbound();
            }
            throw std::runtime_error("QUIC test exceeded datagram work bound");
        };
        const auto pump = [&] {
            relay(clientBio, serverBio, clientAddress);
            if (server.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
                throw std::runtime_error("QUIC test server event failure");
            }
            if (!connection) {
                const auto accepted = server.acceptConnections(1);
                if (accepted.size) {
                    connection = accepted.ids[0];
                }
            }
            relay(serverBio, clientBio, address());
            (void)client.handleEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        (void)client.startConnect();
        bool ready = false;
        while (Clock::now() < deadline && !ready) {
            pump();
            ready = connection && server.connectionInfo(*connection)->handshakeComplete &&
                    client.connectionInfo() == Http3QuicClientTransport::State::kH3Ready;
        }
        if (!ready) {
            throw std::runtime_error("QUIC test handshake deadline");
        }
        const std::size_t lifetimeLimit = serverInitiates
                                              ? Http3QuicStreamSet::kMaxLifetimePeerStreams
                                              : kServerLifetimeLimit;
        std::size_t admitted = 0;
        bool limited = false;
        for (std::size_t stream = 0; stream <= lifetimeLimit; ++stream) {
            Http3QuicStreamSet::OpenStream opened;
            do {
                opened = serverInitiates ? server.openLocalUnidirectionalStream(*connection)
                                         : client.openLocalUnidirectionalStream();
                if (opened.error == Http3QuicStreamSet::Error::kNone) {
                    break;
                }
                if (opened.error != Http3QuicStreamSet::Error::kStreamLimitRetry || Clock::now() >= deadline) {
                    throw std::runtime_error("QUIC test could not open peer stream");
                }
                pump();
            } while (true);
            constexpr std::array<char, 1> unknownType{0x21};
            const auto sent = serverInitiates ? server.writeStream(*connection, opened.id, unknownType)
                                              : client.writeStream(opened.id, unknownType);
            RUVIA_CHECK(sent.status == Http3QuicStreamSet::StreamWrite::Status::kAccepted && sent.bytes == 1);
            RUVIA_CHECK((serverInitiates ? server.finishStream(*connection, opened.id) : client.finishStream(opened.id)) ==
                        Http3QuicStreamSet::Error::kNone);
            Http3QuicStreamSet::AcceptedStreams accepted;
            do {
                pump();
                accepted = serverInitiates ? client.acceptPeerStreams() : server.acceptStreams(*connection);
                if (accepted.error != Http3QuicStreamSet::Error::kNone || accepted.size != 0) {
                    break;
                }
            } while (Clock::now() < deadline);
            if (stream == lifetimeLimit) {
                limited = accepted.error == Http3QuicStreamSet::Error::kConnectionPeerStreamLimit && accepted.size == 0;
                const auto repeated = serverInitiates ? client.acceptPeerStreams() : server.acceptStreams(*connection);
                RUVIA_CHECK(repeated.error == Http3QuicStreamSet::Error::kConnectionPeerStreamLimit);
                break;
            }
            if (accepted.error != Http3QuicStreamSet::Error::kNone || accepted.size != 1) {
                throw std::runtime_error("QUIC test peer admission failed before lifetime bound");
            }
            ++admitted;
            bool fin = false;
            std::size_t bytes = 0;
            std::array<char, 16> buffer{};
            while (Clock::now() < deadline && !fin) {
                const auto read = serverInitiates ? client.readStream(opened.id, buffer)
                                                  : server.readStream(*connection, opened.id, buffer);
                if (read.status == Http3QuicStreamSet::StreamRead::Status::kData) {
                    bytes += read.size;
                } else if (read.status == Http3QuicStreamSet::StreamRead::Status::kFin) {
                    fin = true;
                } else if (read.status != Http3QuicStreamSet::StreamRead::Status::kWouldBlock) {
                    throw std::runtime_error("QUIC test stream read failed");
                }
                if (!fin) {
                    pump();
                }
            }
            RUVIA_CHECK(fin && bytes == 1);
            RUVIA_CHECK(client.closeStream(opened.id) == Http3QuicStreamSet::Error::kNone);
            RUVIA_CHECK(server.closeStream(*connection, opened.id) == Http3QuicStreamSet::Error::kNone);
        }
        RUVIA_CHECK_EQ(admitted, lifetimeLimit);
        RUVIA_CHECK(limited);
        client.close();
        RUVIA_CHECK(server.retireConnectionLocally(*connection) == Http3QuicServerTransport::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportGracefullyFlushesAnOpenStreamBeforeNoErrorClose) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    using Clock = std::chrono::steady_clock;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    using Udp = asio::ip::udp;
    asio::io_context io;
    Udp::socket serverSocket(io, {asio::ip::address_v4::loopback(), 0});
    Udp::socket peerSocket(io, {asio::ip::address_v4::loopback(), 0});
    serverSocket.non_blocking(true);
    peerSocket.non_blocking(true);
    const auto serverAddress = toHttp3QuicDatagramAddress(serverSocket.local_endpoint());
    const auto peerAddress = toHttp3QuicDatagramAddress(peerSocket.local_endpoint());
    RUVIA_CHECK(serverAddress.has_value() && peerAddress.has_value());
    if (!serverAddress || !peerAddress) {
        return;
    }
    Http3QuicDatagramBridge serverBridge(*serverAddress);
    Http3QuicServerTransport server(serverTls, serverBridge);
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    DirectQuicPeer peer(clientTls, *serverAddress, *peerAddress);
    const auto sendOutput = [](Http3QuicDatagramBridge& bridge, Udp::socket& socket) {
        for (unsigned packet = 0; packet < 64; ++packet) {
            Http3QuicOutboundDatagram outbound;
            const auto result = bridge.takeOutbound(outbound);
            if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                return;
            }
            if (result != Http3QuicDatagramBridge::OutboundResult::kReady) {
                throw std::runtime_error("loopback QUIC bridge has an invalid outbound lease");
            }
            const auto destination = toHttp3UdpEndpoint(outbound.destination);
            if (!destination) {
                bridge.completeOutbound();
                throw std::runtime_error("loopback QUIC datagram has no concrete destination");
            }
            asio::error_code error;
            const auto size = socket.send_to(asio::buffer(outbound.bytes.data(), outbound.bytes.size()),
                *destination, 0, error);
            bridge.completeOutbound();
            if (error || size != outbound.bytes.size()) {
                throw std::runtime_error("loopback QUIC datagram send failed");
            }
        }
        throw std::runtime_error("loopback QUIC send exceeded its packet bound");
    };
    const auto receiveInput = [](Udp::socket& socket, Http3QuicDatagramBridge& bridge,
                                  const Http3QuicDatagramAddress& localAddress) {
        std::array<std::byte, 65536> packet{};
        for (unsigned count = 0; count < 64; ++count) {
            Udp::endpoint source;
            asio::error_code error;
            const auto size = socket.receive_from(asio::buffer(packet), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::runtime_error("loopback QUIC datagram receive failed");
            }
            const auto receivedPeerAddress = toHttp3QuicDatagramAddress(source);
            if (!receivedPeerAddress || bridge.inject(
                                            std::span<const std::byte>(packet.data(), size),
                                            *receivedPeerAddress, localAddress) !=
                                            Http3QuicDatagramBridge::InjectResult::kAccepted) {
                throw std::runtime_error("loopback QUIC bridge rejected a received datagram");
            }
        }
        throw std::runtime_error("loopback QUIC receive exceeded its packet bound");
    };
    std::size_t handshakeSteps{};
    const auto pump = [&] {
        sendOutput(peer.bridge(), peerSocket);
        receiveInput(serverSocket, serverBridge, *serverAddress);
        if (server.handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
            throw std::runtime_error("loopback QUIC server event handling failed");
        }
        sendOutput(serverBridge, serverSocket);
        receiveInput(peerSocket, peer.bridge(), *peerAddress);
        peer.handleEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    peer.handleEvents();
    std::optional<Http3QuicServerTransport::ConnectionId> connectionId;
    const auto handshakeDeadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < handshakeDeadline && handshakeSteps < 2000 &&
           (!connectionId || !peer.ready() ||
               !server.connectionInfo(*connectionId)->handshakeComplete)) {
        pump();
        if (!connectionId) {
            const auto accepted = server.acceptConnections(1);
            if (accepted.size != 0) {
                connectionId = accepted.ids[0];
            }
        }
        ++handshakeSteps;
    }
    RUVIA_CHECK(connectionId.has_value());
    if (!connectionId) {
        throw std::runtime_error("graceful-close QUIC handshake deadline");
    }

    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        throw std::runtime_error("failed to create critical stream prefixes");
    }
    Http3CriticalStreamDriver critical(*prefixes);
    const auto driveCritical = [&] {
        return critical.drive(
            [&](Http3CriticalStreamDriver::Kind) {
                return server.openLocalUnidirectionalStream(*connectionId);
            },
            [&](Http3QuicStreamSet::StreamId id, std::span<const char> bytes) {
                return server.writeStream(*connectionId, id, bytes);
            });
    };
    const auto criticalDeadline = Clock::now() + std::chrono::seconds(3);
    while (!critical.complete() && Clock::now() < criticalDeadline) {
        const auto result = driveCritical();
        if (result == Http3CriticalStreamDriver::Result::kFatal) {
            throw std::runtime_error("critical stream setup failed before graceful close");
        }
        if (result == Http3CriticalStreamDriver::Result::kBlocked) {
            pump();
        }
    }
    RUVIA_CHECK(critical.complete());
    RUVIA_CHECK(critical.queueGoaway(0));
    while (!critical.complete() && Clock::now() < criticalDeadline) {
        const auto result = driveCritical();
        if (result == Http3CriticalStreamDriver::Result::kFatal) {
            throw std::runtime_error("GOAWAY write failed before graceful close");
        }
        if (result == Http3CriticalStreamDriver::Result::kBlocked) {
            pump();
        }
    }
    RUVIA_CHECK(critical.complete());
    constexpr std::array criticalKinds{Http3CriticalStreamDriver::Kind::kControl,
        Http3CriticalStreamDriver::Kind::kQpackEncoder,
        Http3CriticalStreamDriver::Kind::kQpackDecoder};
    std::array<Http3QuicStreamSet::StreamId, criticalKinds.size()> criticalIds{};
    for (std::size_t i = 0; i < criticalKinds.size(); ++i) {
        const auto id = critical.streamId(criticalKinds[i]);
        RUVIA_CHECK(id.has_value());
        if (!id) {
            throw std::runtime_error("critical stream was not opened");
        }
        criticalIds[i] = *id;
    }

    const auto stream = server.openLocalUnidirectionalStream(*connectionId);
    RUVIA_CHECK(stream.error == Http3QuicStreamSet::Error::kNone);
    constexpr std::string_view payload = "graceful close flushes this stream";
    std::size_t acceptedBytes{};
    const auto writeDeadline = Clock::now() + std::chrono::seconds(3);
    while (acceptedBytes < payload.size() && Clock::now() < writeDeadline) {
        const auto pending = payload.substr(acceptedBytes);
        const auto write = server.writeStream(*connectionId, stream.id, pending);
        if (write.status == Http3QuicStreamSet::StreamWrite::Status::kWouldBlock) {
            pump();
            continue;
        }
        if (write.status != Http3QuicStreamSet::StreamWrite::Status::kAccepted ||
            write.bytes == 0 || write.bytes > pending.size()) {
            break;
        }
        acceptedBytes += write.bytes;
    }
    RUVIA_CHECK_EQ(acceptedBytes, payload.size());
    RUVIA_CHECK(server.finishStream(*connectionId, stream.id) ==
                Http3QuicStreamSet::Error::kNone);

    Http3CriticalStreamOutput expectedOutput(*prefixes);
    RUVIA_CHECK(expectedOutput.queueGoaway(0));
    std::string expectedControl;
    auto expectedBytes = expectedOutput.next(Http3CriticalStreamOutput::Kind::kControl);
    expectedControl.append(expectedBytes.data(), expectedBytes.size());
    RUVIA_CHECK(expectedOutput.acknowledge(Http3CriticalStreamOutput::Kind::kControl,
        expectedBytes.size()));
    expectedBytes = expectedOutput.next(Http3CriticalStreamOutput::Kind::kControl);
    expectedControl.append(expectedBytes.data(), expectedBytes.size());
    std::array<std::string, criticalKinds.size()> expectedCritical{
        std::move(expectedControl),
        std::string(prefixes->qpackEncoderPrefix().begin(), prefixes->qpackEncoderPrefix().end()),
        std::string(prefixes->qpackDecoderPrefix().begin(), prefixes->qpackDecoderPrefix().end())};

    struct IncomingStream final {
        std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
        std::string received;
        bool fin{};
    };
    std::array<IncomingStream, criticalKinds.size()> incomingCritical;
    std::unique_ptr<SSL, decltype(&SSL_free)> incomingApplicationStream(nullptr, SSL_free);
    std::array<char, 128> readBuffer{};
    bool shutdownRequested{};
    std::string receivedApplication;
    bool receivedApplicationFin{};
    const auto collectPeerStreams = [&] {
        while (SSL* const incoming = peer.acceptPeerStream()) {
            const auto id = SSL_get_stream_id(incoming);
            const auto found = std::find(criticalIds.begin(), criticalIds.end(), id);
            if (found != criticalIds.end()) {
                const auto index = static_cast<std::size_t>(found - criticalIds.begin());
                incomingCritical[index].ssl.reset(incoming);
            } else if (id == stream.id && !incomingApplicationStream) {
                incomingApplicationStream.reset(incoming);
            } else {
                SSL_free(incoming);
                throw std::runtime_error("loopback peer accepted an unexpected stream");
            }
        }
        const auto read = [&](SSL* ssl, std::string& output, bool& fin) {
            if (shutdownRequested || ssl == nullptr || fin) {
                return;
            }
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                ERR_clear_error();
                std::size_t readSize{};
                const int readResult = SSL_read_ex(ssl, readBuffer.data(), readBuffer.size(),
                    &readSize);
                if (readResult == 1) {
                    output.append(readBuffer.data(), readSize);
                    continue;
                }
                const int error = SSL_get_error(ssl, readResult);
                ERR_clear_error();
                if (error == SSL_ERROR_ZERO_RETURN) {
                    fin = true;
                    return;
                }
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                    error == SSL_ERROR_WANT_ACCEPT) {
                    return;
                }
                SSL_CONN_CLOSE_INFO closeInfo{};
                const bool closed = peer.connectionCloseInfo(closeInfo);
                throw std::runtime_error("loopback peer stream read failed: " +
                                         std::to_string(error) + ", stream=" +
                                         std::to_string(SSL_get_stream_id(ssl)) + ", state=" +
                                         std::to_string(SSL_get_stream_read_state(ssl)) +
                                         ", type=" + std::to_string(SSL_get_stream_type(ssl)) +
                                         ", close=" + std::to_string(closed) + ", code=" +
                                         std::to_string(closeInfo.error_code));
            }
        };
        for (auto& incoming : incomingCritical) {
            read(incoming.ssl.get(), incoming.received, incoming.fin);
        }
        read(incomingApplicationStream.get(), receivedApplication, receivedApplicationFin);
    };
    const auto criticalBytesReceived = [&] {
        for (std::size_t i = 0; i < incomingCritical.size(); ++i) {
            if (!incomingCritical[i].ssl || incomingCritical[i].received != expectedCritical[i]) {
                return false;
            }
        }
        return true;
    };

    const auto allStreamBytesDelivered = [&] {
        return criticalBytesReceived() && receivedApplication == payload &&
               receivedApplicationFin;
    };
    const auto criticalDeliveryDeadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < criticalDeliveryDeadline && !allStreamBytesDelivered()) {
        pump();
        collectPeerStreams();
    }
    RUVIA_CHECK(criticalBytesReceived());
    RUVIA_CHECK_EQ(receivedApplication, payload);
    RUVIA_CHECK(receivedApplicationFin);
    for (const auto& incoming : incomingCritical) {
        RUVIA_CHECK(incoming.ssl != nullptr);
        RUVIA_CHECK(!incoming.fin);
    }

    // Keep the peer's latest ACK packets on its BIO. This makes the first
    // graceful-close drive observe a genuine delayed-ACK state.
    std::vector<std::vector<std::byte>> delayedPeerPackets;
    for (unsigned packet = 0; packet < 64; ++packet) {
        Http3QuicOutboundDatagram outbound;
        const auto result = peer.bridge().takeOutbound(outbound);
        if (result == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
            break;
        }
        if (result != Http3QuicDatagramBridge::OutboundResult::kReady) {
            throw std::runtime_error("loopback peer ACK BIO was not ready");
        }
        delayedPeerPackets.emplace_back(outbound.bytes.begin(), outbound.bytes.end());
        peer.bridge().completeOutbound();
    }
    RUVIA_CHECK(!delayedPeerPackets.empty());

    auto close = server.requestGracefulConnectionClose(*connectionId);
    shutdownRequested = true;
    RUVIA_CHECK(close.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending);
    RUVIA_CHECK(!close.allStreamsRetired);
    RUVIA_CHECK(server.writeStream(*connectionId, stream.id, payload).status ==
                Http3QuicStreamSet::StreamWrite::Status::kClosed);
    for (const auto& incoming : incomingCritical) {
        RUVIA_CHECK(!incoming.fin);
    }
    for (unsigned retry = 0; retry < 3; ++retry) {
        RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);
        close = server.requestGracefulConnectionClose(*connectionId);
        RUVIA_CHECK(close.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending);
        RUVIA_CHECK(!close.allStreamsRetired);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (const auto& packet : delayedPeerPackets) {
        RUVIA_CHECK(serverBridge.inject(packet, *peerAddress) ==
                    Http3QuicDatagramBridge::InjectResult::kAccepted);
    }
    RUVIA_CHECK(server.handleEvents() != Http3QuicServerTransport::EventResult::kFatal);

    SSL_CONN_CLOSE_INFO peerClose{};
    const auto closeDeadline = Clock::now() + std::chrono::seconds(5);
    for (std::size_t step = 0; Clock::now() < closeDeadline && step < 3000; ++step) {
        pump();
        collectPeerStreams();
        close = server.requestGracefulConnectionClose(*connectionId);
        if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kFailure ||
            close.status == Http3QuicServerTransport::ConnectionCloseStatus::kConflict) {
            break;
        }
        if (peer.connectionCloseInfo(peerClose) && receivedApplicationFin &&
            close.status == Http3QuicServerTransport::ConnectionCloseStatus::kCompleted &&
            close.allStreamsRetired) {
            break;
        }
    }
    RUVIA_CHECK_EQ(receivedApplication, payload);
    RUVIA_CHECK(receivedApplicationFin);
    for (const auto& incoming : incomingCritical) {
        RUVIA_CHECK(incoming.ssl != nullptr);
        RUVIA_CHECK(!incoming.fin);
    }
    RUVIA_CHECK(peer.connectionCloseInfo(peerClose));
    RUVIA_CHECK_EQ(peerClose.error_code,
        static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kNoError));
    RUVIA_CHECK((peerClose.flags & SSL_CONN_CLOSE_FLAG_LOCAL) == 0);
    RUVIA_CHECK(close.status == Http3QuicServerTransport::ConnectionCloseStatus::kCompleted);
    RUVIA_CHECK(close.allStreamsRetired);
    RUVIA_CHECK(server.retireConnectionLocally(*connectionId) ==
                Http3QuicStreamSet::Error::kNone);
#endif
}

RUVIA_TEST(http3QuicServerTransportRequestsWireCloseBeforeExplicitLocalRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    using Clock = std::chrono::steady_clock;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicDatagramBridge serverBridge(address());
    Http3QuicServerTransport server(serverTls, serverBridge, {.maxActiveConnections = 1});
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);

    auto local = address();
    local.port = 15433;
    DirectQuicPeer peer(clientTls, address(), local);
    std::optional<Http3QuicServerTransport::ConnectionId> connectionId;
    const auto handshakeDeadline = Clock::now() + std::chrono::seconds(8);
    std::size_t handshakeSteps{};
    while (Clock::now() < handshakeDeadline && handshakeSteps < 2000 &&
           (!connectionId || !peer.ready() ||
               !server.connectionInfo(*connectionId)->handshakeComplete)) {
        pumpDirectPair(server, serverBridge, peer, connectionId);
        ++handshakeSteps;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(connectionId.has_value());
    if (!connectionId) {
        throw std::runtime_error("direct QUIC test handshake deadline");
    }
    const auto connected = server.connectionInfo(*connectionId);
    RUVIA_CHECK(connected && connected->handshakeComplete && connected->h3Negotiated);
    RUVIA_CHECK(peer.ready());

    const auto stream = server.openLocalUnidirectionalStream(*connectionId);
    RUVIA_CHECK(stream.error == Http3QuicStreamSet::Error::kNone);
    std::vector<char> borrowedInput(4U * 1024U * 1024U, 'x');
    bool sawPendingWrite{};
    for (std::size_t attempt = 0; attempt < 64 && !sawPendingWrite; ++attempt) {
        const auto write = server.writeStream(*connectionId, stream.id, borrowedInput);
        if (write.status == Http3QuicStreamSet::StreamWrite::Status::kWouldBlock) {
            sawPendingWrite = true;
        } else if (write.status != Http3QuicStreamSet::StreamWrite::Status::kAccepted ||
                   write.bytes == 0) {
            break;
        }
    }
    RUVIA_CHECK(sawPendingWrite);
    std::size_t quietTicks{};
    for (std::size_t attempt = 0; attempt < 64 && quietTicks < 2; ++attempt) {
        const auto relayed = pumpDirectPair(server, serverBridge, peer, connectionId);
        if (relayed.first == 0 && relayed.second == 0) {
            ++quietTicks;
        } else {
            quietTicks = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK_EQ(quietTicks, std::size_t{2});

    constexpr auto closeCode = ruvia::Http3ConnectionErrorCode::kRequestRejected;
    const auto requested = server.requestConnectionClose(*connectionId, closeCode);
    RUVIA_CHECK(requested.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending ||
                requested.status == Http3QuicServerTransport::ConnectionCloseStatus::kCompleted);
    RUVIA_CHECK(requested.allStreamsRetired);
    const auto repeated = server.requestConnectionClose(*connectionId, closeCode);
    RUVIA_CHECK(repeated.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending ||
                repeated.status == Http3QuicServerTransport::ConnectionCloseStatus::kCompleted);
    RUVIA_CHECK(repeated.allStreamsRetired);
    const auto conflict = server.requestConnectionClose(
        *connectionId, ruvia::Http3ConnectionErrorCode::kGeneralProtocolError);
    RUVIA_CHECK(conflict.status == Http3QuicServerTransport::ConnectionCloseStatus::kConflict);
    RUVIA_CHECK(conflict.allStreamsRetired);

    RUVIA_CHECK(server.openLocalUnidirectionalStream(*connectionId).error ==
                Http3QuicStreamSet::Error::kClosed);
    RUVIA_CHECK(server.acceptStreams(*connectionId).error == Http3QuicStreamSet::Error::kClosed);
    RUVIA_CHECK(server.writeStream(*connectionId, stream.id, borrowedInput).status ==
                Http3QuicStreamSet::StreamWrite::Status::kClosed);
    RUVIA_CHECK(server.finishStream(*connectionId, stream.id) == Http3QuicStreamSet::Error::kClosed);
    RUVIA_CHECK(server.resetStream(*connectionId, stream.id, 0) == Http3QuicStreamSet::Error::kClosed);
    RUVIA_CHECK(server.closeStream(*connectionId, stream.id) == Http3QuicStreamSet::Error::kClosed);
    RUVIA_CHECK(server.terminateBidirectionalStream(*connectionId, stream.id, 0).close ==
                Http3QuicStreamSet::Error::kClosed);
    std::array<char, 8> readBuffer{};
    RUVIA_CHECK(server.readStream(*connectionId, stream.id, readBuffer).status ==
                Http3QuicStreamSet::StreamRead::Status::kClosed);
    RUVIA_CHECK(server.connectionInfo(*connectionId).has_value());

    // SSL_free(stream) has returned before the exact SSL_write WANT input storage is released.
    std::vector<char>{}.swap(borrowedInput);
    SSL_CONN_CLOSE_INFO peerClose{};
    const auto closeDeadline = Clock::now() + std::chrono::seconds(3);
    std::size_t closeSteps{};
    while (Clock::now() < closeDeadline && closeSteps < 2000 &&
           !peer.connectionCloseInfo(peerClose)) {
        (void)pumpDirectPair(server, serverBridge, peer, connectionId);
        ++closeSteps;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(peer.connectionCloseInfo(peerClose));
    RUVIA_CHECK_EQ(peerClose.error_code, static_cast<std::uint64_t>(closeCode));
    RUVIA_CHECK((peerClose.flags & SSL_CONN_CLOSE_FLAG_LOCAL) == 0);
    RUVIA_CHECK(server.connectionInfo(*connectionId).has_value());
    RUVIA_CHECK(server.retireConnectionLocally(*connectionId) == Http3QuicStreamSet::Error::kNone);
    RUVIA_CHECK(!server.connectionInfo(*connectionId));
    const auto afterRetirement = server.requestConnectionClose(*connectionId, closeCode);
    RUVIA_CHECK(afterRetirement.status ==
                Http3QuicServerTransport::ConnectionCloseStatus::kNoConnection);
    RUVIA_CHECK(!afterRetirement.allStreamsRetired);

    // The one-connection admission slot returns only after explicit local retirement.
    auto secondLocal = address();
    secondLocal.port = 15434;
    DirectQuicPeer secondPeer(clientTls, address(), secondLocal);
    std::optional<Http3QuicServerTransport::ConnectionId> secondConnectionId;
    const auto secondDeadline = Clock::now() + std::chrono::seconds(8);
    std::size_t secondSteps{};
    while (Clock::now() < secondDeadline && secondSteps < 2000 &&
           (!secondConnectionId || !secondPeer.ready() ||
               !server.connectionInfo(*secondConnectionId)->handshakeComplete)) {
        pumpDirectPair(server, serverBridge, secondPeer, secondConnectionId);
        ++secondSteps;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(secondConnectionId.has_value());
    if (!secondConnectionId) {
        throw std::runtime_error("second direct QUIC test handshake deadline");
    }
    RUVIA_CHECK(secondPeer.ready());
    if (secondConnectionId) {
        const auto secondConnected = server.connectionInfo(*secondConnectionId);
        RUVIA_CHECK(secondConnected && secondConnected->handshakeComplete);
        RUVIA_CHECK(server.retireConnectionLocally(*secondConnectionId) ==
                    Http3QuicStreamSet::Error::kNone);
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportLocallyRetiresWithFullBioWithoutPumping) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    using Clock = std::chrono::steady_clock;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext serverTls(config, std::pmr::get_default_resource());
    Http3QuicDatagramBridge serverBridge(address());
    Http3QuicServerTransport server(serverTls, serverBridge);
    Http3QuicClientTlsContext clientTls(ClientTransportConfigView{});
    SSL_CTX_set_verify(clientTls.nativeHandle(), SSL_VERIFY_NONE, nullptr);
    auto local = address();
    local.port = 15435;
    DirectQuicPeer peer(clientTls, address(), local);
    std::optional<Http3QuicServerTransport::ConnectionId> connectionId;
    const auto handshakeDeadline = Clock::now() + std::chrono::seconds(8);
    std::size_t handshakeSteps{};
    while (Clock::now() < handshakeDeadline && handshakeSteps < 2000 &&
           (!connectionId || !peer.ready() ||
               !server.connectionInfo(*connectionId)->handshakeComplete)) {
        pumpDirectPair(server, serverBridge, peer, connectionId);
        ++handshakeSteps;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(connectionId.has_value());
    if (!connectionId) {
        throw std::runtime_error("full-BIO direct QUIC test handshake deadline");
    }
    RUVIA_CHECK(peer.ready());

    constexpr std::array<std::byte, 1200> junk{};
    bool bioFull{};
    for (std::size_t datagram = 0; datagram < 256 && !bioFull; ++datagram) {
        const auto result = serverBridge.inject(junk, peer.local());
        bioFull = result == Http3QuicDatagramBridge::InjectResult::kFull;
        RUVIA_CHECK(result != Http3QuicDatagramBridge::InjectResult::kFatal);
    }
    RUVIA_CHECK(bioFull);
    const auto close = server.requestConnectionClose(
        *connectionId, ruvia::Http3ConnectionErrorCode::kInternalError);
    RUVIA_CHECK(close.allStreamsRetired);
    RUVIA_CHECK(close.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending ||
                close.status == Http3QuicServerTransport::ConnectionCloseStatus::kCompleted ||
                close.status == Http3QuicServerTransport::ConnectionCloseStatus::kFailure);
    const auto retireStart = Clock::now();
    RUVIA_CHECK(server.retireConnectionLocally(*connectionId) == Http3QuicStreamSet::Error::kNone);
    RUVIA_CHECK(Clock::now() - retireStart < std::chrono::seconds(1));
    RUVIA_CHECK(!server.connectionInfo(*connectionId));
#endif
}

RUVIA_TEST(http3QuicServerTransportZeroCapacityAndRepeatedCloseAreSafe) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext tls(config, std::pmr::get_default_resource());
    Http3QuicDatagramBridge bridge(address());
    Http3QuicServerTransport transport(tls, bridge, {.maxActiveConnections = 0});
    RUVIA_CHECK(transport.acceptConnections(1).empty());
    RUVIA_CHECK(transport.retireConnectionLocally(1) == Http3QuicServerTransport::Error::kNoConnection);
    RUVIA_CHECK(transport.retireConnectionLocally(1) == Http3QuicServerTransport::Error::kNoConnection);
    RUVIA_CHECK(transport.acceptStreams(1).error == Http3QuicServerTransport::Error::kNoConnection);
    std::array<char, 8> bytes{};
    RUVIA_CHECK(transport.readStream(1, 0, bytes).status == Http3QuicServerTransport::StreamRead::Status::kNoConnection);
    RUVIA_CHECK(transport.closeStream(1, 0) == Http3QuicServerTransport::Error::kNoConnection);
    RUVIA_CHECK(transport.openLocalUnidirectionalStream(1).error ==
                Http3QuicServerTransport::Error::kNoConnection);
    const auto noConnectionWrite = transport.writeStream(1, 0, std::span<const char>{});
    RUVIA_CHECK(noConnectionWrite.status == Http3QuicServerTransport::StreamWrite::Status::kNoConnection);
    RUVIA_CHECK(transport.finishStream(1, 0) == Http3QuicServerTransport::Error::kNoConnection);
    RUVIA_CHECK(transport.resetStream(1, 0, 0) == Http3QuicServerTransport::Error::kNoConnection);
    RUVIA_CHECK(!transport.connectionInfo(1));
#endif
}

RUVIA_TEST(http3QuicServerTransportRejectsInvalidConfigurationBeforeTakingBio) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext tls(config, std::pmr::get_default_resource());
    for (const auto timeout : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1),
             std::chrono::milliseconds::max()}) {
        Http3QuicDatagramBridge bridge(address());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicServerTransport invalid(tls, bridge, {.handshakeTimeout = timeout});
        }));
        // Failed configuration must not consume or destroy the bridge's BIO.
        Http3QuicServerTransport valid(tls, bridge);
        RUVIA_CHECK(valid.acceptConnections(1).empty());
    }
    for (const auto timeout : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1),
             std::chrono::milliseconds::max()}) {
        Http3QuicDatagramBridge bridge(address());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicServerTransport invalid(tls, bridge, {.idleTimeout = timeout});
        }));
        Http3QuicServerTransport valid(tls, bridge);
        RUVIA_CHECK(valid.acceptConnections(1).empty());
    }
    {
        Http3QuicDatagramBridge bridge(address());
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            Http3QuicServerTransport invalid(tls, bridge, {.maxLifetimePeerStreams = 0});
        }));
        Http3QuicServerTransport valid(tls, bridge);
        RUVIA_CHECK(valid.acceptConnections(1).empty());
    }
#endif
}

RUVIA_TEST(http3QuicServerTransportRejectsMissingBridgeBioWithoutLeakingSetup) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    using namespace ruvia::detail;
    IdentityFiles files;
    HttpServerListenerDefinition::Tls config;
    config.identity.certificateChainFile = files.certificate.string();
    config.identity.privateKeyFile = files.privateKey.string();
    Http3QuicTlsContext tls(config, std::pmr::get_default_resource());
    Http3QuicDatagramBridge bridge(address());
    BIO_free(bridge.releaseSslBio());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        Http3QuicServerTransport transport(tls, bridge);
    }));
#endif
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
