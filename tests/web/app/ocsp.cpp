#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/web/http_server.h"

#include "tcp_port_reservation.h"
#include "test_tls_identity.h"

namespace ocsp_test {

using namespace std::chrono_literals;
using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

struct runtime final {
    ruvia::blocking_pool pool_{{.thread_count_ = 1, .queue_capacity_ = 64}};
    asio::io_context first_io_, second_io_;
    ruvia::event_loop_attachment first_{ruvia::attach_event_loop(first_io_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop_attachment second_{ruvia::attach_event_loop(second_io_, {.blocking_pool_ = &pool_})};
    std::array<ruvia::event_loop, 2> loops_{first_.loop(), second_.loop()};
    std::jthread first_thread_{[this] { first_.run(); }};
    std::jthread second_thread_{[this] { second_.run(); }};
    ~runtime() {
        first_.stop();
        second_.stop();
    }
};

struct issued_identity final {
    ruvia::test::scoped_temp_directory directory_{"ruvia-ocsp-tls"};
    X509* issuer_certificate_{};
    EVP_PKEY* issuer_key_{};
    std::unique_ptr<X509, decltype(&X509_free)> certificate_{nullptr, X509_free};
    std::filesystem::path certificate_file_;
    std::filesystem::path private_key_file_;

    issued_identity(ruvia::test::tls_identity& issuer, const char* common_name, long serial)
        : issuer_certificate_(SSL_CTX_get0_certificate(issuer.context_.native_handle())),
          issuer_key_(SSL_CTX_get0_privatekey(issuer.context_.native_handle())) {
        std::filesystem::permissions(directory_.path(), std::filesystem::perms::owner_all);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
        certificate_.reset(X509_new_ex(nullptr, nullptr));
        const std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
        if (!key || !certificate_ || !subject ||
            X509_set_version(certificate_.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate_.get()), serial) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(certificate_.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(certificate_.get()), 86400) ||
            X509_set_pubkey(certificate_.get(), key.get()) != 1 ||
            X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate_.get(), subject.get()) != 1 ||
            X509_set_issuer_name(certificate_.get(), X509_get_subject_name(issuer_certificate_)) != 1 ||
            ruvia::test::sign_tls_certificate(certificate_.get(), issuer_key_) <= 0) {
            throw std::runtime_error("cannot generate OCSP leaf identity");
        }
        certificate_file_ = directory_.path() / "cert.pem";
        private_key_file_ = directory_.path() / "key.pem";
        const std::unique_ptr<BIO, decltype(&BIO_free)> cert_out(
            BIO_new_file(certificate_file_.string().c_str(), "w"), BIO_free);
        const std::unique_ptr<BIO, decltype(&BIO_free)> key_out(
            BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
        if (!cert_out || PEM_write_bio_X509(cert_out.get(), certificate_.get()) != 1 ||
            !key_out || ruvia::test::write_tls_private_key(key_out.get(), key.get()) != 1) {
            throw std::runtime_error("cannot write OCSP leaf identity");
        }
    }
};

ruvia::tls_identity_config files(const issued_identity& identity) {
    return {.certificate_chain_file_ = identity.certificate_file_, .private_key_file_ = identity.private_key_file_};
}

// A real issuer-signed BasicOCSPResponse, with a nonce distinguishing renewals.
std::pmr::vector<std::byte> response(const issued_identity& identity, unsigned char nonce) {
    const std::unique_ptr<OCSP_CERTID, decltype(&OCSP_CERTID_free)> id(
        OCSP_cert_to_id(EVP_sha256(), identity.certificate_.get(), identity.issuer_certificate_), OCSP_CERTID_free);
    const std::unique_ptr<OCSP_BASICRESP, decltype(&OCSP_BASICRESP_free)> basic(OCSP_BASICRESP_new(), OCSP_BASICRESP_free);
    const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> current(X509_gmtime_adj(nullptr, -60), ASN1_TIME_free);
    const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> next(X509_gmtime_adj(nullptr, 3600), ASN1_TIME_free);
    if (!id || !basic || !current || !next ||
        !OCSP_basic_add1_status(basic.get(), id.get(), V_OCSP_CERTSTATUS_GOOD, 0, nullptr, current.get(), next.get()) ||
        OCSP_basic_add1_nonce(basic.get(), &nonce, 1) != 1 ||
        OCSP_basic_sign(basic.get(), identity.issuer_certificate_, identity.issuer_key_, EVP_sha256(), nullptr, 0) != 1) {
        throw std::runtime_error("cannot sign OCSP test response");
    }
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> encoded(
        OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic.get()), OCSP_RESPONSE_free);
    const int size = encoded ? i2d_OCSP_RESPONSE(encoded.get(), nullptr) : -1;
    if (size <= 0) {
        throw std::runtime_error("cannot encode OCSP test response");
    }
    std::pmr::vector<std::byte> result(static_cast<std::size_t>(size), ruvia::process_resource());
    auto* cursor = reinterpret_cast<unsigned char*>(result.data());
    if (i2d_OCSP_RESPONSE(encoded.get(), &cursor) != size) {
        throw std::runtime_error("cannot encode OCSP test response");
    }
    return result;
}

// Deadlines bound both TCP and OpenSSL's real client handshake; no fake peer.
template <typename initiation_type>
void client_io(asio::ip::tcp::socket& socket, initiation_type&& initiate) {
    auto& io = static_cast<asio::io_context&>(socket.get_executor().context());
    bool done = false;
    asio::error_code error;
    initiate([&](const asio::error_code& result) {
        error = result;
        done = true;
    });
    io.restart();
    io.run_for(3s);
    if (!done) {
        asio::error_code ignored;
        socket.close(ignored);
        io.restart();
        io.run();
        error = asio::error::timed_out;
    }
    if (error) {
        throw std::system_error(error, "OCSP test client I/O");
    }
}

std::unique_ptr<tls_stream> connect(asio::io_context& io, int version,
    const asio::ip::tcp::endpoint& endpoint, const char* name, bool request_status = true) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    if (SSL_CTX_set_min_proto_version(client.native_handle(), version) != 1 ||
        SSL_CTX_set_max_proto_version(client.native_handle(), version) != 1) {
        throw std::runtime_error("cannot configure OCSP test TLS version");
    }
    auto stream = std::make_unique<tls_stream>(io, client);
    if ((name && SSL_set_tlsext_host_name(stream->native_handle(), name) != 1) ||
        (request_status && SSL_set_tlsext_status_type(stream->native_handle(), TLSEXT_STATUSTYPE_ocsp) != 1)) {
        throw std::runtime_error("cannot request OCSP status");
    }
    client_io(stream->next_layer(), [&](auto completion) {
        stream->next_layer().async_connect(endpoint, std::move(completion));
    });
    client_io(stream->next_layer(), [&](auto completion) {
        stream->async_handshake(asio::ssl::stream_base::client, std::move(completion));
    });
    return stream;
}

