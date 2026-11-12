#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "app_run.h"
#include "scoped_temp_directory.h"
#include "tcp_port_reservation.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace tls_paths_test {

using namespace std::chrono_literals;

class tls_path_controller final {
public:
    RUVIA_ROUTES_BEGIN(tls_path_controller)
    RUVIA_GET("/tls", secure);
    RUVIA_GET("/tls-peer", peer_identity);
    RUVIA_GET("/tls-truncated", truncated);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> secure(ruvia::context& context) {
        co_return context.text(std::string_view("secure"));
    }

    ruvia::task<ruvia::http_response> peer_identity(ruvia::context& context) {
        const auto connection = context.conn();
        const auto subject = connection.tls()->client_certificate_subject();
        co_return context.text(!subject           ? std::string_view("absent")
                               : subject->empty() ? std::string_view("present-empty")
                                                  : *subject);
    }

    // Commits the response, then fails before the body is complete.
    ruvia::task<void> truncated(ruvia::context& context) {
        if (co_await context.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context.stream().write("partial");
        throw std::runtime_error("upstream failed");
    }
};

// A directory whose name no legacy ANSI code page can represent: CJK,
// Cyrillic and a character outside the Basic Multilingual Plane.
class non_ascii_directory final {
public:
    non_ascii_directory()
        : directory_("ruvia-tls-\xe8\xaf\x81\xe4\xb9\xa6-\xd1\x82\xd0\xb5\xd1\x81\xd1\x82-\xf0\x9f\x94\x92") {}

    non_ascii_directory(const non_ascii_directory&) = delete;
    non_ascii_directory& operator=(const non_ascii_directory&) = delete;

    [[nodiscard]] std::filesystem::path file(std::u8string_view name) const {
        return directory_.path() / std::filesystem::path(name);
    }

private:
    ruvia::test::scoped_temp_directory directory_;
};

// Written through the native path API, independent of the server's loader.
void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write TLS test file");
    }
}

std::pmr::string encrypted_private_key(ruvia::test::tls_identity& identity, std::string_view password) {
    const std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    if (!output || ruvia::test::write_tls_private_key(output.get(),
                       SSL_CTX_get0_privatekey(identity.context_.native_handle()), password, true) != 1) {
        throw std::runtime_error("cannot encrypt TLS test private key");
    }
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    if (size <= 0 || data == nullptr) {
        throw std::runtime_error("cannot read encrypted TLS test private key");
    }
    return std::pmr::string(data, static_cast<std::size_t>(size), ruvia::process_resource());
}

// A verified identity may have no Subject attributes (RFC 5280 section 4.1.2.6).
// A separate, conforming CA avoids relying on direct trust of an empty-subject
// leaf. The existing named test identity remains separately trusted.
asio::ssl::context empty_subject_client(ruvia::test::tls_identity& issuer,
    const std::filesystem::path& trust_bundle) {
    auto* const key = SSL_CTX_get0_privatekey(issuer.context_.native_handle());
    const auto authority = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    const auto authority_name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    if (!authority || !authority_name || X509_set_version(authority.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(authority.get()), 3) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(authority.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(authority.get()), 86400) ||
        X509_set_pubkey(authority.get(), key) != 1 ||
        X509_NAME_add_entry_by_txt(authority_name.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("empty-subject-client-ca"), -1, -1, 0) != 1 ||
        X509_set_subject_name(authority.get(), authority_name.get()) != 1 ||
        X509_set_issuer_name(authority.get(), authority_name.get()) != 1) {
        throw std::runtime_error("cannot create empty-subject client CA");
    }
    X509V3_CTX authority_context{};
    X509V3_set_ctx(&authority_context, authority.get(), authority.get(), nullptr, nullptr, 0);
    const auto constraints = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_nconf_nid(nullptr, &authority_context, NID_basic_constraints, "critical,CA:TRUE"),
        X509_EXTENSION_free);
    if (!constraints || X509_add_ext(authority.get(), constraints.get(), -1) != 1 ||
        ruvia::test::sign_tls_certificate(authority.get(), key) <= 0) {
        throw std::runtime_error("cannot sign empty-subject client CA");
    }
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    if (!certificate || !subject || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 2) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) ||
        X509_set_pubkey(certificate.get(), key) != 1 ||
        X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
        X509_set_issuer_name(certificate.get(), authority_name.get()) != 1) {
        throw std::runtime_error("cannot create empty-subject client identity");
    }
    X509V3_CTX extension_context{};
    X509V3_set_ctx(&extension_context, authority.get(), certificate.get(), nullptr, nullptr, 0);
    const auto alternative_name = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name,
            "critical,URI:urn:ruvia:test:client"),
        X509_EXTENSION_free);
    if (!alternative_name || X509_add_ext(certificate.get(), alternative_name.get(), -1) != 1 ||
        ruvia::test::sign_tls_certificate(certificate.get(), key) <= 0) {
        throw std::runtime_error("cannot sign empty-subject client identity");
    }
    asio::ssl::context client(asio::ssl::context::tls_client);
    if (SSL_CTX_use_certificate(client.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(client.native_handle(), key) != 1) {
        throw std::runtime_error("cannot load empty-subject client identity");
    }
    const auto encoded = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
    if (!encoded || PEM_write_bio_X509(encoded.get(), authority.get()) != 1) {
        throw std::runtime_error("cannot encode empty-subject client certificate");
    }
    char* bytes = nullptr;
    const auto size = BIO_get_mem_data(encoded.get(), &bytes);
    std::ofstream output(trust_bundle, std::ios::binary | std::ios::app);
    if (size <= 0 || bytes == nullptr || !output.write(bytes, size)) {
        throw std::runtime_error("cannot trust empty-subject client certificate");
    }
    return client;
}

