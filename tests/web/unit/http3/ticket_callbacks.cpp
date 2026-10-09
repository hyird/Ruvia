#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/http/quic_connection.h"

#include "http3/http3_quic_client_tls_context.h"
#include "http3/openssl_quic_crypto_provider.h"
#include "http3/openssl_quic_tls_session.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {

template <typename t_type, void (*release)(t_type*)>
struct openssl_deleter final {
    void operator()(t_type* pointer) const noexcept {
        release(pointer);
    }
};

template <typename t_type, void (*release)(t_type*)>
using openssl_owner = std::unique_ptr<t_type, openssl_deleter<t_type, release>>;

using ssl_context_owner = openssl_owner<SSL_CTX, SSL_CTX_free>;
using ssl_owner = openssl_owner<SSL, SSL_free>;
using session_owner = openssl_owner<SSL_SESSION, SSL_SESSION_free>;
using key_owner = openssl_owner<EVP_PKEY, EVP_PKEY_free>;
using certificate_owner = openssl_owner<X509, X509_free>;

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("TLS ticket test fixture setup failed");
    }
}

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations_{};
    std::size_t deallocations_{};

    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct tls_pair final {
    ssl_context_owner client_context_{SSL_CTX_new(TLS_method())};
    ssl_context_owner server_context_{SSL_CTX_new(TLS_method())};
    ssl_owner client_{};
    ssl_owner server_{};

    tls_pair() {
        require(client_context_ != nullptr);
        require(server_context_ != nullptr);
        require(SSL_CTX_set_min_proto_version(client_context_.get(), TLS1_3_VERSION) == 1);
        require(SSL_CTX_set_max_proto_version(client_context_.get(), TLS1_3_VERSION) == 1);
        require(SSL_CTX_set_min_proto_version(server_context_.get(), TLS1_3_VERSION) == 1);
        require(SSL_CTX_set_max_proto_version(server_context_.get(), TLS1_3_VERSION) == 1);
        require(SSL_CTX_set_ciphersuites(client_context_.get(), "TLS_AES_128_GCM_SHA256") == 1);
        require(SSL_CTX_set_ciphersuites(server_context_.get(), "TLS_AES_128_GCM_SHA256") == 1);

        key_owner key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"));
        require(key != nullptr);
        certificate_owner certificate(X509_new_ex(nullptr, nullptr));
        require(certificate != nullptr);
        require(X509_set_version(certificate.get(), 2) == 1);
        require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
        require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) != nullptr);
        require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
        require(X509_set_pubkey(certificate.get(), key.get()) == 1);
        const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        require(name != nullptr);
        constexpr unsigned char common_name[] = "localhost";
        require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC, common_name, -1, -1, 0) == 1);
        require(X509_set_subject_name(certificate.get(), name.get()) == 1);
        require(X509_set_issuer_name(certificate.get(), name.get()) == 1);
        require(ruvia::test::sign_tls_certificate(certificate.get(), key.get()) > 0);
        require(SSL_CTX_use_certificate(server_context_.get(), certificate.get()) == 1);
        require(SSL_CTX_use_PrivateKey(server_context_.get(), key.get()) == 1);
        SSL_CTX_set_verify(client_context_.get(), SSL_VERIFY_NONE, nullptr);

        client_.reset(SSL_new(client_context_.get()));
        server_.reset(SSL_new(server_context_.get()));
        require(client_ != nullptr);
        require(server_ != nullptr);

        BIO* client_bio{};
        BIO* server_bio{};
        require(BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) == 1);
        SSL_set_bio(client_.get(), client_bio, client_bio);
        SSL_set_bio(server_.get(), server_bio, server_bio);
        SSL_set_connect_state(client_.get());
        SSL_set_accept_state(server_.get());
        complete_handshake();
    }

    void complete_handshake() {
        bool client_done{};
        bool server_done{};
        for (int attempt_value = 0; attempt_value < 100 && !(client_done && server_done); ++attempt_value) {
            if (!client_done) {
                const int result_value = SSL_do_handshake(client_.get());
                client_done = result_value == 1;
                if (!client_done) {
                    const int error = SSL_get_error(client_.get(), result_value);
                    require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
                }
            }
            if (!server_done) {
                const int result_value = SSL_do_handshake(server_.get());
                server_done = result_value == 1;
                if (!server_done) {
                    const int error = SSL_get_error(server_.get(), result_value);
                    require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
                }
            }
        }
        require(client_done);
        require(server_done);
    }

    [[nodiscard]] session_owner issue_ticket() {
        require(SSL_new_session_ticket(server_.get()) == 1);
        std::array<unsigned char, 1> byte{};
        std::size_t size{};
        const int result_value = SSL_read_ex(client_.get(), byte.data(), byte.size(), &size);
        if (result_value != 1) {
            const int error = SSL_get_error(client_.get(), result_value);
            require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
        }
        session_owner session_value(SSL_get1_session(client_.get()));
        require(session_value != nullptr);
        require(SSL_SESSION_is_resumable(session_value.get()) == 1);
        const unsigned char* ticket{};
        std::size_t ticket_size{};
        SSL_SESSION_get0_ticket(session_value.get(), &ticket, &ticket_size);
        require(ticket != nullptr);
        require(ticket_size != 0);
        return session_value;
    }
};

ruvia::quic_address loopback_address(std::uint16_t port) {
    ruvia::quic_address result;
    result.bytes_[0] = std::byte{127};
    result.bytes_[3] = std::byte{1};
    result.port_ = port;
    result.family_ = ruvia::quic_address_family::ipv4;
    return result;
}

struct observing_tls_driver final {
    explicit observing_tls_driver(ruvia::quic_tls_driver_view delegate) noexcept
        : delegate_(delegate) {}