std::pmr::vector<std::byte> received(SSL* ssl) {
    const unsigned char* bytes = nullptr;
    const auto size = SSL_get_tlsext_status_ocsp_resp(ssl, &bytes);
    if (size <= 0) {
        return std::pmr::vector<std::byte>(ruvia::process_resource());
    }
    const auto* begin = reinterpret_cast<const std::byte*>(bytes);
    return {begin, begin + size, ruvia::process_resource()};
}

void expect_status(SSL* ssl, const issued_identity& identity,
    const std::pmr::vector<std::byte>& expected) {
    const auto bytes = received(ssl);
    ASSERT_EQ(bytes, expected);
    const std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl), X509_free);
    ASSERT_NE(peer, nullptr);
    EXPECT_EQ(X509_cmp(peer.get(), identity.certificate_.get()), 0);
    if (bytes.empty()) {
        return;
    }
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> parsed(
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(OCSP_response_status(parsed.get()), OCSP_RESPONSE_STATUS_SUCCESSFUL);
    const std::unique_ptr<OCSP_BASICRESP, decltype(&OCSP_BASICRESP_free)> basic(
        OCSP_response_get1_basic(parsed.get()), OCSP_BASICRESP_free);
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> trust(X509_STORE_new(), X509_STORE_free);
    ASSERT_NE(basic, nullptr);
    ASSERT_NE(trust, nullptr);
    ASSERT_EQ(X509_STORE_add_cert(trust.get(), identity.issuer_certificate_), 1);
    EXPECT_EQ(OCSP_basic_verify(basic.get(), nullptr, trust.get(), 0), 1);
    const std::unique_ptr<OCSP_CERTID, decltype(&OCSP_CERTID_free)> id(
        OCSP_cert_to_id(EVP_sha256(), peer.get(), identity.issuer_certificate_), OCSP_CERTID_free);
    ASSERT_NE(id, nullptr);
    int status = -1, reason = -1;
    ASN1_GENERALIZEDTIME *revoked = nullptr, *current = nullptr, *next = nullptr;
    ASSERT_EQ(OCSP_resp_find_status(basic.get(), id.get(), &status, &reason, &revoked, &current, &next), 1);
    EXPECT_EQ(status, V_OCSP_CERTSTATUS_GOOD);
}

}  // namespace ocsp_test

