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
#include <variant>
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

#include "ruvia/http/http3_connection_error.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/quic_connection.h"
#include "ruvia/http/quic_server.h"

#include "http3/http3_critical_stream_driver.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_server_transport.h"
#include "http3/http3_quic_socket_address.h"
#include "http3/openssl_quic_crypto_provider.h"
#include "http3/openssl_quic_tls_session.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {

struct identity_files final {
    identity_files() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-http3-listener-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create temporary TLS directory");
        }
        EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (raw_context == nullptr) {
            throw std::runtime_error("failed to create key generator");
        }
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(raw_context,
            EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
            EVP_PKEY_generate(context.get(), &raw_key) <= 0) {
            throw std::runtime_error("failed to generate TLS key");
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new_ex(nullptr, nullptr), X509_free);
        if (!cert || X509_set_version(cert.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400) == nullptr ||
            X509_set_pubkey(cert.get(), key.get()) != 1 ||
            X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get())) != 1 ||
            ruvia::test::sign_tls_certificate(cert.get(), key.get()) <= 0) {
            throw std::runtime_error("failed to create self-signed test certificate");
        }
        certificate_ = directory_ / "cert.pem";
        private_key_ = directory_ / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(
            BIO_new_file(certificate_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
        if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key.get()) != 1) {
            throw std::runtime_error("failed to write test certificate");
        }
    }
    ~identity_files() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::filesystem::path directory_;
    std::filesystem::path certificate_;
    std::filesystem::path private_key_;

    [[nodiscard]] ruvia::detail::http_server_listener_definition::tls_type server_tls_config() const {
        ruvia::detail::http_server_listener_definition::tls_type config;
        config.identity_.certificate_chain_file_ = certificate_.string();
        config.identity_.private_key_file_ = private_key_.string();
        return config;
    }
};

using test_private_key_type = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using test_certificate_type = std::unique_ptr<X509, decltype(&X509_free)>;

test_private_key_type generate_test_key() {
    EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    if (raw_context == nullptr) {
        throw std::runtime_error("failed to create mTLS test key generator");
    }
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        raw_context, EVP_PKEY_CTX_free);
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
        EVP_PKEY_generate(context.get(), &raw_key) <= 0) {
        throw std::runtime_error("failed to generate mTLS test key");
    }
    return test_private_key_type(raw_key, EVP_PKEY_free);
}

void add_certificate_extension(X509* certificate, X509* issuer, int extension_id,
    const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx(&context, issuer == nullptr ? certificate : issuer, certificate,
        nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509V3_EXT_nconf_nid(nullptr, &context, extension_id, value),
        X509_EXTENSION_free);
    if (!extension || X509_add_ext(certificate, extension.get(), -1) != 1) {
        throw std::runtime_error("failed to add mTLS test certificate extension");
    }
}

test_certificate_type generate_test_certificate(EVP_PKEY* subject_key, const char* common_name,
    long serial, X509* issuer, EVP_PKEY* issuer_key, bool is_authority, bool is_server) {
    test_certificate_type certificate(X509_new_ex(nullptr, nullptr), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
        X509_set_pubkey(certificate.get(), subject_key) != 1) {
        throw std::runtime_error("failed to initialize mTLS test certificate");
    }

    const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
        X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
        X509_set_issuer_name(certificate.get(),
            issuer == nullptr ? subject.get() : X509_get_subject_name(issuer)) != 1) {
        throw std::runtime_error("failed to set mTLS test certificate names");
    }

    if (is_authority) {
        add_certificate_extension(certificate.get(), issuer, NID_basic_constraints,
            "critical,CA:TRUE,pathlen:0");
        add_certificate_extension(certificate.get(), issuer, NID_key_usage,
            "critical,keyCertSign,cRLSign");
    } else {
        add_certificate_extension(certificate.get(), issuer, NID_basic_constraints,
            "critical,CA:FALSE");
        add_certificate_extension(certificate.get(), issuer, NID_key_usage,
            is_server ? "critical,digitalSignature,keyEncipherment" : "critical,digitalSignature");
        add_certificate_extension(certificate.get(), issuer, NID_ext_key_usage,
            is_server ? "serverAuth" : "clientAuth");
        add_certificate_extension(certificate.get(), issuer, NID_subject_alt_name, "DNS:localhost");
    }

    EVP_PKEY* const signing_key = issuer_key == nullptr ? subject_key : issuer_key;
    if (ruvia::test::sign_tls_certificate(certificate.get(), signing_key) <= 0) {
        throw std::runtime_error("failed to sign mTLS test certificate");
    }
    return certificate;
}

void write_test_certificate(const std::filesystem::path& certificate_file, X509* certificate) {
    std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
        BIO_new_file(certificate_file.string().c_str(), "w"), BIO_free);
    if (!certificate_bio || PEM_write_bio_X509(certificate_bio.get(), certificate) != 1) {
        throw std::runtime_error("failed to write mTLS test certificate");
    }
}

void write_test_identity(const std::filesystem::path& certificate_file,
    const std::filesystem::path& private_key_file, X509* certificate, EVP_PKEY* private_key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
        BIO_new_file(certificate_file.string().c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
        BIO_new_file(private_key_file.string().c_str(), "w"), BIO_free);
    if (!certificate_bio || !key_bio || PEM_write_bio_X509(certificate_bio.get(), certificate) != 1 ||
        ruvia::test::write_tls_private_key(key_bio.get(), private_key) != 1) {
        throw std::runtime_error("failed to write mTLS test identity");
    }
}

struct mutual_tls_files final {
    explicit mutual_tls_files(const std::filesystem::path& directory) {
        trust_anchor_ = directory / "mtls-trust-anchor.pem";
        server_certificate_ = directory / "mtls-server-certificate.pem";
        server_private_key_ = directory / "mtls-server-private-key.pem";
        trusted_client_certificate_ = directory / "mtls-trusted-client-certificate.pem";
        trusted_client_private_key_ = directory / "mtls-trusted-client-private-key.pem";
        untrusted_client_certificate_ = directory / "mtls-untrusted-client-certificate.pem";
        untrusted_client_private_key_ = directory / "mtls-untrusted-client-private-key.pem";

        const test_private_key_type trusted_authority_key = generate_test_key();
        const test_certificate_type trusted_authority = generate_test_certificate(
            trusted_authority_key.get(), "ruvia-mtls-test-root", 10, nullptr, nullptr, true, false);
        const test_private_key_type server_key = generate_test_key();
        const test_certificate_type server = generate_test_certificate(server_key.get(), "localhost", 11,
            trusted_authority.get(), trusted_authority_key.get(), false, true);
        const test_private_key_type trusted_client_key = generate_test_key();
        const test_certificate_type trusted_client = generate_test_certificate(trusted_client_key.get(),
            "trusted-client", 12, trusted_authority.get(), trusted_authority_key.get(), false, false);
        const test_private_key_type untrusted_authority_key = generate_test_key();
        const test_certificate_type untrusted_authority = generate_test_certificate(
            untrusted_authority_key.get(), "ruvia-untrusted-mtls-root", 20, nullptr, nullptr, true,
            false);
        const test_private_key_type untrusted_client_key = generate_test_key();
        const test_certificate_type untrusted_client = generate_test_certificate(untrusted_client_key.get(),
            "untrusted-client", 21, untrusted_authority.get(), untrusted_authority_key.get(), false,
            false);

        write_test_certificate(trust_anchor_, trusted_authority.get());
        write_test_identity(server_certificate_, server_private_key_, server.get(), server_key.get());
        write_test_identity(trusted_client_certificate_, trusted_client_private_key_, trusted_client.get(),
            trusted_client_key.get());
        write_test_identity(untrusted_client_certificate_, untrusted_client_private_key_,
            untrusted_client.get(), untrusted_client_key.get());
    }