    static ruvia::quic_tls_drive_result drive(void* context_value, ruvia::quic_tls_handshake& handshake) noexcept {
        auto& self = *static_cast<observing_tls_driver*>(context_value);
        ++self.drive_calls_;
        self.last_result_ = self.delegate_.drive_(self.delegate_.context_, handshake);
        return self.last_result_;
    }
    static void retire(void* context_value) noexcept {
        auto& self = *static_cast<observing_tls_driver*>(context_value);
        self.delegate_.retire_(self.delegate_.context_);
    }
    [[nodiscard]] ruvia::quic_tls_driver_view view() noexcept {
        return {.context_ = this, .drive_ = drive, .retire_ = retire};
    }

    ruvia::quic_tls_driver_view delegate_;
    std::size_t drive_calls_{};
    ruvia::quic_tls_drive_result last_result_{};
};

}  // namespace

RUVIA_TEST(http3_quic_tls_new_ticket_callback_snapshots_a_real_session_across_ssl_retirement) {
    using tls_session = ruvia::detail::openssl_quic_tls_session;
    using ticket_context = ruvia::detail::http3_quic_client_tls_context;
    static_assert(std::is_nothrow_move_constructible_v<ticket_context::ticket_lease>);
    static_assert(!std::is_move_assignable_v<ticket_context::ticket_lease>);

    tls_pair pair;
    constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
    auto owner_value = std::make_unique<tls_session>(
        pair.client_context_.get(), ruvia::quic_role::client, alpn);
    auto issued_ticket = pair.issue_ticket();
    const auto callback_value = SSL_CTX_sess_get_new_cb(pair.client_context_.get());
    RUVIA_CHECK(callback_value != nullptr);
    RUVIA_CHECK(callback_value(owner_value->native_handle(), issued_ticket.get()) == 0);
    owner_value->stop();
    auto captured_value = owner_value->take_resumption_session();
    RUVIA_CHECK(captured_value != nullptr);
    RUVIA_CHECK(SSL_SESSION_is_resumable(captured_value.get()) == 1);
    const unsigned char* captured_ticket{};
    std::size_t captured_ticket_size{};
    SSL_SESSION_get0_ticket(captured_value.get(), &captured_ticket, &captured_ticket_size);
    RUVIA_CHECK(captured_ticket != nullptr);
    RUVIA_CHECK(captured_ticket_size != 0);

    counting_resource resource;
    {
        ticket_context cache({}, &resource);
        cache.remember_ticket(issued_ticket.get(), "ticket-cache-hostname.example.",
            ruvia::quic_version::v1, {}, std::nullopt);
        issued_ticket.reset();
        auto lease_value = cache.take_ticket("ticket-cache-hostname.example", ruvia::quic_version::v1);
        RUVIA_CHECK(lease_value.has_value());
        RUVIA_CHECK(SSL_SESSION_is_resumable(lease_value->session_.get()) == 1);
        RUVIA_CHECK(!cache.take_ticket("ticket-cache-hostname.example", ruvia::quic_version::v1).has_value());
    }
    RUVIA_CHECK(resource.allocations_ != 0);
    RUVIA_CHECK(resource.allocations_ == resource.deallocations_);
    ticket_context wrong_host({});
    wrong_host.remember_ticket(captured_value.get(), "localhost", ruvia::quic_version::v1, {}, std::nullopt);
    RUVIA_CHECK(!wrong_host.take_ticket("otherhost", ruvia::quic_version::v1).has_value());
    ticket_context wrong_version({});
    wrong_version.remember_ticket(captured_value.get(), "localhost", ruvia::quic_version::v1, {}, std::nullopt);
    RUVIA_CHECK(!wrong_version.take_ticket("localhost", ruvia::quic_version::v2).has_value());

    auto resumed_owner = std::make_unique<tls_session>(pair.client_context_.get(),
        ruvia::quic_role::client, alpn, std::string_view{}, std::pmr::get_default_resource(),
        captured_value.get());
    RUVIA_CHECK(resumed_owner->take_resumption_session() == nullptr);
    resumed_owner->stop();
}

RUVIA_TEST(http3_quic_tls_preexisting_error_does_not_turn_want_read_into_failure) {
    ruvia::detail::openssl_quic_crypto_provider crypto(std::pmr::get_default_resource());
    ssl_context_owner context(SSL_CTX_new(TLS_method()));
    RUVIA_CHECK(context != nullptr);
    RUVIA_CHECK(SSL_CTX_set_ciphersuites(context.get(), "TLS_AES_128_GCM_SHA256") == 1);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    constexpr std::array<unsigned char, 3> alpn{2, 'h', '3'};
    ruvia::detail::openssl_quic_tls_session session_value(
        context.get(), ruvia::quic_role::client, alpn);
    observing_tls_driver driver(session_value.driver_view());
    ruvia::quic_connection_config config;
    config.role_ = ruvia::quic_role::client;
    config.local_address_ = loopback_address(43101);
    config.peer_address_ = loopback_address(4433);
    ruvia::quic_connection connection(config, crypto.view(), driver.view(),
        std::pmr::get_default_resource(), std::chrono::steady_clock::now());

    std::array<std::byte, 1500> initial_packet{};
    const auto previous_drive_calls = driver.drive_calls_;
    ERR_raise(ERR_LIB_USER, 1);
    const auto started = connection.write_packet(initial_packet, std::chrono::steady_clock::now());
    RUVIA_CHECK(started.size_ != 0);
    RUVIA_CHECK(driver.drive_calls_ > previous_drive_calls);
    RUVIA_CHECK(driver.last_result_.progress_ == ruvia::quic_tls_progress::need_input);
    RUVIA_CHECK(!connection.tls_handshake().failed());
    session_value.stop();
}