TEST(Ruvia, ocsp_stapling_selects_identity_and_replaces_certificate_status_pairs) {
    using namespace ocsp_test;
    ruvia::test::tls_identity issuer("ocsp-issuer-ca");
    issued_identity primary(issuer, "primary", 1), exact(issuer, "exact", 2), wildcard(issuer, "wildcard", 3), local(issuer, "local", 4);
    issued_identity renewed_primary(issuer, "renewed-primary", 5), renewed_exact(issuer, "renewed-exact", 6);
    runtime runtime;
    const auto caller = runtime.loops_[0];
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const asio::ip::tcp::endpoint first(asio::ip::make_address("127.0.0.1"), port);
    const asio::ip::tcp::endpoint second(asio::ip::make_address("127.0.0.2"), port);
    const ruvia::listener_endpoint listener{.address_ = "0.0.0.0", .port_ = port};
    const auto configuration = [&](bool renewed) {
        return ruvia::tls_config{
            .identity_ = files(renewed ? renewed_primary : primary),
            .ocsp_response_ = response(renewed ? renewed_primary : primary, renewed ? 11 : 1),
            .sni_ = {{.host_ = "api.example.test", .identity_ = files(renewed ? renewed_exact : exact), .ocsp_response_ = response(renewed ? renewed_exact : exact, renewed ? 12 : 2)},
                {.host_ = "*.example.test", .identity_ = files(wildcard), .ocsp_response_ = response(wildcard, renewed ? 13 : 3)},
                {.host_ = "empty.example.test", .identity_ = files(exact)}},
            .local_addresses_ = {{.address_ = "127.0.0.2", .identity_ = files(local), .ocsp_response_ = response(local, renewed ? 14 : 4)}}};
    };
    ruvia::http_server_group server(runtime.loops_, [](const ruvia::worker_handle&) {
        return ruvia::http_server_callbacks{.request_stream_ = [](ruvia::request_stream& stream) -> ruvia::task<void> {
            if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
                co_await stream.response().write("ocsp");
                co_await stream.response().end();
            }
        }};
    });
    const auto original = configuration(false);
    server.listen({.address_ = "0.0.0.0", .https_ = port, .tls_ = original, .dispatch_ = ruvia::listener_dispatch::request_stream});
    caller.start(server.start(caller.handle())).get();
    auto retained = connect(io, TLS1_3_VERSION, first, nullptr);
    expect_status(retained->native_handle(), primary, original.ocsp_response_);
    const auto check = [&](const ruvia::tls_config& config, bool renewed) {
        for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(version);
            expect_status(connect(io, version, first, nullptr)->native_handle(), renewed ? renewed_primary : primary, config.ocsp_response_);
            expect_status(connect(io, version, first, "unknown.test")->native_handle(), renewed ? renewed_primary : primary, config.ocsp_response_);
            expect_status(connect(io, version, second, "API.example.TEST")->native_handle(), renewed ? renewed_exact : exact, config.sni_[0].ocsp_response_);
            expect_status(connect(io, version, first, "www.example.test")->native_handle(), wildcard, config.sni_[1].ocsp_response_);
            expect_status(connect(io, version, second, nullptr)->native_handle(), local, config.local_addresses_[0].ocsp_response_);
            expect_status(connect(io, version, second, "empty.example.test")->native_handle(), exact, config.sni_[2].ocsp_response_);
            EXPECT_TRUE(received(connect(io, version, first, nullptr, false)->native_handle()).empty());
        }
    };
    check(original, false);
    auto replacement = configuration(true);
    caller.start(server.replace_tls_identities(caller.handle(), listener, replacement)).get();
    check(replacement, true);
    expect_status(retained->native_handle(), primary, original.ocsp_response_);

    // Reject broken primary/SNI/address DER atomically, including trailing
    // bytes and BER encodings accepted by OpenSSL's permissive d2i parser.
    const std::array<std::pmr::vector<std::byte>, 3> invalid{
        std::pmr::vector<std::byte>{std::byte{0}},
        std::pmr::vector<std::byte>{std::byte{0x30}, std::byte{3}, std::byte{0x0a}, std::byte{1}, std::byte{3}, std::byte{0}},
        std::pmr::vector<std::byte>{std::byte{0x30}, std::byte{0x81}, std::byte{3}, std::byte{0x0a}, std::byte{1}, std::byte{3}}};
    for (const auto& bytes : invalid) {
        for (int entry = 0; entry != 3; ++entry) {
            auto rejected = replacement;
            auto& target = entry == 0 ? rejected.ocsp_response_ : entry == 1 ? rejected.sni_[0].ocsp_response_
                                                                             : rejected.local_addresses_[0].ocsp_response_;
            target = bytes;
            EXPECT_THROW(caller.start(server.replace_tls_identities(caller.handle(), listener, std::move(rejected))).get(), std::invalid_argument);
        }
    }
    auto library = std::make_shared<ruvia::tls_certificate_store>();
    ruvia::tls_config mixed{.ocsp_response_ = replacement.ocsp_response_, .certificate_store_ = library, .default_certificate_ = 1};
    EXPECT_THROW(caller.start(server.replace_tls_identities(caller.handle(), listener, std::move(mixed))).get(), std::invalid_argument);
    check(replacement, true);

    replacement.ocsp_response_.clear();
    replacement.sni_[0].ocsp_response_.clear();
    caller.start(server.replace_tls_identities(caller.handle(), listener, replacement)).get();
    check(replacement, true);
    retained.reset();
    caller.start(server.shutdown(caller.handle())).get();
}