    std::filesystem::path trust_anchor_;
    std::filesystem::path server_certificate_;
    std::filesystem::path server_private_key_;
    std::filesystem::path trusted_client_certificate_;
    std::filesystem::path trusted_client_private_key_;
    std::filesystem::path untrusted_client_certificate_;
    std::filesystem::path untrusted_client_private_key_;
};

class counting_memory_resource final : public std::pmr::memory_resource {
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
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* const allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        outstanding_bytes_ += bytes_value;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        ++deallocations_;
        outstanding_bytes_ -= bytes_value;
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{};
    std::size_t deallocations_{};
    std::size_t outstanding_bytes_{};
};

class plain_udp_quic_server_fixture final {
public:
    using udp = asio::ip::udp;
    using clock = std::chrono::steady_clock;
    using client_transport = ruvia::detail::http3_quic_client_transport;
    using server_transport_type = ruvia::detail::http3_quic_server_transport;
    using socket_address = ruvia::detail::http3_quic_datagram_address;

    struct client_type final {
        client_type(udp::socket configured_socket, udp::endpoint configured_local,
            std::unique_ptr<client_transport> configured_transport)
            : socket_(std::move(configured_socket)),
              local_(std::move(configured_local)),
              transport_(std::move(configured_transport)) {}

        udp::socket socket_;
        udp::endpoint local_;
        std::unique_ptr<client_transport> transport_;
    };

    struct admitted_initial_type final {
        ruvia::quic_connection_token token_;
        ruvia::quic_initial_offer offer_;
    };