template <typename fn_type>
bool throws_system_error(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::system_error&) {
        return true;
    }
}

// The peer deliberately permits legacy algorithms so TLS 1.0 rejection and
// TLS 1.1 success exercise the listener, not the test client's defaults.
void configure_client_version(asio::ssl::context& context, int version) {
    if (version == 0) {
        return;
    }
    if (SSL_CTX_set_min_proto_version(context.native_handle(), version) != 1 ||
        SSL_CTX_set_max_proto_version(context.native_handle(), version) != 1) {
        throw std::runtime_error("cannot configure the TLS test client version");
    }
    if (version <= TLS1_1_VERSION) {
        SSL_CTX_set_security_level(context.native_handle(), 0);
    }
}

// Verifies the server chain against the test CA; `host` is both the SNI name
// and, when `verify_host` is set, the name the certificate must match.
std::pmr::string https_get(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    const std::filesystem::path& ca_file, const char* host, bool verify_host,
    std::string_view target = "/tls", SSL_CTX* client_identity = nullptr, int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(ca_file.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (client_identity != nullptr &&
        (SSL_use_certificate(stream.native_handle(), SSL_CTX_get0_certificate(client_identity)) != 1 ||
            SSL_use_PrivateKey(stream.native_handle(), SSL_CTX_get0_privatekey(client_identity)) != 1)) {
        throw std::runtime_error("cannot present TLS test client identity");
    }
    if (SSL_set_tlsext_host_name(stream.native_handle(), host) != 1 ||
        (verify_host && SSL_set1_host(stream.native_handle(), host) != 1)) {
        return std::pmr::string{ruvia::process_resource()};
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    const auto request = std::pmr::string("GET ", ruvia::process_resource()) + std::pmr::string(target, ruvia::process_resource()) + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    std::pmr::string response{ruvia::process_resource()};
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

bool serves_secure_body(const std::pmr::string& response, std::string_view expected = "secure") {
    const auto body_start = response.find("\r\n\r\n");
    return response.starts_with("HTTP/1.1 200 ") && body_start != std::pmr::string::npos &&
           std::string_view(response).substr(body_start + 4) == expected;
}

// RFC 9112 §9.8: an HTTP/1.0 response body is delimited by the close, so a
// stream that fails after its head must end without close_notify; a clean TLS
// close would make the truncated body read as complete.
bool truncated_close_delimited_stream_lacks_close_notify(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    constexpr std::string_view request = "GET /tls-truncated HTTP/1.0\r\nHost: localhost\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    if (error) {
        return false;
    }
    std::pmr::string response{ruvia::process_resource()};
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response.starts_with("HTTP/1.1 200 ") && response.ends_with("\r\n\r\npartial") &&
           error == asio::ssl::error::stream_truncated;
}

struct alpn_handshake final {
    asio::error_code error_;
    std::pmr::string selected_{ruvia::process_resource()};
};

// Handshakes offering `protocols` (ALPN wire format) and reports the protocol
// the server selected.
alpn_handshake negotiate_alpn(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view protocols, int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_alpn_protos(stream.native_handle(), reinterpret_cast<const unsigned char*>(protocols.data()),
            static_cast<unsigned int>(protocols.size())) != 0) {
        throw std::runtime_error("cannot offer ALPN protocols");
    }
    alpn_handshake result;
    stream.next_layer().connect(endpoint, result.error_);
    if (!result.error_) {
        stream.handshake(asio::ssl::stream_base::client, result.error_);
    }
    if (!result.error_) {
        const unsigned char* selected = nullptr;
        unsigned int size = 0;
        SSL_get0_alpn_selected(stream.native_handle(), &selected, &size);
        result.selected_.assign(reinterpret_cast<const char*>(selected), size);
    }
    return result;
}

// RFC 7301 §3.2: a client offering no protocol the server speaks gets a fatal
// no_application_protocol alert rather than a connection without ALPN.
bool refuses_with_no_application_protocol(const alpn_handshake& handshake) {
    return handshake.error_.category() == asio::error::get_ssl_category() &&
           ERR_GET_REASON(static_cast<unsigned long>(handshake.error_.value())) == SSL_R_TLSV1_ALERT_NO_APPLICATION_PROTOCOL;
}

bool bounded_tls_connection_ends(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, int version) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    std::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (error) {
        return false;
    }
    stream.handshake(asio::ssl::stream_base::client, error);
    if (error) {
        return false;
    }
    unsigned completed = 0;
    for (; completed != 64; ++completed) {
        if (version == TLS1_3_VERSION && completed % 4 == 0 &&
            SSL_key_update(stream.native_handle(), SSL_KEY_UPDATE_REQUESTED) != 1) {
            return false;
        }
        error.clear();
        constexpr std::string_view request = "GET /tls HTTP/1.1\r\nHost: localhost\r\n\r\n";
        asio::write(stream, asio::buffer(request), error);
        if (error) {
            return completed != 0;
        }
        bool done = false;
        std::pmr::string response{ruvia::process_resource()};
        asio::async_read_until(stream, asio::dynamic_buffer(response), "\r\n\r\nsecure",
            [&](const std::error_code& result, std::size_t) { error = result; done = true; });
        io.restart();
        io.run_for(1s);
        if (!done) {
            stream.next_layer().close();
            io.restart();
            io.run();
            return false;
        }
        if (error) {
            return completed != 0;
        }
    }
    return false;
}

