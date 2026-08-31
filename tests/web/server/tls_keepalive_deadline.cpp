// Regression: the TLS handshake's initial-read deadline must not outlive the
// handshake. handleSession registered a ConnectionScanner entry for the TLS
// handshake (phase kReadingInitial, governed by requestHeaderTimeout). If that
// entry stays registered across the session -- the bug fixed alongside this
// test -- the scanner force-closes the connection one requestHeaderTimeout after
// the handshake regardless of what the session is doing, because the entry is
// frozen at kReadingInitial and never refreshed while the session runs under
// its own entry.
//
// The isolating scenario is a slow-but-valid body upload: while the body
// streams in, the session's own entry sits in kReadingPayload governed by the
// long requestBodyTimeout, so only a leaked handshake entry (short
// requestHeaderTimeout) can close the connection. The upload deliberately spans
// several requestHeaderTimeouts; with the handshake deadline released it must
// complete and be echoed back.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

using namespace std::chrono_literals;

// A short handshake/header deadline against a long body deadline: the upload
// below outlives requestHeaderTimeout many times over while staying far under
// requestBodyTimeout, so a close can only come from a leaked handshake entry.
constexpr auto kRequestHeaderTimeout = 300ms;
constexpr auto kRequestBodyTimeout = 30s;
constexpr int kBodyChunks = 6;
constexpr auto kChunkGap = 150ms;  // 6 * 150ms = 900ms upload, 3x the header timeout

struct SelfSignedPem {
    std::string cert;
    std::string key;
};

// Ephemeral RSA-2048 self-signed cert + key, PEM into memory (frees everything
// so the test process releases all server resources).
SelfSignedPem makeSelfSignedPem() {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    SelfSignedPem out;
    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, x509);
    char* certData = nullptr;
    const long certLen = BIO_get_mem_data(certBio, &certData);
    out.cert.assign(certData, static_cast<std::size_t>(certLen));
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* keyData = nullptr;
    const long keyLen = BIO_get_mem_data(keyBio, &keyData);
    out.key.assign(keyData, static_cast<std::size_t>(keyLen));

    BIO_free(certBio);
    BIO_free(keyBio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return out;
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

// Read one complete HTTP/1.1 response (header block + Content-Length body) into
// `carry`, leaving any surplus bytes for a later call. Returns false if the peer
// closed the connection before a full response arrived.
bool readResponse(TlsStream& stream, std::string& carry, std::error_code& ec) {
    while (carry.find("\r\n\r\n") == std::string_view::npos) {
        char buffer[1024];
        const auto n = stream.read_some(asio::buffer(buffer), ec);
        if (ec) {
            return false;
        }
        carry.append(buffer, n);
    }
    const auto headerEnd = carry.find("\r\n\r\n") + 4;
    std::string headers = carry.substr(0, headerEnd);
    for (auto& c : headers) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::size_t contentLength = 0;
    if (const auto pos = headers.find("content-length:"); pos != std::string::npos) {
        std::size_t cursor = pos + std::string_view("content-length:").size();
        while (cursor < headers.size() && headers[cursor] == ' ') {
            ++cursor;
        }
        while (cursor < headers.size() && headers[cursor] >= '0' && headers[cursor] <= '9') {
            contentLength = contentLength * 10 + static_cast<std::size_t>(headers[cursor] - '0');
            ++cursor;
        }
    }
    const auto needed = headerEnd + contentLength;
    while (carry.size() < needed) {
        char buffer[1024];
        const auto n = stream.read_some(asio::buffer(buffer), ec);
        if (ec) {
            return false;
        }
        carry.append(buffer, n);
    }
    return true;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    const auto pem = makeSelfSignedPem();
    const auto dir = fs::temp_directory_path() / "ruvia_tls_handshake_deadline";
    std::error_code dirEc;
    fs::remove_all(dir, dirEc);
    fs::create_directories(dir, dirEc);
    const auto certPath = dir / "cert.pem";
    const auto keyPath = dir / "key.pem";
    writeFile(certPath, pem.cert);
    writeFile(keyPath, pem.key);

    ruvia::detail::Router router;
    auto& routerImpl = ruvia::detail::RouterImpl::from(router);
    // Consuming the body drives the kReadingPayload phase (requestBodyTimeout),
    // then echoes it so the client can confirm the whole upload was received.
    auto handler = [](ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
        const auto body = co_await context.req().text();
        co_return context.text(body);
    };
    routerImpl.registerRoute(ruvia::HttpKnownMethod::kPost,
        std::pmr::string("/", std::pmr::get_default_resource()),
        ruvia::detail::makeCallableRef<ruvia::HttpResponse, ruvia::Context&>(handler),
        ruvia::detail::RequestBodyMode::kBuffered, {}, {});
    routerImpl.finalize();

    ruvia::detail::HttpServerOptions options;
    ruvia::detail::HttpServerListenerDefinition::Tls tls;
    tls.identity.certificateChainFile =
        std::pmr::string(certPath.string(), std::pmr::get_default_resource());
    tls.identity.privateKeyFile =
        std::pmr::string(keyPath.string(), std::pmr::get_default_resource());
    options.requestHeaderTimeout = kRequestHeaderTimeout;
    options.requestBodyTimeout = kRequestBodyTimeout;
    options.scanInterval = 50ms;  // fire a leaked handshake deadline promptly

    ruvia::detail::WebWorkerRuntime server(
        ruvia::detail::HttpServerListenerDefinition(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), std::move(tls)),
        routerImpl.routeTable(), {}, std::move(options));
    server.start();
    const auto endpoint = server.localEndpoint();

    int result = 0;
    {
        asio::io_context clientContext;
        asio::ssl::context sslContext(asio::ssl::context::tls_client);
        sslContext.set_verify_mode(asio::ssl::verify_none);
        TlsStream stream(clientContext, sslContext);

        std::error_code ec;
        stream.lowest_layer().connect(endpoint, ec);
        if (!ec) {
            stream.handshake(asio::ssl::stream_base::client, ec);
        }
        if (ec) {
            std::fputs("TLS handshake failed\n", stderr);
            result = 1;
        }

        if (result == 0) {
            const std::string head = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                                     std::to_string(kBodyChunks) + "\r\n\r\n";
            asio::write(stream, asio::buffer(head), ec);

            // Trickle the body across several requestHeaderTimeouts. A leaked
            // handshake deadline closes the socket partway through, so a write
            // (or the response read) fails.
            bool uploadOk = !ec;
            for (int i = 0; i < kBodyChunks && uploadOk; ++i) {
                std::this_thread::sleep_for(kChunkGap);
                asio::write(stream, asio::buffer(std::string_view("x", 1)), ec);
                uploadOk = !ec;
            }

            std::string carry;
            if (!uploadOk || !readResponse(stream, carry, ec)) {
                std::fputs(
                    "slow TLS body upload was cut off: the handshake initial-read "
                    "deadline leaked into the session and closed an active upload\n",
                    stderr);
                result = 2;
            }
        }

        std::error_code ignored;
        stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        stream.lowest_layer().close(ignored);
    }

    server.stop();
    server.join();
    fs::remove_all(dir, dirEc);
    return result;
}