    plain_udp_quic_server_fixture(const ruvia::detail::http_server_listener_definition::tls_type& tls_config_value,
        ruvia::quic_server_config server_config = {},
        ruvia::detail::client_transport_config_view client_config = {
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : server_socket_(io_),
          server_tls_(tls_config_value, resource),
          client_tls_(client_config, resource),
          server_(server_tls_, server_config, resource),
          resource_(resource != nullptr ? resource : std::pmr::get_default_resource()) {
        server_socket_.open(udp::v4());
        server_socket_.bind({asio::ip::address_v4::loopback(), 0});
        server_socket_.non_blocking(true);
        server_endpoint_ = server_socket_.local_endpoint();
        server_address_ = to_address(server_endpoint_);
    }

    plain_udp_quic_server_fixture(const plain_udp_quic_server_fixture&) = delete;
    plain_udp_quic_server_fixture& operator=(const plain_udp_quic_server_fixture&) = delete;

    [[nodiscard]] client_type& add_client(std::string_view host = "localhost",
        ruvia::quic_transport_parameters parameters = {}, ruvia::quic_limits limits = {}) {
        udp::socket socket(io_);
        socket.open(udp::v4());
        socket.bind({asio::ip::address_v4::loopback(), 0});
        socket.non_blocking(true);
        const auto local = socket.local_endpoint();
        const auto local_address = to_address(local);
        ruvia::quic_connection_config config;
        config.local_address_ = to_quic_address(local_address);
        config.peer_address_ = to_quic_address(server_address_);
        config.local_transport_parameters_ = parameters;
        config.limits_ = limits;
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
            if (const auto expiry = client.transport_->next_expiry(); expiry && *expiry <= now) {
                (void)client.transport_->handle_expiry(now);
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
            if (last.status_ == ruvia::quic_operation_status::accepted) {
                admissions_.push_back({last.connection_, *iterator});
                iterator = pending_offers_.erase(iterator);
                ++admitted;
            } else if (last.status_ == ruvia::quic_operation_status::would_block) {
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
            (void)client.transport_->handle_expiry(now);
        }
    }

    template <typename predicate_type>
    [[nodiscard]] bool run_until(predicate_type&& predicate,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(8),
        bool admit_initials = true) {
        const auto deadline_value = clock::now() + timeout;
        while (!predicate() && clock::now() < deadline_value) {
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
        bool server_initiates, const client_type& client) {
        const auto token = admissions_.front().token_;
        return server_initiates ? client.transport_->connection().info().state_
                                : server_.server().connection(token).info().state_;
    }
    [[nodiscard]] client_type& client(std::size_t index) {
        return clients_.at(index);
    }
    void reset_packet_observation() noexcept {
        last_client_packets_ = 0;
    }
    [[nodiscard]] std::size_t last_client_packets() const noexcept {
        return last_client_packets_;
    }
    [[nodiscard]] const std::vector<admitted_initial_type>& admissions() const noexcept {
        return admissions_;
    }
    [[nodiscard]] ruvia::quic_operation_status retire(ruvia::quic_connection_token token) {
        const auto result_value = server_.server().retire(token);
        if (result_value == ruvia::quic_operation_status::accepted ||
            result_value == ruvia::quic_operation_status::retired) {
            std::erase_if(admissions_, [token](const admitted_initial_type& admission) {
                return admission.token_ == token;
            });
        }
        return result_value;
    }
    [[nodiscard]] const std::vector<ruvia::quic_initial_offer>& pending_offers() const noexcept {
        return pending_offers_;
    }
    [[nodiscard]] std::optional<ruvia::quic_connection_token> token_for_offer(
        std::uint64_t offer_id) const noexcept {
        const auto found = std::find_if(admissions_.begin(), admissions_.end(),
            [offer_id](const admitted_initial_type& admission) {
                return admission.offer_.offer_id_ == offer_id;
            });
        return found == admissions_.end()
                   ? std::nullopt
                   : std::optional<ruvia::quic_connection_token>(found->token_);
    }
    [[nodiscard]] const udp::endpoint& server_endpoint() const noexcept {
        return server_endpoint_;
    }
    [[nodiscard]] std::optional<ruvia::quic_timestamp> next_expiry() const noexcept {
        auto result_value = server_.server().next_expiry();
        for (const auto& client : clients_) {
            const auto expiry = client.transport_->next_expiry();
            if (expiry && (!result_value || *expiry < *result_value)) {
                result_value = expiry;
            }
        }
        return result_value;
    }
    [[nodiscard]] const socket_address& server_address() const noexcept {
        return server_address_;
    }

private:
    [[nodiscard]] static socket_address to_address(const udp::endpoint& endpoint) {
        const auto address = ruvia::detail::to_http3_quic_datagram_address(endpoint);
        if ((address.index() != 0)) {
            throw std::runtime_error("invalid plain UDP QUIC fixture endpoint");
        }
        return std::get<0>(address);
    }

    std::size_t write_client_packets(client_type& client, ruvia::quic_timestamp now) {
        std::size_t packets_written{};
        for (unsigned count = 0; count < 32; ++count) {
            const auto packet = client.transport_->write_packet(packet_buffer_, now);
            if (packet.size_ == 0) {
                return packets_written;
            }
            asio::error_code error;
            const auto sent = client.socket_.send_to(
                asio::buffer(packet_buffer_.data(), packet.size_), server_endpoint_, 0, error);
            if (error || sent != packet.size_) {
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
            const auto peer_address = to_address(peer);
            const auto route = server_.route_datagram(
                std::span<const std::byte>(packet_buffer_).first(size), server_address_, peer_address);
            if (route.kind_ == ruvia::quic_server_route_kind::initial_offer) {
                const auto found = std::find_if(pending_offers_.begin(), pending_offers_.end(),
                    [&route](const ruvia::quic_initial_offer& offer) {
                        return offer.offer_id_ == route.offer_.offer_id_;
                    });
                if (found == pending_offers_.end()) {
                    pending_offers_.push_back(route.offer_);
                }
                if (admit_initials) {
                    (void)admit_pending(pending_offers_.size(), now);
                }
            } else if (route.kind_ == ruvia::quic_server_route_kind::existing_connection) {
                const ruvia::quic_datagram_view datagram{
                    std::span<const std::byte>(packet_buffer_).first(size),
                    to_quic_address(server_address_), to_quic_address(peer_address)};
                (void)server_.server().receive(route.connection_, datagram, now);
            }
        }
    }

    void write_server_packets(ruvia::quic_timestamp now) {
        for (const auto& admission : admissions_) {
            auto& connection = server_.server().connection(admission.token_);
            for (unsigned count = 0; count < 32; ++count) {
                const auto packet = connection.write_packet(packet_buffer_, now);
                if (packet.size_ == 0) {
                    break;
                }
                const auto destination = ruvia::detail::to_udp_endpoint(
                    ruvia::detail::from_quic_address(packet.peer_));
                if ((destination.index() != 0)) {
                    throw std::runtime_error("QUIC server output has invalid peer address");
                }
                asio::error_code error;
                const auto sent = server_socket_.send_to(
                    asio::buffer(packet_buffer_.data(), packet.size_), std::get<0>(destination), 0, error);
                if (error || sent != packet.size_) {
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
                const auto size = client.socket_.receive_from(asio::buffer(packet_buffer_), peer, 0, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    throw std::system_error(error, "receive QUIC client UDP packet");
                }
                const auto remote = to_address(peer);
                const ruvia::quic_datagram_view datagram{
                    std::span<const std::byte>(packet_buffer_).first(size),
                    to_quic_address(to_address(client.local_)), to_quic_address(remote)};
                (void)client.transport_->receive(datagram, now);
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
    std::vector<client_type> clients_;
    std::vector<ruvia::quic_initial_offer> pending_offers_;
    std::vector<admitted_initial_type> admissions_;
    std::array<std::byte, 65536> packet_buffer_{};
    std::size_t last_client_packets_{};
};

struct client_certificate_observation final {
    std::size_t callback_count_{};
    bool saw_leaf_{};
    bool leaf_verified_{};
    bool all_verified_{true};
    bool saw_verification_failure_{};
    int leaf_name_length_{};
    std::array<char, 128> leaf_common_name_{};
};

int client_certificate_observation_index() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int observe_client_certificate(int preverify_ok, X509_STORE_CTX* store_value) noexcept {
    SSL* const ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(store_value, SSL_get_ex_data_X509_STORE_CTX_idx()));
    SSL_CTX* const context_value = ssl == nullptr ? nullptr : SSL_get_SSL_CTX(ssl);
    const int index = client_certificate_observation_index();
    auto* const observation_value = context_value == nullptr || index < 0
                                        ? nullptr
                                        : static_cast<client_certificate_observation*>(
                                              SSL_CTX_get_ex_data(context_value, index));
    if (observation_value == nullptr) {
        return preverify_ok;
    }
    ++observation_value->callback_count_;
    if (preverify_ok != 1) {
        observation_value->all_verified_ = false;
        observation_value->saw_verification_failure_ = true;
    }
    if (X509_STORE_CTX_get_error_depth(store_value) == 0) {
        observation_value->saw_leaf_ = true;
        observation_value->leaf_verified_ = preverify_ok == 1;
        X509* const certificate = X509_STORE_CTX_get_current_cert(store_value);
        if (certificate != nullptr) {
            const auto* subject = X509_get_subject_name(certificate);
            const int name_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
            if (name_index >= 0) {
                const auto* name = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, name_index));
                const int length = ASN1_STRING_length(name);
                if (length > 0 && length < static_cast<int>(observation_value->leaf_common_name_.size())) {
                    std::memcpy(observation_value->leaf_common_name_.data(), ASN1_STRING_get0_data(name),
                        static_cast<std::size_t>(length));
                    observation_value->leaf_name_length_ = length;
                }
            }
        }
    }
    return preverify_ok;
}

class plain_udp_tls_client_peer final {
public:
    using udp = asio::ip::udp;
    using clock = std::chrono::steady_clock;

    plain_udp_tls_client_peer(ruvia::detail::client_transport_config_view tls_config_value,
        std::string_view host, const udp::endpoint& remote, std::uint16_t local_port,
        std::pmr::memory_resource* resource)
        : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
          tls_context_(tls_config_value, resource_),
          socket_(io_),
          crypto_(resource_),
          remote_(remote) {
        socket_.open(udp::v4());
        socket_.bind({asio::ip::address_v4::loopback(), local_port});
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        const auto local_address = ruvia::detail::to_http3_quic_datagram_address(local_);
        const auto peer_address = ruvia::detail::to_http3_quic_datagram_address(remote_);
        if ((local_address.index() != 0) || (peer_address.index() != 0)) {
            throw std::runtime_error("invalid plain UDP TLS peer endpoint");
        }

        constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
        tls_session_ = std::make_unique<ruvia::detail::openssl_quic_tls_session>(
            tls_context_.native_handle(), ruvia::quic_role::client, h3_alpn, host, resource_);
        tls_context_.prepare(tls_session_->native_handle(), host);

        ruvia::quic_connection_config config;
        config.role_ = ruvia::quic_role::client;
        config.local_address_ = ruvia::detail::to_quic_address(std::get<0>(local_address));
        config.peer_address_ = ruvia::detail::to_quic_address(std::get<0>(peer_address));
        std::array<std::byte, 8> destination_id{};
        std::array<std::byte, 8> source_id{};
        const auto provider = crypto_.view();
        provider.random_bytes_(provider.context_, destination_id);
        provider.random_bytes_(provider.context_, source_id);
        config.destination_connection_id_ = ruvia::quic_connection_id(destination_id);
        config.source_connection_id_ = ruvia::quic_connection_id(source_id);
        connection_ = std::make_unique<ruvia::quic_connection>(config, provider,
            tls_session_->driver_view(), resource_, clock::now());
    }

    ~plain_udp_tls_client_peer() {
        if (tls_session_) {
            tls_session_->stop();
        }
        connection_.reset();
        tls_session_.reset();
    }
    plain_udp_tls_client_peer(const plain_udp_tls_client_peer&) = delete;
    plain_udp_tls_client_peer& operator=(const plain_udp_tls_client_peer&) = delete;

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
            if (packet.size_ == 0) {
                return;
            }
            asio::error_code error;
            const auto sent = socket_.send_to(asio::buffer(packet_.data(), packet.size_), remote_, 0, error);
            if (error || sent != packet.size_) {
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
            if ((local_address.index() != 0) || (peer_address.index() != 0)) {
                throw std::runtime_error("invalid received standalone QUIC peer endpoint");
            }
            const ruvia::quic_datagram_view datagram{
                std::span<const std::byte>(packet_).first(size),
                ruvia::detail::to_quic_address(std::get<0>(local_address)),
                ruvia::detail::to_quic_address(std::get<0>(peer_address))};
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

struct mutual_tls_handshake final {
    bool accepted_by_server_{};
    bool client_ready_{};
    bool client_saw_remote_close_{};
    bool client_verifies_localhost_{};
    bool server_handshake_complete_{};
    bool server_h3_negotiated_{};
    bool crypto_failure_observed_{};
    std::string_view crypto_failure_stage_;
    std::uint8_t server_tls_failure_alert_{};
    long client_verify_result_{X509_V_ERR_UNSPECIFIED};
    client_certificate_observation client_certificate_;
};

mutual_tls_handshake perform_required_mutual_tls_handshake(const mutual_tls_files& files,
    std::uint16_t local_port, const std::filesystem::path* client_certificate,
    const std::filesystem::path* client_private_key) {
    counting_memory_resource memory;
    mutual_tls_handshake result;
    {
        ruvia::detail::http_server_listener_definition::tls_type server_config;
        server_config.identity_.certificate_chain_file_ = files.server_certificate_.string();
        server_config.identity_.private_key_file_ = files.server_private_key_.string();
        ruvia::detail::http_server_listener_definition::tls_client_certificate_policy_type client_policy;
        client_policy.verify_file_ = files.trust_anchor_.string();
        client_policy.requirement_ = ruvia::tls_client_certificate_requirement::required;
        server_config.client_certificates_ = client_policy;
        plain_udp_quic_server_fixture fixture_value(server_config, {}, {}, &memory);

        const int observation_index = client_certificate_observation_index();
        SSL_CTX* const server_context = fixture_value.server_tls_context().default_context();
        if (observation_index < 0 || server_context == nullptr ||
            SSL_CTX_set_ex_data(server_context, observation_index, &result.client_certificate_) != 1) {
            throw std::runtime_error("failed to attach server client-certificate observer");
        }
        SSL_CTX_set_verify(server_context, SSL_CTX_get_verify_mode(server_context),
            &observe_client_certificate);

        const std::string client_certificate_file = client_certificate == nullptr
                                                        ? std::string{}
                                                        : client_certificate->string();
        const std::string client_private_key_file = client_private_key == nullptr
                                                        ? std::string{}
                                                        : client_private_key->string();
        const auto trust_anchor_file = files.trust_anchor_.string();
        const ruvia::detail::client_transport_config_view client_config{
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::verify,
            .ca_file_ = trust_anchor_file,
            .certificate_chain_file_ = client_certificate_file,
            .private_key_file_ = client_private_key_file};
        plain_udp_tls_client_peer peer(client_config, "localhost", fixture_value.server_endpoint(),
            local_port, &memory);

        const bool expects_valid_client_certificate =
            client_certificate == &files.trusted_client_certificate_;
        auto drive_stage = [&](std::string_view phase, auto&& operation) -> bool {
            try {
                operation();
                return true;
            } catch (const ruvia::quic_error& error) {
                if (error.code() == ruvia::quic_error_code::crypto_failure &&
                    !expects_valid_client_certificate && !fixture_value.admissions().empty()) {
                    auto& server = fixture_value.protocol_server().connection(
                        fixture_value.admissions().front().token_);
                    if (server.tls_handshake().failed()) {
                        result.crypto_failure_observed_ = true;
                        result.crypto_failure_stage_ = phase;
                        result.server_tls_failure_alert_ = static_cast<std::uint8_t>(
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

        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline_value) {
            if (!drive_stage("client packet generation/transmit", [&] { peer.send_packets(); })) {
                break;
            }
            if (!drive_stage("server packet processing", [&] {
                    fixture_value.pump(false);
                    if (fixture_value.admissions().empty() && !fixture_value.pending_offers().empty()) {
                        (void)fixture_value.admit_pending(1);
                    }
                })) {
                break;
            }
            if (!drive_stage("client packet receive/processing", [&] { peer.receive_packets(); })) {
                break;
            }

            const auto client_info = peer.connection().info();
            const bool server_ready = !fixture_value.admissions().empty() &&
                                      fixture_value.protocol_server().connection(fixture_value.admissions().front().token_).info().quic_handshake_complete_;
            if ((client_info.quic_handshake_complete_ && server_ready) || peer.saw_remote_close() ||
                client_info.state_ == ruvia::quic_connection_state::failed ||
                client_info.state_ == ruvia::quic_connection_state::draining) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (result.crypto_failure_observed_ && !fixture_value.admissions().empty()) {
            auto& server = fixture_value.protocol_server().connection(fixture_value.admissions().front().token_);
            (void)drive_stage("server TLS-failure close submission", [&] {
                (void)server.close({.kind_ = ruvia::quic_close_kind::transport,
                    .code_ = 0x100U + result.server_tls_failure_alert_});
            });
            (void)drive_stage("server TLS-failure close transmission", [&] { fixture_value.pump(false); });
            (void)drive_stage("client TLS-failure close reception", [&] { peer.receive_packets(); });
        }

        result.accepted_by_server_ = !fixture_value.admissions().empty();
        result.client_ready_ = peer.connection().info().quic_handshake_complete_;
        result.client_saw_remote_close_ = peer.saw_remote_close();
        result.client_verifies_localhost_ = peer.verifies_host("localhost");
        result.client_verify_result_ = SSL_get_verify_result(peer.native_tls_handle());
        if (result.accepted_by_server_) {
            const auto token = fixture_value.admissions().front().token_;
            auto& server_connection = fixture_value.protocol_server().connection(token);
            const auto info = server_connection.info();
            result.server_handshake_complete_ = info.quic_handshake_complete_ && info.tls_handshake_complete_;
            const auto alpn = server_connection.tls_handshake().info().negotiated_alpn_;
            result.server_h3_negotiated_ = alpn.size() == 2 &&
                                           std::to_integer<char>(alpn[0]) == 'h' && std::to_integer<char>(alpn[1]) == '3';
        }
    }
    if (memory.allocations() != memory.deallocations() || memory.outstanding_bytes() != 0) {
        throw std::runtime_error("mTLS QUIC fixture leaked PMR allocations");
    }
    return result;
}

}  // namespace

RUVIA_TEST(http3_quic_server_transport_constructs_and_releases_repeatedly) {
    identity_files files;
    auto config = files.server_tls_config();
    for (int iteration = 0; iteration != 2; ++iteration) {
        plain_udp_quic_server_fixture fixture(config);
        RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
        RUVIA_CHECK_EQ(fixture.protocol_server().pending_connection_count(), std::size_t{0});
        RUVIA_CHECK(!fixture.next_expiry());
    }
}
RUVIA_TEST(http3_quic_server_transport_bounds_pending_initials_before_accept) {
    identity_files files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections_ = 2;
    server_config.max_pending_connections_ = 3;
    plain_udp_quic_server_fixture fixture(config, server_config);
    for (std::size_t index = 0; index != 5; ++index) {
        (void)fixture.add_client();
    }

    fixture.pump(false);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{3});
    RUVIA_CHECK_EQ(fixture.protocol_server().pending_connection_count(), std::size_t{3});
    RUVIA_CHECK(fixture.admissions().empty());
    const auto admitted = fixture.admit_pending(3);
    RUVIA_CHECK_EQ(admitted.status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{2});

    const auto first_token = fixture.admissions().front().token_;
    RUVIA_CHECK_EQ(fixture.retire(first_token), ruvia::quic_operation_status::retired);
    const auto resumed = fixture.admit_pending(1);
    RUVIA_CHECK_EQ(resumed.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK(fixture.pending_offers().empty());
}
RUVIA_TEST(http3_quic_server_transport_keeps_interleaved_initial_peer_paths_paired) {
    identity_files files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_pending_connections_ = 4;
    plain_udp_quic_server_fixture fixture(config, server_config);
    (void)fixture.add_client();
    (void)fixture.add_client();

    fixture.pump(false);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});
    const auto first_offer = fixture.pending_offers()[0];
    const auto second_offer = fixture.pending_offers()[1];
    RUVIA_CHECK(first_offer.offer_id_ != second_offer.offer_id_);
    RUVIA_CHECK(first_offer.peer_address_.port_ != second_offer.peer_address_.port_);
    RUVIA_CHECK_EQ(fixture.admit_pending(2).status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(fixture.run_until([&] {
        const auto first_token = fixture.token_for_offer(first_offer.offer_id_);
        const auto second_token = fixture.token_for_offer(second_offer.offer_id_);
        return first_token && second_token &&
               fixture.protocol_server().connection(*first_token).info().quic_handshake_complete_ &&
               fixture.protocol_server().connection(*second_token).info().quic_handshake_complete_ &&
               fixture.client(0).transport_->connection().info().quic_handshake_complete_ &&
               fixture.client(1).transport_->connection().info().quic_handshake_complete_;
    }));

    const auto first_token = fixture.token_for_offer(first_offer.offer_id_);
    const auto second_token = fixture.token_for_offer(second_offer.offer_id_);
    RUVIA_CHECK(first_token.has_value() && second_token.has_value());
    if (first_token && second_token) {
        RUVIA_CHECK(*first_token != *second_token);
        const auto first_info = fixture.protocol_server().connection(*first_token).info();
        const auto second_info = fixture.protocol_server().connection(*second_token).info();
        RUVIA_CHECK_EQ(first_info.peer_address_.port_, first_offer.peer_address_.port_);
        RUVIA_CHECK_EQ(second_info.peer_address_.port_, second_offer.peer_address_.port_);
        const auto first_alpn = fixture.protocol_server().connection(*first_token).tls_handshake().info().negotiated_alpn_;
        const auto second_alpn = fixture.protocol_server().connection(*second_token).tls_handshake().info().negotiated_alpn_;
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
RUVIA_TEST(http3_quic_server_transport_reports_negotiated_idle_expiry_without_active_streams) {
    identity_files files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.local_transport_parameters_.idle_timeout_ms_ = 120;
    plain_udp_quic_server_fixture fixture(config, server_config);
    ruvia::quic_transport_parameters client_parameters;
    client_parameters.idle_timeout_ms_ = 120;
    (void)fixture.add_client("localhost", client_parameters);
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token_).info().quic_handshake_complete_;
    }));

    const auto token = fixture.admissions().front().token_;
    auto& connection = fixture.protocol_server().connection(token);
    RUVIA_CHECK_EQ(connection.info().negotiated_idle_timeout_ms_, std::uint64_t{120});
    const auto expiry = connection.next_expiry();
    RUVIA_CHECK(expiry.has_value());
    RUVIA_CHECK(fixture.run_until([&] {
        const auto state_value = connection.info().state_;
        return state_value == ruvia::quic_connection_state::closing ||
               state_value == ruvia::quic_connection_state::draining ||
               state_value == ruvia::quic_connection_state::retired;
    },
        std::chrono::seconds(2)));
}
RUVIA_TEST(http3_quic_server_transport_inherits_listener_idle_timeout_for_deferred_pending_handshakes) {
    const auto verify_negotiated_timeout = [&ruvia_ctx](std::uint64_t server_timeout,
                                               std::uint64_t client_timeout,
                                               std::uint64_t expected_timeout) {
        identity_files files;
        auto config = files.server_tls_config();
        ruvia::quic_server_config server_config;
        server_config.local_transport_parameters_.idle_timeout_ms_ = server_timeout;
        plain_udp_quic_server_fixture fixture(config, server_config);
        ruvia::quic_transport_parameters client_parameters;
        client_parameters.idle_timeout_ms_ = client_timeout;
        (void)fixture.add_client("localhost", client_parameters);

        fixture.pump(false);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK(fixture.admissions().empty());
        RUVIA_CHECK_EQ(fixture.admit_pending(0).status_, ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture.admit_pending(1).status_, ruvia::quic_operation_status::accepted);
        RUVIA_CHECK(fixture.run_until([&] {
            return fixture.protocol_server().connection(fixture.admissions().front().token_).info().quic_handshake_complete_;
        }));
        RUVIA_CHECK_EQ(fixture.protocol_server().connection(fixture.admissions().front().token_).info().negotiated_idle_timeout_ms_,
            expected_timeout);
    };

    verify_negotiated_timeout(20'000, 90'000, 20'000);
    verify_negotiated_timeout(75'000, 120'000, 75'000);
    verify_negotiated_timeout(0, 0, 0);
}
RUVIA_TEST(http3_quic_server_transport_requires_per_call_accept_credits) {
    identity_files files;
    auto config = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections_ = 2;
    server_config.max_pending_connections_ = 4;
    plain_udp_quic_server_fixture fixture(config, server_config);
    for (std::size_t index = 0; index != 4; ++index) {
        (void)fixture.add_client();
    }
    fixture.pump(false);

    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{4});
    RUVIA_CHECK(fixture.admissions().empty());
    RUVIA_CHECK_EQ(fixture.admit_pending(0).status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(fixture.admissions().empty());
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{4});

    RUVIA_CHECK_EQ(fixture.admit_pending(2).status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{2});

    const auto first = fixture.admissions().front().token_;
    RUVIA_CHECK_EQ(fixture.retire(first), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(fixture.admissions().size(), std::size_t{2});
    RUVIA_CHECK_EQ(fixture.pending_offers().size(), std::size_t{1});
}
RUVIA_TEST(http3_quic_transport_bounds_lifetime_peer_admissions_despite_repeated_close) {
    constexpr std::size_t lifetime_limit = 8;
    const auto exercise_direction = [&ruvia_ctx](bool server_initiates) {
        identity_files files;
        auto config = files.server_tls_config();
        ruvia::quic_server_config server_config;
        server_config.limits_.max_lifetime_peer_streams_ = lifetime_limit;
        server_config.local_transport_parameters_.initial_max_streams_bidi_ = 0;
        server_config.local_transport_parameters_.initial_max_streams_uni_ = lifetime_limit;
        counting_memory_resource memory;
        {
            const ruvia::detail::client_transport_config_view client_tls_config{
                .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
            plain_udp_quic_server_fixture fixture_value(config, server_config, client_tls_config, &memory);
            ruvia::quic_transport_parameters client_parameters;
            client_parameters.initial_max_streams_bidi_ = 0;
            client_parameters.initial_max_streams_uni_ = lifetime_limit;
            ruvia::quic_limits client_limits;
            client_limits.max_lifetime_peer_streams_ = lifetime_limit;
            auto& client = fixture_value.add_client("localhost", client_parameters, client_limits);
            RUVIA_CHECK(fixture_value.run_until([&] {
                return !fixture_value.admissions().empty() &&
                       fixture_value.protocol_server().connection(fixture_value.admissions().front().token_).info().quic_handshake_complete_ &&
                       client.transport_->connection().info().quic_handshake_complete_;
            }));

            auto& server = fixture_value.protocol_server().connection(fixture_value.admissions().front().token_);
            std::size_t admitted{};
            for (std::size_t index = 0; index < lifetime_limit; ++index) {
                auto& sender = server_initiates ? server : client.transport_->connection();
                auto& receiver = server_initiates ? client.transport_->connection() : server;
                const auto opened = sender.open_stream(true);
                RUVIA_CHECK_EQ(opened.status_, ruvia::quic_operation_status::accepted);
                constexpr std::array payload_value{std::byte{0x21}};
                const auto write = sender.write_stream(opened.stream_id_, payload_value, true);
                RUVIA_CHECK_EQ(write.status_, ruvia::quic_operation_status::accepted);
                RUVIA_CHECK_EQ(write.accepted_, payload_value.size());
                bool stream_seen = false;
                const auto stream_ready = fixture_value.run_until([&] {
                    auto accepted = receiver.accept_streams();
                    stream_seen |= std::ranges::any_of(
                        std::span(accepted.streams_).first(accepted.size_),
                        [&opened](const ruvia::quic_stream_metadata& item) {
                            return item.stream_id_ == opened.stream_id_;
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
                RUVIA_CHECK(fixture_value.run_until([&] {
                    const auto read = receiver.read_stream(opened.stream_id_, output);
                    if (read.status_ == ruvia::quic_stream_read_status::data) {
                        bytes_read += read.size_;
                    } else if (read.status_ == ruvia::quic_stream_read_status::fin) {
                        fin = true;
                    }
                    return fin;
                }));
                RUVIA_CHECK(fin);
                RUVIA_CHECK_EQ(bytes_read, payload_value.size());
                ++admitted;
                const auto sender_close = sender.close_stream(opened.stream_id_);
                const auto receiver_close = receiver.close_stream(opened.stream_id_);
                RUVIA_CHECK(sender_close == ruvia::quic_operation_status::accepted ||
                            sender_close == ruvia::quic_operation_status::completed);
                RUVIA_CHECK(receiver_close == ruvia::quic_operation_status::accepted ||
                            receiver_close == ruvia::quic_operation_status::completed);
            }
            RUVIA_CHECK_EQ(admitted, lifetime_limit);

            auto& sender = server_initiates ? server : client.transport_->connection();
            const auto beyond_limit = sender.open_stream(true);
            RUVIA_CHECK_EQ(beyond_limit.status_, ruvia::quic_operation_status::accepted);
            constexpr std::array payload_value{std::byte{0x21}};
            const auto write = sender.write_stream(beyond_limit.stream_id_, payload_value, true);
            RUVIA_CHECK_EQ(write.status_, ruvia::quic_operation_status::accepted);
            bool rejected = false;
            try {
                (void)fixture_value.run_until([&] {
                    return fixture_value.connection_state(server_initiates, client) ==
                           ruvia::quic_connection_state::failed;
                });
            } catch (const ruvia::quic_error& error) {
                rejected = error.code() == ruvia::quic_error_code::resource_limit;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(fixture_value.connection_state(server_initiates, client),
                ruvia::quic_connection_state::failed);
        }
        RUVIA_CHECK_EQ(memory.allocations(), memory.deallocations());
        RUVIA_CHECK_EQ(memory.outstanding_bytes(), std::size_t{0});
    };
    exercise_direction(false);
    exercise_direction(true);
}
RUVIA_TEST(http3_quic_server_transport_gracefully_flushes_an_open_stream_before_no_error_close) {
    using namespace ruvia::detail;
    identity_files files;
    auto tls_config_value = files.server_tls_config();
    plain_udp_quic_server_fixture fixture(tls_config_value);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token_).info().quic_handshake_complete_ &&
               fixture.client(0).transport_->connection().info().quic_handshake_complete_;
    }));
    const auto token = fixture.admissions().front().token_;
    auto& server = fixture.protocol_server().connection(token);
    auto& client = fixture.client(0).transport_->connection();

    const auto prefixes = ruvia::http3_local_critical_streams::create();
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    http3_critical_stream_driver critical(std::get<0>(prefixes));
    const auto drive_critical = [&] {
        return critical.drive(
            [&](http3_critical_stream_driver::kind_type) { return server.open_stream(true); },
            [&](std::uint64_t stream_id, std::span<const char> bytes_value) {
                return server.write_stream(stream_id, std::as_bytes(bytes_value));
            });
    };
    RUVIA_CHECK(fixture.run_until([&] {
        const auto result_value = drive_critical();
        if (result_value == http3_critical_stream_driver::result_type::fatal) {
            throw std::runtime_error("QUIC critical stream setup failed");
        }
        return critical.complete();
    }));
    RUVIA_CHECK(critical.queue_goaway(0));
    RUVIA_CHECK(fixture.run_until([&] {
        const auto result_value = drive_critical();
        if (result_value == http3_critical_stream_driver::result_type::fatal) {
            throw std::runtime_error("QUIC GOAWAY write failed");
        }
        return critical.complete();
    }));

    constexpr std::array kinds{http3_critical_stream_driver::kind_type::control,
        http3_critical_stream_driver::kind_type::qpack_encoder,
        http3_critical_stream_driver::kind_type::qpack_decoder};
    std::array<std::uint64_t, 4> stream_ids{};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        const auto stream_id = critical.stream_id(kinds[index]);
        RUVIA_CHECK(stream_id.has_value());
        if (!stream_id) {
            return;
        }
        stream_ids[index] = *stream_id;
    }
    const auto application_value = server.open_stream(true);
    RUVIA_CHECK_EQ(application_value.status_, ruvia::quic_operation_status::accepted);
    stream_ids.back() = application_value.stream_id_;

    ruvia::http3_critical_stream_output expected_output(std::get<0>(prefixes));
    RUVIA_CHECK(expected_output.queue_goaway(0));
    std::array<std::string, 4> expected{
        std::string{},
        std::string(std::get<0>(prefixes).qpack_encoder_prefix().begin(), std::get<0>(prefixes).qpack_encoder_prefix().end()),
        std::string(std::get<0>(prefixes).qpack_decoder_prefix().begin(), std::get<0>(prefixes).qpack_decoder_prefix().end()),
        "graceful close flushes this stream"};
    auto goaway = expected_output.next(ruvia::http3_critical_stream_output::stream_kind::control);
    expected[0].append(goaway.data(), goaway.size());
    RUVIA_CHECK(expected_output.acknowledge(ruvia::http3_critical_stream_output::stream_kind::control,
        goaway.size()));
    goaway = expected_output.next(ruvia::http3_critical_stream_output::stream_kind::control);
    expected[0].append(goaway.data(), goaway.size());

    const auto payload_value = std::as_bytes(std::span(expected.back().data(), expected.back().size()));
    const auto write = server.write_stream(application_value.stream_id_, payload_value, true);
    RUVIA_CHECK_EQ(write.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(write.accepted_, payload_value.size());

    std::array<bool, 4> accepted_streams{};
    std::array<std::string, 4> received_value{};
    bool application_fin{};
    std::array<std::byte, 128> read_buffer{};
    RUVIA_CHECK(fixture.run_until([&] {
        const auto accepted = client.accept_streams();
        for (std::size_t item = 0; item < accepted.size_; ++item) {
            const auto found = std::find(stream_ids.begin(), stream_ids.end(),
                accepted.streams_[item].stream_id_);
            if (found != stream_ids.end()) {
                accepted_streams[static_cast<std::size_t>(found - stream_ids.begin())] = true;
            }
        }
        for (std::size_t index = 0; index < stream_ids.size(); ++index) {
            if (!accepted_streams[index]) {
                continue;
            }
            const auto read = client.read_stream(stream_ids[index], read_buffer);
            if (read.status_ == ruvia::quic_stream_read_status::data) {
                received_value[index].append(reinterpret_cast<const char*>(read_buffer.data()), read.size_);
            } else if (read.status_ == ruvia::quic_stream_read_status::fin && index == 3) {
                application_fin = true;
            }
        }
        return std::ranges::all_of(accepted_streams, [](bool stream_accepted) { return stream_accepted; }) &&
               std::ranges::equal(received_value, expected) && application_fin;
    },
        std::chrono::seconds(8)));
    RUVIA_CHECK(std::ranges::all_of(accepted_streams, [](bool stream_accepted) { return stream_accepted; }));
    RUVIA_CHECK(std::ranges::equal(received_value, expected));
    RUVIA_CHECK(application_fin);

    // Drive pending acknowledgments without requiring a new ACK after the receive loop.
    fixture.pump();
    constexpr std::array<char, 0> no_reason{};
    const ruvia::quic_close_reason_view no_error_close{
        .kind_ = ruvia::quic_close_kind::application,
        .code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::no_error),
        .frame_type_ = 0,
        .reason_ = no_reason};
    RUVIA_CHECK_EQ(server.close(no_error_close), ruvia::quic_operation_status::accepted);
    const auto close_code = no_error_close.code_;
    RUVIA_CHECK(fixture.run_until([&] {
        const auto info = client.info();
        return info.state_ == ruvia::quic_connection_state::draining ||
               info.state_ == ruvia::quic_connection_state::closing ||
               info.close_error_code_ == close_code;
    }));
    RUVIA_CHECK_EQ(client.info().close_error_code_, close_code);
    RUVIA_CHECK_EQ(fixture.retire(token), ruvia::quic_operation_status::retired);
}
RUVIA_TEST(http3_quic_server_transport_requests_wire_close_before_explicit_local_retirement) {
    identity_files files;
    auto tls_config_value = files.server_tls_config();
    ruvia::quic_server_config server_config;
    server_config.max_active_connections_ = 1;
    counting_memory_resource memory;
    {
        plain_udp_quic_server_fixture fixture_value(tls_config_value, server_config,
            {.tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification}, &memory);
        (void)fixture_value.add_client();
        RUVIA_CHECK(fixture_value.run_until([&] {
            return !fixture_value.admissions().empty() &&
                   fixture_value.protocol_server().connection(fixture_value.admissions().front().token_).info().confirmed_ &&
                   fixture_value.client(0).transport_->connection().info().confirmed_;
        }));
        const auto token = fixture_value.admissions().front().token_;
        auto& server = fixture_value.protocol_server().connection(token);
        auto& first_client = fixture_value.client(0).transport_->connection();

        (void)fixture_value.add_client();
        fixture_value.pump(false);
        RUVIA_CHECK_EQ(fixture_value.pending_offers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture_value.admit_pending(1).status_,
            ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(fixture_value.pending_offers().size(), std::size_t{1});

        const auto stream = server.open_stream(true);
        RUVIA_CHECK_EQ(stream.status_, ruvia::quic_operation_status::accepted);
        std::vector<std::byte> owned_input(4U * 1024U * 1024U, std::byte{'x'});
        std::size_t accepted_bytes{};
        bool backpressured{};
        for (std::size_t attempt_value = 0; attempt_value < 64 && !backpressured; ++attempt_value) {
            const auto result_value = server.write_stream(stream.stream_id_,
                std::span<const std::byte>(owned_input).subspan(accepted_bytes));
            if (result_value.status_ == ruvia::quic_operation_status::would_block) {
                backpressured = true;
            } else {
                RUVIA_CHECK_EQ(result_value.status_, ruvia::quic_operation_status::accepted);
                RUVIA_CHECK(result_value.accepted_ > 0);
                accepted_bytes += result_value.accepted_;
            }
        }
        RUVIA_CHECK(backpressured);
        RUVIA_CHECK(accepted_bytes > 0);
        owned_input.clear();
        owned_input.shrink_to_fit();

        constexpr std::array<char, 0> no_reason{};
        const ruvia::quic_close_reason_view close_reason{
            .kind_ = ruvia::quic_close_kind::application,
            .code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_rejected),
            .frame_type_ = 0,
            .reason_ = no_reason};
        RUVIA_CHECK_EQ(server.close(close_reason), ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(server.close(close_reason), ruvia::quic_operation_status::accepted);
        constexpr std::array<char, 0> conflicting_reason{};
        const ruvia::quic_close_reason_view conflict{
            .kind_ = ruvia::quic_close_kind::application,
            .code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::general_protocol_error),
            .frame_type_ = 0,
            .reason_ = conflicting_reason};
        RUVIA_CHECK_EQ(server.close(conflict), ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(server.info().state_, ruvia::quic_connection_state::closing);
        RUVIA_CHECK_EQ(server.info().close_error_code_, close_reason.code_);

        RUVIA_CHECK(fixture_value.run_until([&] {
            const auto info = first_client.info();
            return info.state_ == ruvia::quic_connection_state::draining ||
                   info.state_ == ruvia::quic_connection_state::closing ||
                   info.close_error_code_ == close_reason.code_;
        }));
        RUVIA_CHECK_EQ(first_client.info().close_error_code_, close_reason.code_);
        RUVIA_CHECK_EQ(fixture_value.protocol_server().connection_count(), std::size_t{1});
        RUVIA_CHECK_EQ(fixture_value.retire(token), ruvia::quic_operation_status::retired);
        RUVIA_CHECK_EQ(fixture_value.protocol_server().connection_count(), std::size_t{0});
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)fixture_value.protocol_server().connection(token); }));

        RUVIA_CHECK_EQ(fixture_value.admit_pending(1).status_, ruvia::quic_operation_status::accepted);
        RUVIA_CHECK_EQ(fixture_value.admissions().size(), std::size_t{1});
        RUVIA_CHECK(fixture_value.run_until([&] {
            return fixture_value.protocol_server().connection(fixture_value.admissions().front().token_).info().quic_handshake_complete_;
        }));
    }
    RUVIA_CHECK_EQ(memory.allocations(), memory.deallocations());
    RUVIA_CHECK_EQ(memory.outstanding_bytes(), std::size_t{0});
}
RUVIA_TEST(http3_quic_server_transport_locally_retires_with_unsent_udp_packet_without_pumping) {
    identity_files files;
    auto tls_config_value = files.server_tls_config();
    plain_udp_quic_server_fixture fixture(tls_config_value);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] {
        return !fixture.admissions().empty() &&
               fixture.protocol_server().connection(fixture.admissions().front().token_).info().quic_handshake_complete_;
    }));
    const auto token = fixture.admissions().front().token_;
    auto& connection = fixture.protocol_server().connection(token);
    constexpr std::array<char, 0> reason{};
    const ruvia::quic_close_reason_view close_reason{
        .kind_ = ruvia::quic_close_kind::application,
        .code_ = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::internal_error),
        .frame_type_ = 0,
        .reason_ = reason};
    RUVIA_CHECK_EQ(connection.close(close_reason), ruvia::quic_operation_status::accepted);

    std::array<std::byte, 65536> packet_storage{};
    const auto packet = connection.write_packet(packet_storage, plain_udp_quic_server_fixture::clock::now());
    RUVIA_CHECK(packet.size_ > 0);
    const auto retained = std::vector<std::byte>(packet_storage.begin(),
        packet_storage.begin() + static_cast<std::ptrdiff_t>(packet.size_));
    RUVIA_CHECK_EQ(fixture.retire(token), ruvia::quic_operation_status::retired);
    RUVIA_CHECK(std::equal(retained.begin(), retained.end(), packet_storage.begin()));
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)fixture.protocol_server().connection(token); }));
}
RUVIA_TEST(http3_quic_server_transport_zero_capacity_and_repeated_close_are_safe) {
    identity_files files;
    auto tls_config_value = files.server_tls_config();
    ruvia::quic_server_config zero_config;
    zero_config.max_active_connections_ = 0;
    plain_udp_quic_server_fixture fixture(tls_config_value, zero_config);
    (void)fixture.add_client();
    RUVIA_CHECK(fixture.run_until([&] { return !fixture.pending_offers().empty(); },
        std::chrono::seconds(8), false));
    RUVIA_CHECK_EQ(fixture.admit_pending(1).status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(fixture.protocol_server().connection_count(), std::size_t{0});
    ruvia::detail::http3_quic_tls_context tls(tls_config_value, std::pmr::get_default_resource());

    ruvia::quic_server_config valid_config;
    valid_config.max_active_connections_ = 1;
    ruvia::detail::http3_quic_server_transport transport(tls, valid_config);
    const ruvia::quic_connection_token stale{1};
    RUVIA_CHECK_EQ(transport.server().retire(stale), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(transport.server().retire(stale), ruvia::quic_operation_status::retired);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)transport.server().connection(stale); }));
}
RUVIA_TEST(http3_quic_server_transport_rejects_invalid_configuration_without_taking_socket_ownership) {
    identity_files files;
    auto tls_config_value = files.server_tls_config();
    ruvia::detail::http3_quic_tls_context tls(tls_config_value, std::pmr::get_default_resource());
    asio::io_context io;
    asio::ip::udp::socket owned_socket(io);
    owned_socket.open(asio::ip::udp::v4());
    owned_socket.bind({asio::ip::address_v4::loopback(), 0});

    std::array<ruvia::quic_server_config, 4> invalid_configs{};
    invalid_configs[0].local_transport_parameters_.max_udp_payload_size_ = 1199;
    invalid_configs[1].local_transport_parameters_.active_connection_id_limit_ = 1;
    invalid_configs[2].limits_.max_datagram_size_ = 0;
    invalid_configs[3].limits_.max_lifetime_peer_streams_ = 0;
    for (const auto& config : invalid_configs) {
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            ruvia::detail::http3_quic_server_transport invalid(tls, config);
        }));
        RUVIA_CHECK(owned_socket.is_open());
        RUVIA_CHECK(owned_socket.local_endpoint().port() != 0);
    }
    ruvia::detail::http3_quic_server_transport valid(tls);
    RUVIA_CHECK(owned_socket.is_open());
}
RUVIA_TEST(http3_quic_server_transport_rejects_invalid_udp_address_without_taking_socket_ownership) {
    asio::io_context io;
    asio::ip::udp::socket socket(io);
    socket.open(asio::ip::udp::v4());
    socket.bind({asio::ip::address_v4::loopback(), 0});
    const auto owned_endpoint = socket.local_endpoint();
    RUVIA_CHECK(owned_endpoint.port() != 0);

    const asio::ip::udp::endpoint zero_port(asio::ip::address_v4::loopback(), 0);
    const auto invalid_peer = ruvia::detail::to_http3_quic_datagram_address(zero_port);
    RUVIA_CHECK(!(invalid_peer.index() == 0));
    RUVIA_CHECK_EQ(std::get<1>(invalid_peer),
        ruvia::detail::http3_quic_socket_address_error::zero_port);

    ruvia::detail::http3_quic_datagram_address invalid_address;
    invalid_address.port_ = 0;
    const auto invalid_output = ruvia::detail::to_udp_endpoint(invalid_address);
    RUVIA_CHECK(!(invalid_output.index() == 0));
    RUVIA_CHECK_EQ(std::get<1>(invalid_output),
        ruvia::detail::http3_quic_socket_address_error::zero_port);
    RUVIA_CHECK(socket.is_open());
    RUVIA_CHECK_EQ(socket.local_endpoint(), owned_endpoint);
}
RUVIA_TEST(http3_quic_server_transport_requires_and_validates_mutual_tls_certificates) {
    identity_files temporary_files;
    mutual_tls_files files(temporary_files.directory_);

    const auto trusted = perform_required_mutual_tls_handshake(files, 15440,
        &files.trusted_client_certificate_, &files.trusted_client_private_key_);
    RUVIA_CHECK(trusted.accepted_by_server_);
    RUVIA_CHECK(trusted.client_ready_);
    RUVIA_CHECK(trusted.client_verifies_localhost_);
    RUVIA_CHECK(trusted.server_handshake_complete_);
    RUVIA_CHECK(trusted.server_h3_negotiated_);
    RUVIA_CHECK_EQ(trusted.client_verify_result_, X509_V_OK);
    RUVIA_CHECK(trusted.client_certificate_.callback_count_ > 0);
    RUVIA_CHECK(trusted.client_certificate_.saw_leaf_);
    RUVIA_CHECK(trusted.client_certificate_.leaf_verified_);
    RUVIA_CHECK(trusted.client_certificate_.all_verified_);
    RUVIA_CHECK(!trusted.client_certificate_.saw_verification_failure_);
    RUVIA_CHECK_EQ(std::string_view(trusted.client_certificate_.leaf_common_name_.data(),
                       static_cast<std::size_t>(trusted.client_certificate_.leaf_name_length_)),
        std::string_view("trusted-client"));

    const auto missing = perform_required_mutual_tls_handshake(files, 15441, nullptr, nullptr);
    RUVIA_CHECK(missing.accepted_by_server_);
    RUVIA_CHECK(missing.client_verifies_localhost_);
    RUVIA_CHECK(missing.client_saw_remote_close_);
    RUVIA_CHECK(!missing.server_handshake_complete_);
    RUVIA_CHECK_EQ(missing.client_verify_result_, X509_V_OK);
    RUVIA_CHECK(!missing.client_certificate_.saw_leaf_);
    RUVIA_CHECK_EQ(missing.client_certificate_.callback_count_, std::size_t{0});

    const auto untrusted = perform_required_mutual_tls_handshake(files, 15442,
        &files.untrusted_client_certificate_, &files.untrusted_client_private_key_);
    RUVIA_CHECK(untrusted.accepted_by_server_);
    RUVIA_CHECK(untrusted.client_verifies_localhost_);
    RUVIA_CHECK(untrusted.client_saw_remote_close_);
    RUVIA_CHECK(!untrusted.server_handshake_complete_);
    RUVIA_CHECK_EQ(untrusted.client_verify_result_, X509_V_OK);
    RUVIA_CHECK(untrusted.client_certificate_.saw_leaf_);
    RUVIA_CHECK(!untrusted.client_certificate_.all_verified_);
    RUVIA_CHECK(untrusted.client_certificate_.saw_verification_failure_);
    RUVIA_CHECK_EQ(std::string_view(untrusted.client_certificate_.leaf_common_name_.data(),
                       static_cast<std::size_t>(untrusted.client_certificate_.leaf_name_length_)),
        std::string_view("untrusted-client"));
}