// A client context that authenticates nothing; tests choose version and offer.
asio::ssl::context cipher_client(const char* cipher_suites, int version) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    if (SSL_CTX_set_cipher_list(client.native_handle(), cipher_suites) != 1) {
        throw std::runtime_error("cannot configure the TLS test client ciphers");
    }
    return client;
}

// The suite selected from the client's offer; empty when the handshake fails.
std::pmr::string selected_cipher(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    const char* offer, int version = TLS1_2_VERSION, const char* host = "localhost") {
    auto client = cipher_client(offer, version);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), host) != 1) {
        throw std::runtime_error("cannot configure the TLS test SNI");
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (error) {
        return std::pmr::string{ruvia::process_resource()};
    }
    return std::pmr::string(SSL_get_cipher_name(stream.native_handle()), ruvia::process_resource());
}

// 在指定 TLS 版本下建立会话，再以第二个 SNI 尝试恢复。
bool resumes_tls_session_under(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    int version, const char* first, const char* second) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    configure_client_version(client, version);
    std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> session(nullptr, SSL_SESSION_free);
    for (const char* name : {first, second}) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        if (SSL_set_tlsext_host_name(stream.native_handle(), name) != 1 ||
            (session && SSL_set_session(stream.native_handle(), session.get()) != 1)) {
            throw std::runtime_error("cannot prepare the TLS resumption client");
        }
        asio::error_code error;
        stream.next_layer().connect(endpoint, error);
        if (!error) {
            stream.handshake(asio::ssl::stream_base::client, error);
        }
        if (error) {
            throw std::system_error(error, "TLS resumption handshake failed");
        }
        if (session) {
            return SSL_session_reused(stream.native_handle()) == 1;
        }
        if (version == TLS1_3_VERSION) {
            // TLS 1.3 的 ticket 在握手后发送；读取实际响应以处理 NewSessionTicket。
            const auto request = std::pmr::string("GET /tls HTTP/1.1\r\nHost: ", ruvia::process_resource()) + name + "\r\nConnection: close\r\n\r\n";
            asio::write(stream, asio::buffer(request), error);
            std::array<char, 1024> buffer;
            while (!error) {
                (void)stream.read_some(asio::buffer(buffer), error);
            }
            if (error != asio::error::eof) {
                throw std::system_error(error, "TLS session ticket read failed");
            }
        }
        session.reset(SSL_get1_session(stream.native_handle()));
        if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
            throw std::runtime_error("TLS session is not resumable");
        }
        // A session whose SSL is freed without a local shutdown is marked
        // unresumable; record a clean local close instead.
        SSL_set_shutdown(stream.native_handle(), SSL_SENT_SHUTDOWN);
    }
    return false;
}

}  // namespace tls_paths_test

