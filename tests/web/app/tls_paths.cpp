#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

namespace tls_paths_test {

using namespace std::chrono_literals;

class tls_path_controller final {
public:
    RUVIA_ROUTES_BEGIN(tls_path_controller)
    RUVIA_GET("/tls", secure);
    RUVIA_GET("/tls-truncated", truncated);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> secure(ruvia::context& context) {
        co_return context.text(std::string_view("secure"));
    }

    // Commits the response, then fails before the body is complete.
    ruvia::task<void> truncated(ruvia::context& context) {
        co_await context.stream_text().write("partial");
        throw std::runtime_error("upstream failed");
    }
};

// A directory whose name no legacy ANSI code page can represent: CJK,
// Cyrillic and a character outside the Basic Multilingual Plane.
class non_ascii_directory final {
public:
    non_ascii_directory() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                             std::filesystem::path(u8"ruvia-tls-\u8bc1\u4e66-\u0442\u0435\u0441\u0442-\U0001F512-");
            candidate += std::to_string(random());
            if (std::filesystem::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create non-ASCII TLS test directory");
    }

    ~non_ascii_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    non_ascii_directory(const non_ascii_directory&) = delete;
    non_ascii_directory& operator=(const non_ascii_directory&) = delete;

    [[nodiscard]] std::filesystem::path file(std::u8string_view name) const {
        return path_ / std::filesystem::path(name);
    }

private:
    std::filesystem::path path_;
};

// Written through the native path API, independent of the server's loader.
void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("cannot write TLS test file");
    }
}

std::string encrypted_private_key(ruvia::test::tls_identity& identity, std::string_view password) {
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
    return std::string(data, static_cast<std::size_t>(size));
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

// Verifies the server chain against the test CA; `host` is both the SNI name
// and, when `verify_host` is set, the name the certificate must match.
std::string https_get(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    const std::filesystem::path& ca_file, const char* host, bool verify_host) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(ca_file.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), host) != 1 ||
        (verify_host && SSL_set1_host(stream.native_handle(), host) != 1)) {
        return {};
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    const auto request = std::string("GET /tls HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n";
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

bool serves_secure_body(const std::string& response) {
    const auto body_start = response.find("\r\n\r\n");
    return response.starts_with("HTTP/1.1 200 ") && body_start != std::string::npos &&
           std::string_view(response).substr(body_start + 4) == "secure";
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
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response.starts_with("HTTP/1.0 200 ") && response.ends_with("\r\n\r\npartial") &&
           error == asio::ssl::error::stream_truncated;
}

struct alpn_handshake final {
    asio::error_code error_;
    std::string selected_;
};

// Handshakes offering `protocols` (ALPN wire format) and reports the protocol
// the server selected.
alpn_handshake negotiate_alpn(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view protocols) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
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

}  // namespace tls_paths_test

using namespace tls_paths_test;

// Every TLS file (certificate chain, encrypted private key, client CA bundle
// and the SNI identity) lives under a path that no ANSI code page can encode.
// listen() loads them once and cleanses its password copy, so the same
// listener must still serve HTTPS on a second run without another listen().
// Each run also negotiates ALPN: http/1.1 when offered, a fatal alert otherwise.
RUVIA_TEST(https_serves_credentials_from_non_ascii_paths_across_runs) {
    ruvia::test::tls_identity identity("localhost");
    const non_ascii_directory directory;
    const auto certificate = directory.file(u8"\u8bc1\u4e66.pem");
    const auto private_key = directory.file(u8"\u79c1\u94a5-\U0001F511.pem");
    const auto client_ca = directory.file(u8"\u4fe1\u4efb-\u0434\u043e\u0432\u0435\u0440\u0438\u0435.pem");
    const std::string password = "correct horse battery staple";
    std::filesystem::copy_file(identity.ca_file_, certificate);
    std::filesystem::copy_file(identity.ca_file_, client_ca);
    write_file(private_key, encrypted_private_key(identity, password));

    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    auto& app = ruvia::app();
    const auto listener = [&](std::string key_password, std::filesystem::path chain) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1",
            .https_ = endpoint.port(),
            .tls_ =
                {
                    .certificate_chain_file_ = std::move(chain),
                    .private_key_file_ = private_key,
                    .private_key_password_ = std::move(key_password),
                    .client_certificates_ = {.verify_file_ = client_ca},
                    .sni_ = {{
                        .host_ = "sni.test",
                        .certificate_chain_file_ = certificate,
                        .private_key_file_ = private_key,
                        .private_key_password_ = password,
                    }},
                },
        };
    };
    // Credentials load at listen(): unreadable files and wrong passwords fail there.
    RUVIA_CHECK(throws_system_error([&] { app.listen(listener("wrong password", certificate)); }));
    RUVIA_CHECK(throws_system_error(
        [&] { app.listen(listener(password, directory.file(u8"\u7f3a\u5931.pem"))); }));

    std::binary_semaphore started{0};
    app.server({.worker_count_ = 1}).listen(listener(password, certificate)).on_start([&started] {
        started.release();
    });
    for (int round = 0; round < 2; ++round) {
        std::exception_ptr failure;
        std::thread runner([&app, &failure] {
            try {
                app.run();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        const bool ready = started.try_acquire_for(10s);
        RUVIA_CHECK(ready);
        if (ready) {
            RUVIA_CHECK(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "localhost", true)));
            RUVIA_CHECK(serves_secure_body(https_get(io, endpoint, identity.ca_file_, "sni.test", false)));
            RUVIA_CHECK(refuses_with_no_application_protocol(negotiate_alpn(io, endpoint, "\x04h2xx")));
            RUVIA_CHECK_EQ(negotiate_alpn(io, endpoint, "\x02h2").selected_, std::string("h2"));
            const auto negotiated = negotiate_alpn(io, endpoint, "\x02h2\x08http/1.1");
            RUVIA_CHECK(!negotiated.error_);
            RUVIA_CHECK_EQ(negotiated.selected_, std::string("h2"));
            RUVIA_CHECK(truncated_close_delimited_stream_lacks_close_notify(io, endpoint));
        }
        app.stop();
        runner.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}