using namespace tls_paths_test;

// Every TLS file (certificate chain, encrypted private key, client CA bundle
// and the SNI identity) lives under a path that no ANSI code page can encode.
// Startup prepares the staged credentials off-worker and cleanses its password
// copy; the same listener must serve HTTPS on a second run without listen().
// Each run also negotiates ALPN: any HTTP version the server speaks when
// offered (RFC 7301 §3.2), a fatal alert otherwise. TLS 1.1 negotiates RSA/ECDSA
// ECDHE+AES-CBC and HTTP/1.x only; TLS 1.0 is refused. TLS 1.2 stays ECDHE+AEAD
// in the server's order (RFC 9325 §4.1-4.2.1). Every supported version enforces
// the outgoing record budget and isolates resumed sessions by server_name.
TEST(Ruvia, https_serves_credentials_from_non_ascii_paths_across_runs) {
    // The SNI identity reuses this certificate, so it must also name sni.test
    // (RFC 9110 §7.4: other hosts get 421).
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,DNS:sni.test");
    ruvia::test::tls_identity elliptic_identity("ecdsa.test", "DNS:ecdsa.test", true);
    const non_ascii_directory directory;
    const auto certificate = directory.file(u8"\u8bc1\u4e66.pem");
    const auto private_key = directory.file(u8"\u79c1\u94a5-\U0001F511.pem");
    const auto client_ca = directory.file(u8"\u4fe1\u4efb-\u0434\u043e\u0432\u0435\u0440\u0438\u0435.pem");
    const std::pmr::string password{"correct horse battery staple", ruvia::process_resource()};
    std::filesystem::copy_file(identity.ca_file_, certificate);
    std::filesystem::copy_file(identity.ca_file_, client_ca);
    write_file(private_key, encrypted_private_key(identity, password));
    auto empty_client = empty_subject_client(identity, client_ca);

    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();

    auto& app = ruvia::app();
    const auto listener = [&](std::string_view key_password, std::filesystem::path chain) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1",
            .https_ = endpoint.port(),
            .tls_ =
                {
                    .identity_ =
                        {
                            .certificate_chain_file_ = std::move(chain),
                            .private_key_file_ = private_key,
                            .private_key_password_ = std::pmr::string(key_password, ruvia::process_resource()),
                        },
                    .client_certificates_ = {.ca_file_ = client_ca},
                    .sni_ = {
                        {
                            .host_ = "sni.test",
                            .identity_ =
                                {
                                    .certificate_chain_file_ = certificate,
                                    .private_key_file_ = private_key,
                                    .private_key_password_ = password,
                                },
                        },
                        {
                            .host_ = "ecdsa.test",
                            .identity_ =
                                {
                                    .certificate_chain_file_ = elliptic_identity.ca_file_,
                                    .private_key_file_ = elliptic_identity.private_key_file(),
                                },
                        },
                    },
                    .max_outgoing_records_ = 32,
                },
        };
    };
    // Invalid credentials never replace the application's staged listeners.
    app.server({.worker_count_ = 1});
    EXPECT_TRUE(throws_system_error([&] { app.listen(listener("wrong password", certificate)); }));
    EXPECT_TRUE(throws_system_error([&] { app.listen(listener(password, directory.file(u8"\u7f3a\u5931.pem"))); }));

    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1}).listen(listener(password, certificate)).on_start([] {
        started.release();
    });
    for (int round = 0; round < 2; ++round) {
        ruvia::test::app_run runner(app);
        const bool ready = started.try_acquire_for(10s);
        EXPECT_TRUE(ready);
        if (ready) {
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "sni.test", false)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                "/tls", nullptr, TLS1_1_VERSION)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, elliptic_identity.ca_file_, "ecdsa.test", true,
                "/tls", nullptr, TLS1_1_VERSION)));
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA", TLS1_VERSION).empty());
            for (const char* cipher : {"ECDHE-RSA-AES128-SHA", "ECDHE-RSA-AES256-SHA"}) {
                EXPECT_EQ(selected_cipher(io, endpoint, cipher, TLS1_1_VERSION),
                    std::pmr::string(cipher, ruvia::process_resource()));
            }
            for (const char* cipher : {"ECDHE-ECDSA-AES128-SHA", "ECDHE-ECDSA-AES256-SHA"}) {
                EXPECT_EQ(selected_cipher(io, endpoint, cipher, TLS1_1_VERSION, "ecdsa.test"),
                    std::pmr::string(cipher, ruvia::process_resource()));
            }
            const auto legacy_alpn = negotiate_alpn(io, endpoint, "\x02h2\x08http/1.1", TLS1_1_VERSION);
            EXPECT_FALSE(legacy_alpn.error_);
            EXPECT_EQ(legacy_alpn.selected_, std::pmr::string("http/1.1", ruvia::process_resource()));
            EXPECT_TRUE(refuses_with_no_application_protocol(negotiate_alpn(io, endpoint, "\x02h2", TLS1_1_VERSION)));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer"),
                "absent"));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer", identity.context_.native_handle()),
                "CN=localhost"));
            EXPECT_TRUE(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true,
                                               "/tls-peer", empty_client.native_handle()),
                "present-empty"));
            EXPECT_TRUE(refuses_with_no_application_protocol(negotiate_alpn(io, endpoint, "\x04h2xx")));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x02h2").selected_, std::pmr::string("h2", ruvia::process_resource()));
            const auto negotiated = negotiate_alpn(io, endpoint, "\x02h2\x08http/1.1");
            EXPECT_TRUE(!negotiated.error_);
            EXPECT_EQ(negotiated.selected_, std::pmr::string("h2", ruvia::process_resource()));
            EXPECT_TRUE(truncated_close_delimited_stream_lacks_close_notify(io, endpoint));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x08http/1.0").selected_, std::pmr::string("http/1.0", ruvia::process_resource()));
            EXPECT_EQ(negotiate_alpn(io, endpoint, "\x08http/1.0\x08http/1.1").selected_, std::pmr::string("http/1.1", ruvia::process_resource()));
            EXPECT_TRUE(selected_cipher(io, endpoint, "AES128-SHA").empty());
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA").empty());
            EXPECT_TRUE(selected_cipher(io, endpoint, "ECDHE-ECDSA-AES128-SHA", TLS1_2_VERSION, "ecdsa.test").empty());
            EXPECT_EQ(selected_cipher(io, endpoint, "AES128-SHA:ECDHE-RSA-AES128-GCM-SHA256"),
                std::pmr::string("ECDHE-RSA-AES128-GCM-SHA256", ruvia::process_resource()));
            EXPECT_EQ(selected_cipher(io, endpoint, "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES128-GCM-SHA256"),
                std::pmr::string("ECDHE-RSA-AES128-GCM-SHA256", ruvia::process_resource()));
            EXPECT_EQ(selected_cipher(io, endpoint, "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256"),
                std::pmr::string("ECDHE-RSA-AES128-GCM-SHA256", ruvia::process_resource()));
            for (const int version : {TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION}) {
                EXPECT_TRUE(bounded_tls_connection_ends(io, endpoint, version));
                EXPECT_TRUE(resumes_tls_session_under(io, endpoint, version, "localhost", "localhost"));
                EXPECT_TRUE(resumes_tls_session_under(io, endpoint, version, "sni.test", "SNI.test"));
                EXPECT_TRUE(!resumes_tls_session_under(io, endpoint, version, "localhost", "sni.test"));
                EXPECT_TRUE(!resumes_tls_session_under(io, endpoint, version, "localhost", "unknown.test"));
            }
        }
        app.stop();
        runner.join();
    }
}
